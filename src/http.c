#include "transport.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include "photos.h"
#ifdef __PSP__
#include "psp_dns.h"
#endif
#if defined(__PSP__) || defined(ST_MBEDTLS_TEST)
#include "psp_tls.h"
#include <mbedtls/ssl.h>
#include <time.h>
/* Cached TLS contexts can outlive a request's stack. There is one serial
 * worker, so the observer has process lifetime and is reset before each use. */
static StTlsDiag tls_observation;
static CURLcode tls_context(CURL *handle, void *config, void *ctx) {
    (void)handle;(void)ctx;st_psp_tls_begin();st_stage("TLS handshake");
    mbedtls_ssl_conf_verify(config,st_tls_observe,&tls_observation);
    return CURLE_OK;
}
#endif
void st_curl_error(StError *err,const char *prefix,CURLcode code,const char *detail,const StTlsDiag *diag) {
#ifdef __PSP__
    if(code==CURLE_COULDNT_RESOLVE_HOST&&st_psp_dns_stats().result==ST_DNS_TIMEOUT)detail="DNS lookup timed out";
#endif
#if defined(__PSP__) || defined(ST_MBEDTLS_TEST)
    diag=&tls_observation;
    if(st_psp_tls_stats().expired){code=CURLE_OPERATION_TIMEDOUT;detail="TLS handshake deadline exceeded";}
#endif
    st_tls_error(err,prefix,code,detail,diag);
}

typedef struct { char *data; size_t length,limit; } Buffer;
static size_t receive(void *data, size_t size, size_t count, void *ctx) {
    st_stage("HTTP body");
    Buffer *b=ctx;
    if (size && count>ST_BODY_MAX/size) return 0;
    size_t n=size*count;
    if (n>b->limit-b->length) return 0;
    char *p=realloc(b->data,b->length+n+1);
    if (!p) return 0;
    b->data=p; memcpy(p+b->length,data,n); b->length+=n; p[b->length]=0;
    return n;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) {
    (void)ctx;(void)a;(void)b;(void)d;(void)e; return st_cancelled();
}
/* Curl debug records can contain credentials and complete message bodies.
 * Consume every record, inspect only its type, and never retain/output data. */
static int transfer_stage(CURL *handle,curl_infotype type,char *data,size_t size,void *ctx) {
    (void)handle;(void)data;(void)size;(void)ctx;
    st_transfer_observe(type,size);
    if(type==CURLINFO_HEADER_OUT)st_stage("HTTP send");
    else if(type==CURLINFO_DATA_OUT)st_stage("HTTP upload");
    else if(type==CURLINFO_HEADER_IN)st_stage("HTTP headers");
    return 0;
}
CURL *st_curl_new(const StConfig *config, StTlsDiag *diag, StError *err) {
    memset(diag,0,sizeof(*diag));
#if defined(__PSP__) || defined(ST_MBEDTLS_TEST)
    memset(&tls_observation,0,sizeof(tls_observation));
    st_psp_tls_begin();
    if (time(NULL)==(time_t)-1) {
        st_fail(err,"PSP RTC clock unavailable; TLS blocked");
        return NULL;
    }
#endif
    CURL *h=curl_easy_init();
    if (!h) { st_fail(err,"curl initialization failed"); return NULL; }
    curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,15L);
    curl_easy_setopt(h,CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
#ifdef __PSP__
    st_psp_transport_options(h);
#endif
    curl_easy_setopt(h,CURLOPT_TIMEOUT,40L);
    curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
    /* CAINFO does not clear the build-time CAPATH. The PSP curl build inherits
     * /etc/ssl/certs; mbedTLS fails on that absent directory even with a valid
     * bundled CA file. Require our explicit trust settings to be accepted. */
#define TLS_OPTION(option,value) do { \
    CURLcode code=curl_easy_setopt(h,option,value); \
    if(code!=CURLE_OK) { \
        snprintf(err->text,sizeof(err->text),"TLS setup %s: %s",#option,curl_easy_strerror(code)); \
        curl_easy_cleanup(h);return NULL; \
    } \
} while(0)
    TLS_OPTION(CURLOPT_SSL_VERIFYPEER,1L);
    TLS_OPTION(CURLOPT_SSL_VERIFYHOST,2L);
    TLS_OPTION(CURLOPT_CAINFO,config->ca_file);
    TLS_OPTION(CURLOPT_CAPATH,(char *)NULL);
    TLS_OPTION(CURLOPT_SSLVERSION,(long)CURL_SSLVERSION_TLSv1_2);
#if defined(__PSP__) || defined(ST_MBEDTLS_TEST)
    /* curl 7.64.1 omits error text for e.g. FUTURE/BAD_MD/BAD_KEY flags.
     * Observe mbedTLS's verifier; keep all trust and hostname checks intact. */
    TLS_OPTION(CURLOPT_SSL_CTX_FUNCTION,tls_context);
    TLS_OPTION(CURLOPT_SSL_CTX_DATA,NULL);
#endif
#undef TLS_OPTION
    curl_easy_setopt(h,CURLOPT_PROTOCOLS,(long)CURLPROTO_HTTPS);
    curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(h,CURLOPT_PROXY,"");
    curl_easy_setopt(h,CURLOPT_USERAGENT,ST_APP_NAME "/" ST_APP_VERSION);
    curl_easy_setopt(h,CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
    if(curl_easy_setopt(h,CURLOPT_DEBUGFUNCTION,transfer_stage)!=CURLE_OK){curl_easy_cleanup(h);st_fail(err,"Cannot install private transfer observer");return NULL;}
    return h;
}
static int header_safe(const char *s);
static int api_request(StClient *c,const char *method,const char *path,cJSON *body,const char *key,const char *ticket,cJSON **out,StError *err) {
    memset(err,0,sizeof(*err)); *out=NULL;
    if (*path!='/' || strchr(path,'\r') || strchr(path,'\n')) return st_fail(err,"Invalid API path");
    char url[1400];
    if (snprintf(url,sizeof(url),"%s%s",c->config.api_url,path)>=(int)sizeof(url)) return st_fail(err,"API URL too long");
    StTlsDiag diag;
    CURL *h=st_curl_new(&c->config,&diag,err); if (!h) return 0;
    if(ticket&&(*ticket==0||strlen(ticket)>255||!header_safe(ticket))){curl_easy_cleanup(h);return st_fail(err,"Invalid reauthentication ticket");}
    char error[CURL_ERROR_SIZE]={0}, auth[300], idem[100],mfa[300];
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"Content-Type: application/json");
    headers=curl_slist_append(headers,"Accept: application/json");
    headers=curl_slist_append(headers,"Expect:");
    if (c->token[0]) {
        snprintf(auth,sizeof(auth),"X-Session-Token: %s",c->token);
        headers=curl_slist_append(headers,auth);
    }
    if (key && *key) { snprintf(idem,sizeof(idem),"Idempotency-Key: %s",key); headers=curl_slist_append(headers,idem); }
    if(ticket){snprintf(mfa,sizeof(mfa),"X-MFA-Ticket: %s",ticket);headers=curl_slist_append(headers,mfa);}
    char *payload=body?cJSON_PrintUnformatted(body):NULL;
    if (body && !payload) { curl_slist_free_all(headers); curl_easy_cleanup(h); return st_fail(err,"Out of memory"); }
    Buffer b={.limit=ST_BODY_MAX};
    curl_easy_setopt(h,CURLOPT_URL,url);
    curl_easy_setopt(h,CURLOPT_CUSTOMREQUEST,method);
    /* Curl may replay a request on a stale reused connection. Mutations use
     * a disposable owner after the parked HTTPS read pool has been released. */
    int once=strcmp(method,"GET")!=0;
    if(once){curl_easy_setopt(h,CURLOPT_FRESH_CONNECT,1L);curl_easy_setopt(h,CURLOPT_FORBID_REUSE,1L);}
    curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(h,CURLOPT_ERRORBUFFER,error);
    if (payload) curl_easy_setopt(h,CURLOPT_POSTFIELDS,payload);
    else if (!strcmp(method,"POST") || !strcmp(method,"PUT")) curl_easy_setopt(h,CURLOPT_POSTFIELDS,"");
    st_stage("HTTP / TLS");
    CURLM *owner=NULL;CURLcode result=once?st_curl_perform_once(h,&owner,st_millis()+40000):st_curl_perform(h,&owner,st_millis()+40000);
    curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&err->status);
    if (payload) { st_wipe(payload,strlen(payload)); cJSON_free(payload); }
    st_curl_free(h,owner);curl_slist_free_all(headers);st_wipe(auth,sizeof(auth));st_wipe(mfa,sizeof(mfa));
    int good=0;
    if (result!=CURLE_OK) {
        st_curl_error(err,"Network",result,error,&diag);st_transport_reset();
    } else {
        cJSON *json=NULL;
        if (b.length) json=cJSON_ParseWithLengthOpts(b.data,b.length+1,NULL,1);
        if (err->status>=200 && err->status<300) {
            if (b.length && !json) st_fail(err,"Invalid JSON response");
            else { *out=json; json=NULL; good=1; }
        } else {
            const char *type=st_str(json,"type");
            snprintf(err->text,sizeof(err->text),"HTTP %ld: %.170s",err->status,*type?type:"Request rejected");
            if (err->status==401 && !ticket && strcmp(path,"/auth/session/login") && strcmp(path,"/auth/mfa/ticket")) st_wipe(c->token,sizeof(c->token));
        }
        cJSON_Delete(json);
    }
    if (b.data) { st_wipe(b.data,b.length); free(b.data); }
    return good;
}
int st_http(StClient *c,const char *method,const char *path,cJSON *body,const char *key,cJSON **out,StError *err){return api_request(c,method,path,body,key,NULL,out,err);}
int st_http_ticket(StClient *c,const char *method,const char *path,cJSON *body,const char *ticket,cJSON **out,StError *err){return api_request(c,method,path,body,NULL,ticket,out,err);}

/* Stream directly from the card. Never buffer an entire photo in PSP RAM.
 * Only the discovered Autumn endpoint receives authentication; no redirects. */
int st_upload_photo(StClient *c,const char *path,char id[64],StError *err) {
    uint64_t bytes;const char *mime;char host[512],part[700],url[1100];
    memset(err,0,sizeof(*err));id[0]=0;
    if(!c->token[0]||!st_url_split(c->assets_url,"https://",host,sizeof(host),part,sizeof(part))||strpbrk(c->assets_url,"?#"))return st_fail(err,"No secure upload endpoint");
    if(!st_photo_info(path,&bytes,&mime,err))return 0;
    size_t n=strlen(c->assets_url);while(n&&c->assets_url[n-1]=='/')n--;
    if(snprintf(url,sizeof(url),"%.*s/attachments",(int)n,c->assets_url)>=(int)sizeof(url))return st_fail(err,"Upload URL too long");
    StTlsDiag diag;CURL *h=st_curl_new(&c->config,&diag,err);if(!h)return 0;
    curl_mime *form=curl_mime_init(h);curl_mimepart *file=form?curl_mime_addpart(form):NULL;
    const char *name=strrchr(path,'/');name=name?name+1:path;
    if(!file||strpbrk(name,"\r\n")||curl_mime_name(file,"file")||curl_mime_filedata(file,path)||curl_mime_filename(file,name)||curl_mime_type(file,mime)){
        curl_mime_free(form);curl_easy_cleanup(h);return st_fail(err,"Cannot prepare photo upload");
    }
    char auth[300],detail[CURL_ERROR_SIZE]={0};snprintf(auth,sizeof(auth),"X-Session-Token: %s",c->token);
    struct curl_slist *headers=NULL;headers=curl_slist_append(headers,auth);headers=curl_slist_append(headers,"Expect:");headers=curl_slist_append(headers,"Accept: application/json");
    Buffer b={.limit=8192};CURLM *owner=NULL;
    curl_easy_setopt(h,CURLOPT_URL,url);curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(h,CURLOPT_MIMEPOST,form);
    curl_easy_setopt(h,CURLOPT_FRESH_CONNECT,1L);curl_easy_setopt(h,CURLOPT_FORBID_REUSE,1L);
    curl_easy_setopt(h,CURLOPT_TIMEOUT,120L);curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);curl_easy_setopt(h,CURLOPT_ERRORBUFFER,detail);
    st_stage("Photo upload");CURLcode code=st_curl_perform_once(h,&owner,st_millis()+120000);curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&err->status);
    st_curl_free(h,owner);curl_mime_free(form);curl_slist_free_all(headers);st_wipe(auth,sizeof(auth));
    int ok=0;cJSON *r=b.data?cJSON_ParseWithLengthOpts(b.data,b.length+1,NULL,1):NULL;
    if(code!=CURLE_OK){st_curl_error(err,"Upload",code,detail,&diag);st_transport_reset();}
    else if(err->status>=200&&err->status<300){const char *value=st_str(r,"id");size_t len=strlen(value);if(len&&len<64&&strspn(value,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")==len){st_copy(id,64,value);ok=1;}else st_fail(err,"Invalid upload response");}
    else {snprintf(err->text,sizeof(err->text),"Upload HTTP %ld: %.150s",err->status,st_str(r,"type"));if(err->status==401)st_wipe(c->token,sizeof(c->token));}
    cJSON_Delete(r);free(b.data);return ok;
}

/* Media is a separate unauthenticated request: never forward the API bearer. */
static int header_safe(const char *s) {
    for(;*s;s++)if((unsigned char)*s<32 || (unsigned char)*s>126)return 0;
    return 1;
}
static size_t media_header(char *line,size_t size,size_t count,void *ctx) {
    if(size && count>SIZE_MAX/size)return 0;
    size_t n=size*count;StHeaders *h=ctx;
    if(n>=5 && !memcmp(line,"HTTP/",5)){memset(h,0,sizeof(*h));h->max_age=ST_STATIC_TTL;return n;}
    char text[1024];if(n>=sizeof(text))return n;
    memcpy(text,line,n);text[n]=0;
    char *colon=strchr(text,':');if(!colon)return n;*colon++=0;
    while(*colon==' '||*colon=='\t')colon++;
    size_t len=strlen(colon);while(len && (colon[len-1]=='\r'||colon[len-1]=='\n'||colon[len-1]==' '))colon[--len]=0;
    if(!header_safe(colon))return n;
    if(!strcasecmp(text,"ETag") && len<sizeof(h->etag))st_copy(h->etag,sizeof(h->etag),colon);
    if(!strcasecmp(text,"Last-Modified") && len<sizeof(h->modified))st_copy(h->modified,sizeof(h->modified),colon);
    if(!strcasecmp(text,"Cache-Control")) {
        h->cache_control=1;
        for(char *p=colon;*p;p++)*p=(char)tolower((unsigned char)*p);
        if(strstr(colon,"no-store"))h->no_store=1;
        if(strstr(colon,"no-cache"))h->no_cache=1;
        if(h->no_cache)h->max_age=0;
        else {char *age=strstr(colon,"max-age=");if(age){unsigned long v=strtoul(age+8,NULL,10);h->max_age=v>ST_STATIC_TTL?ST_STATIC_TTL:(unsigned)v;}}
    }
    return n;
}
int st_download_conditional(const StConfig *config,const char *url,const StHeaders *validators,StHeaders *response,unsigned char **out,size_t *length,size_t limit,unsigned timeout,StError *err) {
    *out=NULL;*length=0;memset(err,0,sizeof(*err));char host[512],path[700];
    if(!limit||limit>ST_BODY_MAX||!st_url_split(url,"https://",host,sizeof(host),path,sizeof(path)))return st_fail(err,"Invalid media request");
    StTlsDiag diag;CURL *h=st_curl_new(config,&diag,err);if(!h)return 0;
    char text[CURL_ERROR_SIZE]={0};Buffer b={.limit=limit};CURLM *owner=NULL;
    memset(response,0,sizeof(*response));response->max_age=ST_STATIC_TTL;
    struct curl_slist *headers=NULL;char conditional[320];
    if(validators)headers=curl_slist_append(headers,"Cache-Control: no-cache");
    if(validators && validators->etag[0] && header_safe(validators->etag)) {
        snprintf(conditional,sizeof(conditional),"If-None-Match: %s",validators->etag);headers=curl_slist_append(headers,conditional);
    } else if(validators && validators->modified[0] && header_safe(validators->modified)) {
        snprintf(conditional,sizeof(conditional),"If-Modified-Since: %s",validators->modified);headers=curl_slist_append(headers,conditional);
    }
    /* Autumn redirects animated emoji previews to their original filename.
     * Only unauthenticated media follows a bounded HTTPS-only redirect chain. */
    curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(h,CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);
    curl_easy_setopt(h,CURLOPT_MAXREDIRS,3L);
    curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,media_header);curl_easy_setopt(h,CURLOPT_HEADERDATA,response);
    curl_easy_setopt(h,CURLOPT_URL,url);curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);curl_easy_setopt(h,CURLOPT_ERRORBUFFER,text);
    CURLcode result=st_curl_perform(h,&owner,st_millis()+timeout);
    curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&err->status);st_curl_free(h,owner);curl_slist_free_all(headers);
    int ok=result==CURLE_OK&&((err->status==200&&b.length)||(err->status==304&&validators));
    if(ok){*out=(unsigned char *)b.data;*length=b.length;}else {
        free(b.data);if(result!=CURLE_OK){st_curl_error(err,"Image",result,text,&diag);st_transport_reset();}else st_fail(err,"Image unavailable or redirected");
    }
    return ok;
}

int st_download(const StConfig *config,const char *url,unsigned char **out,size_t *length,size_t limit,unsigned timeout,StError *err) {
    StHeaders response;return st_download_conditional(config,url,NULL,&response,out,length,limit,timeout,err);
}

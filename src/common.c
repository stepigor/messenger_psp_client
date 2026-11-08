#include "stoat.h"
#include "disk_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum StPresence st_presence_value(const char *value) {
    if(value){
        if(!strcmp(value,"Idle"))return ST_IDLE;
        if(!strcmp(value,"Focus"))return ST_FOCUS;
        if(!strcmp(value,"Busy"))return ST_BUSY;
        if(!strcmp(value,"Invisible"))return ST_INVISIBLE;
    }
    return ST_ONLINE;
}
const char *st_presence_label(enum StPresence value) {
    static const char *names[]={"Online","Idle","Focus","Do not disturb","Invisible","Offline"};
    return names[value>=ST_ONLINE&&value<=ST_OFFLINE?value:ST_OFFLINE];
}
void st_copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strlen(src ? src : "");
    if (n >= cap) n = cap - 1;
    /* Do not leave a partial UTF-8 sequence when truncating. */
    if (src && n < strlen(src)) while (n && (((unsigned char)src[n] & 0xc0) == 0x80)) --n;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}
void st_wipe(void *p, size_t n) { volatile unsigned char *b = p; while (n--) *b++ = 0; }
const char *st_str(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
int st_fail(StError *err, const char *text) { st_copy(err->text, sizeof(err->text), text); return 0; }

int st_url_split(const char *url, const char *scheme, char *host, size_t hc, char *path, size_t pc) {
    size_t sl = strlen(scheme);
    if (strncmp(url, scheme, sl)) return 0;
    const char *start = url + sl, *end = strchr(start, '/');
    if (!end) end = start + strlen(start);
    size_t n = (size_t)(end-start);
    if (!n || n >= hc || strchr(url, '#') || strchr(url, '@')) return 0;
    for (const char *p = url; *p; ++p) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '\\') return 0;
    for (const char *p = start; p < end; ++p) if (*p == '?' || *p == '%') return 0;
    memcpy(host, start, n); host[n] = 0;
    if (strlen(*end ? end : "/") >= pc) return 0;
    st_copy(path, pc, *end ? end : "/");
    return 1;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
int st_config_load_assets(const char *path,const char *base,StConfig *c,StError *err) {
    memset(c, 0, sizeof(*c)); c->page_size = 25;
    st_copy(c->ca_file, sizeof(c->ca_file), "cacert.pem");
    st_copy(c->font_file, sizeof(c->font_file), "NotoSans-Regular.ttf");
    st_copy(c->emoji_file,sizeof(c->emoji_file),"emoji.pack");
    FILE *f = fopen(path, "rb");
    if (!f) return st_fail(err, "Cannot open config.ini");
    char line[1100]; int good = 1;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) { good = 0; break; }
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '='); if (!eq) { good = 0; break; }
        *eq++ = 0; char *k = trim(s), *v = trim(eq);
        char *dst = NULL; size_t cap = 0;
        if (!strcmp(k,"api_url")) { dst=c->api_url; cap=sizeof(c->api_url); }
        else if (!strcmp(k,"ca_file")) { dst=c->ca_file; cap=sizeof(c->ca_file); }
        else if (!strcmp(k,"font_file")) { dst=c->font_file; cap=sizeof(c->font_file); }
        else if (!strcmp(k,"page_size")) {
            char *end; long n = strtol(v, &end, 10);
            if (*end || n < 1 || n > 50) { good=0; break; }
            c->page_size=(int)n; continue;
        } else { good=0; break; }
        if (!*v || strlen(v) >= cap) { good=0; break; }
        st_copy(dst,cap,v);
    }
    if (ferror(f)) good=0;
    fclose(f);
    char host[512], part[512];
    if (!good || !st_url_split(c->api_url,"https://",host,sizeof(host),part,sizeof(part)) || strchr(c->api_url,'?'))
        return st_fail(err,"Invalid config: HTTPS URL and known keys required");
    size_t n = strlen(c->api_url);
    while (n && c->api_url[n-1]=='/') c->api_url[--n]=0;
    /* Asset paths belong to the config file's directory, not the host's cwd. */
    const char *slash=strrchr(path,'/');
    if(base||slash) {
        char *assets[]={c->ca_file,c->font_file,c->emoji_file};
        for(size_t i=0;i<3;i++)if(assets[i][0]!='/'&&!strchr(assets[i],':')) {
            size_t prefix=base?strlen(base)+1:(size_t)(slash-path)+1,length=strlen(assets[i]);
            if(prefix+length>=sizeof(c->ca_file))return st_fail(err,"Config asset path too long");
            char full[512];if(base){memcpy(full,base,prefix-1);full[prefix-1]='/';}else memcpy(full,path,prefix);
            memcpy(full+prefix,assets[i],length+1);st_copy(assets[i],512,full);
        }
    }
    return 1;
}
int st_config_load(const char *path,StConfig *c,StError *err){return st_config_load_assets(path,NULL,c,err);}
uint32_t st_utf8_next(const char **text) {
    const unsigned char *p=(const unsigned char *)*text;
    if (!*p) return 0;
    unsigned lead=*p++; uint32_t cp; int n;
    if (lead<128) { *text=(const char *)p; return lead; }
    if (lead>=0xc2 && lead<=0xdf) { cp=lead&31; n=1; }
    else if (lead>=0xe0 && lead<=0xef) { cp=lead&15; n=2; }
    else if (lead>=0xf0 && lead<=0xf4) { cp=lead&7; n=3; }
    else { *text=(const char *)p; return 0xfffd; }
    for (int i=0;i<n;i++) {
        if ((*p&0xc0)!=0x80) { *text=(const char *)p; return 0xfffd; }
        cp=(cp<<6)|(*p++&63);
    }
    *text=(const char *)p;
    if ((n==1 && cp<128)||(n==2 && cp<2048)||(n==3 && cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) return 0xfffd;
    return cp;
}
size_t st_utf8_to_utf16(const char *text, uint16_t *out, size_t cap) {
    size_t n=0; if (!cap) return 0;
    while (*text) {
        uint32_t cp=st_utf8_next(&text);
        size_t need=cp>65535?2:1;
        if (n+need>=cap) break;
        if (cp>65535) { cp-=65536; out[n++]=(uint16_t)(0xd800+(cp>>10)); out[n++]=(uint16_t)(0xdc00+(cp&1023)); }
        else out[n++]=(uint16_t)cp;
    }
    out[n]=0; return n;
}
int st_utf16_to_utf8(const uint16_t *text, char *out, size_t cap) {
    size_t n=0;
    while (*text) {
        uint32_t cp=*text++;
        if (cp>=0xd800 && cp<=0xdbff) {
            if (*text<0xdc00 || *text>0xdfff) return 0;
            cp=65536+((cp-0xd800)<<10)+(*text++-0xdc00);
        } else if (cp>=0xdc00 && cp<=0xdfff) return 0;
        int k=cp<128?1:cp<2048?2:cp<65536?3:4;
        if (n+(size_t)k>=cap) return 0;
        if (k==1) out[n++]=(char)cp;
        else {
            out[n++]=(char)((k==2?0xc0:k==3?0xe0:0xf0)|(cp>>(6*(k-1))));
            for (int j=k-2;j>=0;j--) out[n++]=(char)(0x80|((cp>>(6*j))&63));
        }
    }
    if (n>=cap) return 0;
    out[n]=0; return 1;
}
int st_utf8_count(const char *text, size_t *count) {
    *count=0;
    while (*text) {
        const char *start=text;
        if (st_utf8_next(&text)==0xfffd && strncmp(start,"\xef\xbf\xbd",3)) return 0;
        ++*count;
    }
    return 1;
}

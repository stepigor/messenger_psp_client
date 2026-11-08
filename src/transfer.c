#include "transport.h"
#include <limits.h>
#include <string.h>

static CURLM *https_pool;
static StTransferStats last_transfer;
StTransferStats st_transfer_stats(void){return last_transfer;}
static void add_bytes(unsigned *total,size_t size){*total=size>UINT_MAX-*total?UINT_MAX:*total+(unsigned)size;}
void st_transfer_observe(curl_infotype type,size_t size) {
    if(type==CURLINFO_HEADER_OUT||type==CURLINFO_DATA_OUT)add_bytes(&last_transfer.request_bytes,size);
    if(type==CURLINFO_DATA_OUT)add_bytes(&last_transfer.upload_bytes,size);
    if(type==CURLINFO_HEADER_IN)add_bytes(&last_transfer.header_bytes,size);
}
void st_transport_reset(void) {
    if(https_pool){curl_multi_cleanup(https_pool);https_pool=NULL;}
}
/* HTTPS requests share at most two idle connections. CONNECT_ONLY WSS owns
 * its separate multi until send/recv is done and never enters that pool. */
static CURLcode perform(CURL *h,CURLM **owner,uint64_t deadline,int private_owner) {
    if(private_owner)*owner=curl_multi_init();
    else {
        if(!https_pool) {
            https_pool=curl_multi_init();
            if(https_pool&&(curl_multi_setopt(https_pool,CURLMOPT_MAXCONNECTS,2L)!=CURLM_OK||
               curl_multi_setopt(https_pool,CURLMOPT_MAX_TOTAL_CONNECTIONS,2L)!=CURLM_OK)){st_transport_reset();return CURLE_FAILED_INIT;}
        }
        *owner=https_pool;
    }
    if(!*owner)return CURLE_OUT_OF_MEMORY;
    /* 7.64.1 copies VERBOSE, but not DEBUGFUNCTION, into the multi's hidden
     * cleanup handle. Add silently, then enable our private observer only on h. */
    if(curl_easy_setopt(h,CURLOPT_VERBOSE,0L)!=CURLE_OK)return CURLE_FAILED_INIT;
    if(curl_multi_add_handle(*owner,h)!=CURLM_OK)return CURLE_FAILED_INIT;
    if(curl_easy_setopt(h,CURLOPT_VERBOSE,1L)!=CURLE_OK)return CURLE_FAILED_INIT;
    for(;;) {
        if(st_cancelled())return CURLE_ABORTED_BY_CALLBACK;
        if(st_millis()>=deadline)return CURLE_OPERATION_TIMEDOUT;
        int running=0;CURLMcode r=curl_multi_perform(*owner,&running);
        if(r!=CURLM_OK&&r!=CURLM_CALL_MULTI_PERFORM)return CURLE_RECV_ERROR;
        int count;CURLMsg *message;
        while((message=curl_multi_info_read(*owner,&count)))
            if(message->msg==CURLMSG_DONE&&message->easy_handle==h)return message->data.result;
        if(!running)return CURLE_RECV_ERROR;
        st_pause();
    }
}
static unsigned bounded(double value){return value<=0?0:value>UINT_MAX?UINT_MAX:(unsigned)value;}
static CURLcode measured(CURL *h,CURLM **owner,uint64_t deadline,int private_owner) {
    memset(&last_transfer,0,sizeof(last_transfer));uint64_t started=st_millis();CURLcode result=perform(h,owner,deadline,private_owner);
    last_transfer.code=(int)result;double value=0;long count=0;
#define TIME(field,info) do{value=0;curl_easy_getinfo(h,info,&value);last_transfer.field=bounded(value*1000);}while(0)
    TIME(connect_ms,CURLINFO_CONNECT_TIME);TIME(tls_ms,CURLINFO_APPCONNECT_TIME);TIME(ready_ms,CURLINFO_PRETRANSFER_TIME);
    TIME(first_byte_ms,CURLINFO_STARTTRANSFER_TIME);TIME(total_ms,CURLINFO_TOTAL_TIME);
#undef TIME
    unsigned elapsed=bounded((double)(st_millis()-started));if(elapsed>last_transfer.total_ms)last_transfer.total_ms=elapsed;
    curl_easy_getinfo(h,CURLINFO_NUM_CONNECTS,&count);last_transfer.new_connections=bounded((double)count);
    return result;
}
CURLcode st_curl_perform(CURL *h,CURLM **owner,uint64_t deadline){return measured(h,owner,deadline,0);}
CURLcode st_curl_perform_once(CURL *h,CURLM **owner,uint64_t deadline){st_transport_reset();return measured(h,owner,deadline,1);}
CURLcode st_curl_connect(CURL *h,CURLM **owner,uint64_t deadline){return measured(h,owner,deadline,1);}
void st_curl_free(CURL *h,CURLM *owner) {
    if(owner)curl_multi_remove_handle(owner,h);
    curl_easy_cleanup(h);
    if(owner&&owner!=https_pool)curl_multi_cleanup(owner);
}

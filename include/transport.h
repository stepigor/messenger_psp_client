#ifndef ST_TRANSPORT_H
#define ST_TRANSPORT_H
#include "stoat.h"
#include <sys/select.h>
#include <curl/curl.h>
#include "tls_diag.h"
CURL *st_curl_new(const StConfig *config, StTlsDiag *diag, StError *err);
#define ST_STATIC_TTL (6u*60u*60u)
typedef struct { char etag[256], modified[128]; unsigned max_age; int no_store, cache_control, no_cache; } StHeaders;
int st_download_conditional(const StConfig *config,const char *url,const StHeaders *validators,
                            StHeaders *response,unsigned char **out,size_t *length,size_t limit,unsigned timeout,StError *err);
int st_download(const StConfig *config,const char *url,unsigned char **out,size_t *length,size_t limit,unsigned timeout,StError *err);
CURLcode st_curl_perform(CURL *h, CURLM **owner, uint64_t deadline);
/* A mutation owns a disposable multi and first releases parked HTTPS reads.
 * It cannot borrow/retry an old connection; the WSS owner is independent. */
CURLcode st_curl_perform_once(CURL *h,CURLM **owner,uint64_t deadline);
CURLcode st_curl_connect(CURL *h, CURLM **owner, uint64_t deadline);
void st_curl_free(CURL *h, CURLM *owner);
/* Only the serial network worker may reset its idle HTTPS connection pool. */
void st_transport_reset(void);
typedef struct { int code;unsigned connect_ms,tls_ms,ready_ms,first_byte_ms,total_ms;unsigned request_bytes,upload_bytes,header_bytes,new_connections; } StTransferStats;
StTransferStats st_transfer_stats(void);
void st_transfer_observe(curl_infotype type,size_t size);
void st_curl_error(StError *err,const char *prefix,CURLcode code,const char *detail,const StTlsDiag *diag);
typedef struct { unsigned opened,closed,open_errors,close_errors,live; } StSocketStats;
typedef struct { unsigned calls,errors,last_errno,last_native,maximum_native; } StPollStats;
StPollStats st_psp_poll_stats(void);
#if defined(__PSP__) || defined(ST_NET_TEST) || defined(ST_MAIN_TEST)
void st_psp_transport_options(CURL *h);
StSocketStats st_psp_socket_stats(void);
void st_network_protect(int fd);
void st_network_abort_all(void);
#endif
#endif

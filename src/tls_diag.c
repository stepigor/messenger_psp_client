#include "tls_diag.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void cert_time(char out[20], const mbedtls_x509_time *value) {
    struct tm date={0};
    date.tm_year=value->year-1900; date.tm_mon=value->mon-1;
    date.tm_mday=value->day; date.tm_hour=value->hour;
    date.tm_min=value->min; date.tm_sec=value->sec;
    if (!strftime(out,20,"%Y-%m-%d %H:%M:%S",&date)) strcpy(out,"unavailable");
}
int st_tls_observe(void *ctx, mbedtls_x509_crt *cert, int depth, uint32_t *flags) {
    StTlsDiag *diag=ctx;
    diag->seen=1;
    if (*flags && !diag->flags) {
        diag->depth=depth;
        cert_time(diag->valid_from,&cert->valid_from);
        cert_time(diag->valid_to,&cert->valid_to);
    }
    diag->flags|=*flags;
    /* Returning zero continues normal verification. Never clear *flags. */
    return 0;
}
void st_tls_error(StError *err, const char *prefix, CURLcode code,
                  const char *detail, const StTlsDiag *diag) {
    if (code==CURLE_PEER_FAILED_VERIFICATION && diag->seen) {
        char utc[20]="unavailable", reasons[72]="";
        time_t now=time(NULL); struct tm date;
        if (now!=(time_t)-1 && gmtime_r(&now,&date))
            if (!strftime(utc,sizeof(utc),"%Y-%m-%d %H:%M:%S",&date)) strcpy(utc,"unavailable");
        static const struct { uint32_t flag; const char *name; } names[]={
            {MBEDTLS_X509_BADCERT_FUTURE,"future"},
            {MBEDTLS_X509_BADCERT_EXPIRED,"expired"},
            {MBEDTLS_X509_BADCERT_NOT_TRUSTED,"untrusted"},
            {MBEDTLS_X509_BADCERT_CN_MISMATCH,"hostname"},
            {MBEDTLS_X509_BADCERT_BAD_MD,"bad-hash"},
            {MBEDTLS_X509_BADCERT_BAD_PK,"bad-algorithm"},
            {MBEDTLS_X509_BADCERT_BAD_KEY,"bad-key"}
        };
        size_t used=0;
        for(size_t i=0;i<sizeof(names)/sizeof(*names);i++) {
            if (!(diag->flags & names[i].flag)) continue;
            int n=snprintf(reasons+used,sizeof(reasons)-used,"%s%s",used?",":"",names[i].name);
            if (n<0 || (size_t)n>=sizeof(reasons)-used) break;
            used+=(size_t)n;
        }
        snprintf(err->text,sizeof(err->text),
                 "%s: TLS verify 0x%08lx (%s), depth=%d\nUTC %s\nCert %s .. %s",
                 prefix,(unsigned long)diag->flags,reasons[0]?reasons:"other",diag->depth,
                 utc,diag->valid_from[0]?diag->valid_from:"unavailable",
                 diag->valid_to[0]?diag->valid_to:"unavailable");
    } else {
        snprintf(err->text,sizeof(err->text),"%s: %.210s",prefix,
                 detail && detail[0]?detail:curl_easy_strerror(code));
    }
}

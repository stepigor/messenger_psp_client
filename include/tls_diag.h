#ifndef ST_TLS_DIAG_H
#define ST_TLS_DIAG_H
#include "stoat.h"
#include <sys/select.h>
#include <curl/curl.h>
#include <mbedtls/x509_crt.h>

typedef struct {
    uint32_t flags;
    int seen, depth;
    char valid_from[20], valid_to[20];
} StTlsDiag;

/* Observe the verifier without changing its flags or accepting a certificate. */
int st_tls_observe(void *ctx, mbedtls_x509_crt *cert, int depth, uint32_t *flags);
void st_tls_error(StError *err, const char *prefix, CURLcode code,
                  const char *detail, const StTlsDiag *diag);
#endif

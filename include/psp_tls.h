#ifndef ST_PSP_TLS_H
#define ST_PSP_TLS_H
#include "stoat.h"
typedef struct {
    unsigned steps,phase,max_step_ms,cpu_ms,cpu_valid,reads,writes,received,sent,max_io_ms,expired;
    int result;
} StPspTlsStats;
void st_psp_tls_begin(void);
StPspTlsStats st_psp_tls_stats(void);
#endif

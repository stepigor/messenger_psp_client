#ifndef ST_PSP_DNS_H
#define ST_PSP_DNS_H
/* Only the serial network worker owns DNS sockets and these diagnostics. */
enum StDnsResult { ST_DNS_OK,ST_DNS_TIMEOUT,ST_DNS_CANCELLED,ST_DNS_NETWORK,
    ST_DNS_INVALID,ST_DNS_NOT_FOUND,ST_DNS_NO_SERVER,ST_DNS_REPLY_ERROR };
typedef struct { unsigned queries,replies,tcp,elapsed_ms,opened,closed;enum StDnsResult result; } StDnsStats;
StDnsStats st_psp_dns_stats(void);
#endif

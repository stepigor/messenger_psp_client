/* Private, namespaced mbedTLS. DTLS authenticates the peer fingerprint from
 * verified WSS; HTTPS/WSS always enforce CA, hostname and certificate validity. */
#include "rtc_config.h"
#include "rtc_tls_symbols.h"
#include "rtc_tls.h"
#include <mbedtls/ssl.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct StRtcTls {
    mbedtls_ssl_context ssl;mbedtls_ssl_config config;
    mbedtls_x509_crt ca,certificate;mbedtls_pk_context key;
    int datagram,server,done,exported,authenticated;uint64_t timer;uint32_t intermediate,final;
    unsigned char keys[60],peer[32];char fingerprint[96];
};
static int random_bytes(void *ctx,unsigned char *p,size_t n){(void)ctx;return st_random(p,n)?0:MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;}
static void timer_set(void *ctx,uint32_t intermediate,uint32_t final){StRtcTls *t=ctx;t->timer=st_millis();t->intermediate=intermediate;t->final=final;}
static int timer_get(void *ctx){StRtcTls *t=ctx;if(!t->final)return -1;uint64_t elapsed=st_millis()-t->timer;return elapsed>=t->final?2:elapsed>=t->intermediate?1:0;}
static int exported(void *ctx,const unsigned char *master,const unsigned char *block,size_t mac,size_t key,size_t iv,const unsigned char client[32],const unsigned char server[32],mbedtls_tls_prf_types type) {
    (void)block;(void)mac;(void)key;(void)iv;StRtcTls *t=ctx;unsigned char seed[64];memcpy(seed,client,32);memcpy(seed+32,server,32);
    int result=mbedtls_ssl_tls_prf(type,master,48,"EXTRACTOR-dtls_srtp",seed,sizeof(seed),t->keys,sizeof(t->keys));t->exported=result==0;return result;
}
static int self_certificate(StRtcTls *t) {
    int result=mbedtls_pk_setup(&t->key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if(!result)result=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(t->key),random_bytes,NULL);
    mbedtls_x509write_cert w;mbedtls_x509write_crt_init(&w);mbedtls_mpi serial;mbedtls_mpi_init(&serial);
    if(!result)result=mbedtls_mpi_fill_random(&serial,16,random_bytes,NULL);
    if(!result)result=mbedtls_x509write_crt_set_serial(&w,&serial);
    mbedtls_x509write_crt_set_version(&w,MBEDTLS_X509_CRT_VERSION_3);mbedtls_x509write_crt_set_md_alg(&w,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w,&t->key);mbedtls_x509write_crt_set_issuer_key(&w,&t->key);
    if(!result)result=mbedtls_x509write_crt_set_subject_name(&w,"CN=" ST_APP_NAME);
    if(!result)result=mbedtls_x509write_crt_set_issuer_name(&w,"CN=" ST_APP_NAME);
    if(!result)result=mbedtls_x509write_crt_set_validity(&w,"20200101000000","20500101000000");
    unsigned char pem[4096];if(!result)result=mbedtls_x509write_crt_pem(&w,pem,sizeof(pem),random_bytes,NULL);
    if(!result)result=mbedtls_x509_crt_parse(&t->certificate,pem,strlen((char *)pem)+1);
    st_wipe(pem,sizeof(pem));mbedtls_x509write_crt_free(&w);mbedtls_mpi_free(&serial);
    if(!result){unsigned char hash[32];result=mbedtls_sha256_ret(t->certificate.raw.p,t->certificate.raw.len,hash,0);for(int i=0;i<32;i++)snprintf(t->fingerprint+i*3,sizeof(t->fingerprint)-i*3,i==31?"%02X":"%02X:",hash[i]);}
    return result;
}
StRtcTls *st_rtc_tls_new(int datagram,const char *ca,const char *hostname,StError *err) {
    StRtcTls *t=calloc(1,sizeof(*t));if(!t){st_fail(err,"Not enough memory for voice TLS");return NULL;}
    t->datagram=datagram!=0;t->server=datagram==2;mbedtls_ssl_init(&t->ssl);mbedtls_ssl_config_init(&t->config);mbedtls_x509_crt_init(&t->ca);mbedtls_x509_crt_init(&t->certificate);mbedtls_pk_init(&t->key);
    int r=mbedtls_ssl_config_defaults(&t->config,t->server?MBEDTLS_SSL_IS_SERVER:MBEDTLS_SSL_IS_CLIENT,datagram?MBEDTLS_SSL_TRANSPORT_DATAGRAM:MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_min_version(&t->config,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_rng(&t->config,random_bytes,NULL);
    if(datagram) {
        static const mbedtls_ssl_srtp_profile profiles[]={MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,MBEDTLS_TLS_SRTP_UNSET};
        if(!r)r=self_certificate(t);
        if(!r)r=mbedtls_ssl_conf_own_cert(&t->config,&t->certificate,&t->key);
        if(!r)r=mbedtls_ssl_conf_dtls_srtp_protection_profiles(&t->config,profiles);
        mbedtls_ssl_conf_authmode(&t->config,MBEDTLS_SSL_VERIFY_OPTIONAL);
        if(t->server)mbedtls_ssl_conf_dtls_cookies(&t->config,NULL,NULL,NULL);
        mbedtls_ssl_conf_export_keys_ext_cb(&t->config,exported,t);
    } else {
        if(!r)r=mbedtls_x509_crt_parse_file(&t->ca,ca);
        mbedtls_ssl_conf_ca_chain(&t->config,&t->ca,NULL);mbedtls_ssl_conf_authmode(&t->config,MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    if(!r)r=mbedtls_ssl_setup(&t->ssl,&t->config);
    if(!r&&!datagram)r=mbedtls_ssl_set_hostname(&t->ssl,hostname);
    if(r){snprintf(err->text,sizeof(err->text),"Voice TLS setup failed (0x%04x)",(unsigned)-r);st_rtc_tls_free(t);return NULL;}
    if(datagram){mbedtls_ssl_set_mtu(&t->ssl,1200);mbedtls_ssl_set_timer_cb(&t->ssl,t,timer_set,timer_get);}
    return t;
}
void st_rtc_tls_io(StRtcTls *t,void *ctx,StRtcSend send,StRtcReceive receive){mbedtls_ssl_set_bio(&t->ssl,ctx,send,receive,NULL);}
void st_rtc_tls_free(StRtcTls *t){if(!t)return;mbedtls_ssl_free(&t->ssl);mbedtls_ssl_config_free(&t->config);mbedtls_x509_crt_free(&t->ca);mbedtls_x509_crt_free(&t->certificate);mbedtls_pk_free(&t->key);st_wipe(t,sizeof(*t));free(t);}
int st_rtc_tls_peer(StRtcTls *t,const char *fingerprint) {
    if(strlen(fingerprint)!=95)return 0;
    unsigned char peer[32];
    for(int i=0;i<32;i++) {
        unsigned v=0;
        for(int j=0;j<2;j++){char c=fingerprint[i*3+j];int digit=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;if(digit<0)return 0;v=v*16+(unsigned)digit;}
        if(i<31&&fingerprint[i*3+2]!=':')return 0;peer[i]=(unsigned char)v;
    }
    memcpy(t->peer,peer,sizeof(peer));
    return 1;
}
int st_rtc_tls_step(StRtcTls *t,StError *err) {
    if(t->done)return 1;
    int r=mbedtls_ssl_handshake_step(&t->ssl);
    if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;
    if(r){snprintf(err->text,sizeof(err->text),"Voice %s handshake failed (0x%04x)",t->datagram?"DTLS":"TLS",(unsigned)-r);return -1;}
    if(t->ssl.state!=MBEDTLS_SSL_HANDSHAKE_OVER)return 0;
    if(t->datagram) {
        const mbedtls_x509_crt *peer=mbedtls_ssl_get_peer_cert(&t->ssl);unsigned char hash[32];
        if(!peer||mbedtls_sha256_ret(peer->raw.p,peer->raw.len,hash,0)||memcmp(hash,t->peer,32)||!t->exported){st_fail(err,"Voice DTLS fingerprint verification failed");return -1;}
        mbedtls_dtls_srtp_info info;mbedtls_ssl_get_dtls_srtp_negotiation_result(&t->ssl,&info);
        if(info.chosen_dtls_srtp_profile!=MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80){st_fail(err,"Unsupported voice encryption profile");return -1;}
    } else if(mbedtls_ssl_get_verify_result(&t->ssl)){st_fail(err,"Voice server certificate verification failed");return -1;}
    t->authenticated=t->done=1;return 1;
}
int st_rtc_tls_read(StRtcTls *t,unsigned char *out,size_t n){int r=mbedtls_ssl_read(&t->ssl,out,n);return r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE?-2:r;}
int st_rtc_tls_write(StRtcTls *t,const unsigned char *p,size_t n){int r=mbedtls_ssl_write(&t->ssl,p,n>16384?16384:n);return r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE?-2:r;}
const char *st_rtc_tls_fingerprint(const StRtcTls *t){return t->fingerprint;}
int st_rtc_tls_keys(const StRtcTls *t,unsigned char in[30],unsigned char out[30]) {
    if(!t->authenticated||!t->datagram)return 0;
    unsigned mine=t->server?16:0,peer=t->server?0:16;
    memcpy(out,t->keys+mine,16);memcpy(out+16,t->keys+32+(t->server?14:0),14);
    memcpy(in,t->keys+peer,16);memcpy(in+16,t->keys+32+(t->server?0:14),14);return 1;
}

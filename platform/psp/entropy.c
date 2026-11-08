/* Seed the in-memory DRBG from the installed SDK and personalize it with the
 * firmware-generated encrypted SAVEDATA IV. No external seed files or writes.
 * The SDK source itself is time-seeded MT19937, not a cryptographic RNG. The
 * native IV avoids repeating its stream after restarts within the same second. */
#include "stoat.h"
#include <mbedtls/ctr_drbg.h>
#include <errno.h>
#include <pspkernel.h>
#include <string.h>
static mbedtls_ctr_drbg_context rng;
static int ready,mutex=-1;
int __real__getentropy(void *out,size_t n);
static int sdk_source(void *unused,unsigned char *out,size_t n) {
    (void)unused;return __real__getentropy(out,n);
}
int entropy_init_savedata(const unsigned char nonce[16],StError *err) {
    if(ready)return 1;
    unsigned char personal[sizeof(ST_APP_NAME)-1+16]=ST_APP_NAME;
    memcpy(personal+sizeof(ST_APP_NAME)-1,nonce,16);
    mbedtls_ctr_drbg_init(&rng);mbedtls_ctr_drbg_set_entropy_len(&rng,48);
    int ok=mbedtls_ctr_drbg_seed(&rng,sdk_source,NULL,personal,sizeof(personal))==0;
    st_wipe(personal,sizeof(personal));
    if(!ok){mbedtls_ctr_drbg_free(&rng);return st_fail(err,"SDK random initialization failed");}
    mutex=sceKernelCreateSema("TLS random",0,1,1,NULL);
    if(mutex<0){mbedtls_ctr_drbg_free(&rng);return st_fail(err,"Cannot initialize TLS random lock");}
    ready=1;return 1;
}
int st_random(void *out,size_t n) {
    if(!ready)return 0;
    if(sceKernelWaitSema(mutex,1,NULL)<0)return 0;
    unsigned char *p=out;int ok=1;
    while(n){size_t k=n>256?256:n;if(mbedtls_ctr_drbg_random(&rng,p,k)){ok=0;break;}p+=k;n-=k;}
    sceKernelSignalSema(mutex,1);return ok;
}
/* Linker wrapping lets mbedTLS use our initialized DRBG while its seed callback
 * calls the original libcglue implementation, without recursion. */
int __wrap__getentropy(void *out,size_t n) {
    if(n>256||!st_random(out,n)){errno=EIO;return -1;}return 0;
}
void entropy_term(void){ready=0;mbedtls_ctr_drbg_free(&rng);if(mutex>=0){sceKernelDeleteSema(mutex);mutex=-1;}}

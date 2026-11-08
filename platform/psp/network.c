#include "network.h"
#include <psputility_modules.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <pspsysmem.h>
#include <stdio.h>
#include <string.h>

/* Track only resources acquired by this application; a preloaded module is
 * usable but must not be unloaded as part of our failure or exit cleanup. */
static struct {
    int common_module,inet_module,net,inet,resolver,apctl;
} owned;

void network_term(void) {
    if(owned.apctl){sceNetApctlDisconnect();sceNetApctlTerm();}
    if(owned.resolver)sceNetResolverTerm();
    if(owned.inet)sceNetInetTerm();
    if(owned.net)sceNetTerm();
    if(owned.inet_module)sceUtilityUnloadModule(PSP_MODULE_NET_INET);
    if(owned.common_module)sceUtilityUnloadModule(PSP_MODULE_NET_COMMON);
    memset(&owned,0,sizeof(owned));
}

static int network_error(StError *err,const char *stage,int result) {
    char text[256];
    snprintf(text,sizeof(text),"Network initialization failed:\n%s\nCode: 0x%08X\n"
             "Free: %u KiB\nLargest block: %u KiB",
             stage,(unsigned)result,(unsigned)sceKernelTotalFreeMemSize()/1024,
             (unsigned)sceKernelMaxFreeMemSize()/1024);
    /* Capture the failing stage and memory before releasing partial resources. */
    network_term();
    return st_fail(err,text);
}

static int load_module(int module,const char *name,int *acquired,StError *err) {
    int result=sceUtilityLoadModule(module);
    if((uint32_t)result==SCE_ERROR_MODULE_ALREADY_LOADED)return 1;
    if(result<0)return network_error(err,name,result);
    *acquired=1;
    return 1;
}

int network_init(StError *err) {
    if(owned.apctl)return 1;
    if(!load_module(PSP_MODULE_NET_COMMON,"sceUtilityLoadModule(NET_COMMON)",&owned.common_module,err))return 0;
    if(!load_module(PSP_MODULE_NET_INET,"sceUtilityLoadModule(NET_INET)",&owned.inet_module,err))return 0;
    int result=sceNetInit(512*1024,42,16*1024,42,16*1024);
    if(result<0)return network_error(err,"sceNetInit",result);
    owned.net=1;
    result=sceNetInetInit();
    if(result<0)return network_error(err,"sceNetInetInit",result);
    owned.inet=1;
    result=sceNetResolverInit();
    if(result<0)return network_error(err,"sceNetResolverInit",result);
    owned.resolver=1;
    result=sceNetApctlInit(0x8000,48);
    if(result<0)return network_error(err,"sceNetApctlInit",result);
    owned.apctl=1;
    return 1;
}

#include "wifi.h"
#include "ui_draw.h"
#include <pspwlan.h>
#include <pspnet_apctl.h>
#include <psputility_netparam.h>
#include <string.h>
static int status(StWifi *w,enum StWifiState state,const char *notice) {
    w->state=state;st_copy(w->notice,sizeof(w->notice),notice);return state==ST_WIFI_READY;
}
void wifi_init(StWifi *w){memset(w,0,sizeof(*w));w->was_on=-1;w->state=ST_WIFI_IDLE;}
static void remember_profile(StWifi *w) {
    w->profile=0;
    union SceNetApctlInfo name,ssid;memset(&name,0,sizeof(name));memset(&ssid,0,sizeof(ssid));
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME,&name)<0||sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID,&ssid)<0)return;
    name.name[sizeof(name.name)-1]=0;
    /* Read only name/SSID from firmware. No credentials are copied or stored. */
    for(int i=1;i<=100;i++)if(sceUtilityCheckNetParam(i)==0) {
        netData data;if(sceUtilityGetNetParam(i,PSP_NETPARAM_NAME,&data)<0)continue;
        data.asString[sizeof(data.asString)-1]=0;if(strcmp(data.asString,name.name))continue;
        if(sceUtilityGetNetParam(i,PSP_NETPARAM_SSID,&data)<0)continue;
        data.asString[sizeof(data.asString)-1]=0;
        size_t n=strlen(data.asString);if(n<=sizeof(ssid.ssid)&&!memcmp(data.asString,ssid.ssid,n)&&
           (n==sizeof(ssid.ssid)||ssid.ssid[n]==0)){w->profile=i;return;}
    }
}
void wifi_retry(StWifi *w) {
    w->reset=1;w->attempts=w->joining=0;w->deadline=w->retry=0;
    status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
}
void wifi_dialog_result(StWifi *w,int connected) {
    w->reset=w->joining=0;w->deadline=w->retry=0;w->attempts=connected?0:2;
    if(connected){remember_profile(w);status(w,ST_WIFI_READY,"");}
    else status(w,ST_WIFI_FAILED,"Wi-Fi disconnected. " UI_KEY_SQUARE " choose connection.");
}
int wifi_poll(StWifi *w,uint64_t now,int idle) {
    int on=sceWlanGetSwitchState()>0,state=0;
    int ok=sceNetApctlGetState(&state)>=0;
    if(!on) {
        if(w->was_on!=0){w->reset=1;w->joining=w->attempts=0;w->deadline=w->retry=0;}
        w->was_on=0;
        if(idle&&w->reset==1&&ok){if(state!=PSP_NET_APCTL_STATE_DISCONNECTED)sceNetApctlDisconnect();w->reset=2;}
        return status(w,ST_WIFI_OFF,"WLAN is off. Turn on the switch.");
    }
    w->was_on=1;
    if(!ok)return status(w,ST_WIFI_FAILED,"Cannot read Wi-Fi status. " UI_KEY_SQUARE " choose connection.");
    if(w->reset) {
        if(!idle)return status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
        if(state==PSP_NET_APCTL_STATE_DISCONNECTED){w->reset=0;w->deadline=0;}
        else {
            if(w->reset==1){sceNetApctlDisconnect();w->reset=2;}
            if(!w->deadline)w->deadline=now+5000;
            if(now>=w->deadline)return status(w,ST_WIFI_FAILED,"Wi-Fi reset timed out. " UI_KEY_SQUARE " choose connection.");
            return status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
        }
    }
    if(state==PSP_NET_APCTL_STATE_GOT_IP) {
        if(w->state!=ST_WIFI_READY)remember_profile(w);
        w->attempts=w->joining=0;w->deadline=w->retry=0;return status(w,ST_WIFI_READY,"");
    }
    if(w->state==ST_WIFI_READY){w->attempts=w->joining=0;w->deadline=w->retry=0;}
    if(w->joining||state!=PSP_NET_APCTL_STATE_DISCONNECTED) {
        if(!w->deadline)w->deadline=now+20000;
        if(now<w->deadline)return status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
        w->joining=0;w->reset=1;w->deadline=0;w->retry=now+3000;
        return status(w,ST_WIFI_CONNECTING,"Wi-Fi timed out. Retrying...");
    }
    if(w->attempts>=2)return status(w,ST_WIFI_FAILED,"Wi-Fi reconnect failed. " UI_KEY_SQUARE " choose connection.");
    if(!w->profile)return status(w,ST_WIFI_IDLE,"Wi-Fi disconnected. " UI_KEY_SQUARE " choose connection.");
    if(!idle||now<w->retry)return status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
    w->attempts++;
    if(sceNetApctlConnect(w->profile)<0){w->retry=now+3000;return status(w,ST_WIFI_CONNECTING,"Wi-Fi connect failed. Retrying...");}
    w->joining=1;w->deadline=now+20000;return status(w,ST_WIFI_CONNECTING,"Reconnecting Wi-Fi...");
}

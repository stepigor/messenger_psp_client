#ifndef ST_WIFI_H
#define ST_WIFI_H
#include "stoat.h"
enum StWifiState { ST_WIFI_OFF,ST_WIFI_IDLE,ST_WIFI_CONNECTING,ST_WIFI_READY,ST_WIFI_FAILED };
typedef struct {
    enum StWifiState state;
    int profile,was_on,reset,attempts,joining;
    uint64_t deadline,retry;
    char notice[128];
} StWifi;
void wifi_init(StWifi *wifi);
/* Read status every frame; connection control is allowed only with no job in flight. */
int wifi_poll(StWifi *wifi,uint64_t now,int idle);
void wifi_retry(StWifi *wifi);
void wifi_dialog_result(StWifi *wifi,int connected);
#endif

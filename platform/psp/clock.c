/* PSP libc's sceKernelLibcGettimeofday path returned a 1970 date on ARK-4.
 * mbedTLS calls time() for certificate validity. Use the calendar RTC in UTC
 * for that symbol, without changing curl's elapsed-time/gettimeofday path. */
#include <psprtc.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <psputility_sysparam.h>

_Static_assert(sizeof(time_t)==8,"The installed PSP libraries use 64-bit time_t");

int st_local_offset_minutes(void) {
    static int loaded,offset;
    if(!loaded) {
        int zone=0,dst=0;
        if(sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_TIMEZONE,&zone)>=0&&zone>=-720&&zone<=840) {
            if(sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS,&dst)<0)dst=0;
            offset=zone+(dst==1?60:0);if(offset>840)offset=840;
        }
        loaded=1;
    }
    return offset;
}

time_t time(time_t *out) {
    const ScePspDateTime epoch={1970,1,1,0,0,0,0};
    ScePspDateTime utc={0};
    u64 now_ticks=0,epoch_ticks=0;
    time_t result=(time_t)-1;
    /* tz=0 explicitly asks for UTC, independent of the XMB local timezone.
     * Avoid sceRtcGetTime_t: its firmware output has a 32-bit ABI, unlike the
     * installed newlib's time_t. Tick pointers use the firmware's u64 ABI. */
    if (sceRtcGetCurrentClock(&utc,0)<0 || sceRtcCheckValid(&utc)<0 ||
        sceRtcGetTick(&utc,&now_ticks)<0 || sceRtcGetTick(&epoch,&epoch_ticks)<0) {
        errno=EIO;
    } else {
        u32 resolution=sceRtcGetTickResolution();
        if (!resolution || now_ticks<epoch_ticks) {
            errno=EINVAL;
        } else {
            u64 seconds=(now_ticks-epoch_ticks)/resolution;
            if (seconds>(u64)INT64_MAX) errno=EOVERFLOW;
            else result=(time_t)seconds;
        }
    }
    if (out) *out=result;
    return result;
}

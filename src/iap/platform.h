#pragma once
#include <stddef.h>
#include <stdint.h>

#include "span.h"
#include "spec/iap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iap_platform_get_play_status */
struct IAPPlatformPlayStatus {
    uint32_t track_total_ms;
    uint32_t track_pos_ms;
    uint32_t track_index;
    uint32_t track_count;
    uint8_t  state; /* IAPIPodStatePlayStatus */
};

/* iap_platform_control */
enum IAPPlatformControl {
    IAPPlatformControl_TogglePlayPause,
    IAPPlatformControl_Play,
    IAPPlatformControl_Pause,
    IAPPlatformControl_Stop,
    IAPPlatformControl_Next,
    IAPPlatformControl_Prev,
};

/* iap_platform_get_volume */
struct IAPPlatformVolumeStatus {
    uint8_t volume;
    IAPBool muted;
};

/* iap_platform_get_power_status */
struct IAPPlatformPowerStatus {
    uint8_t state; /* IAPIPodStatePowerState */
    uint8_t battery_level;
};

/* iap_platform_get_date_time */
struct IAPPlatformTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  seconds;
};

/* iap_platform_get_indexed_track_info */
struct IAPPlatformTrackInfo {
    uint32_t*               track_total_ms;
    struct IAPPlatformTime* release_date;
    struct IAPSpan*         composer;
};

void*   iap_platform_malloc(void* platform, size_t size);
void    iap_platform_free(void* platform, void* ptr);
int     iap_platform_send_hid_report(void* platform, const void* ptr, size_t size);
IAPBool iap_platform_get_ipod_serial_num(void* platform, struct IAPSpan* serial);
IAPBool iap_platform_get_play_status(void* platform, struct IAPPlatformPlayStatus* status);
IAPBool iap_platform_control(void* platform, enum IAPPlatformControl control);
IAPBool iap_platform_get_volume(void* platform, struct IAPPlatformVolumeStatus* status);
IAPBool iap_platform_get_power_status(void* platform, struct IAPPlatformPowerStatus* status);
IAPBool iap_platform_get_shuffle_setting(void* platform, uint8_t* status /* IAPIPodStateShuffleSettingState */);
IAPBool iap_platform_set_shuffle_setting(void* platform, uint8_t status /* IAPIPodStateShuffleSettingState */);
IAPBool iap_platform_get_repeat_setting(void* platform, uint8_t* status /* IAPIPodStateRepeatSettingState */);
IAPBool iap_platform_set_repeat_setting(void* platform, uint8_t status /* IAPIPodStateRepeatSettingState */);
IAPBool iap_platform_get_date_time(void* platform, struct IAPPlatformTime* time);
IAPBool iap_platform_get_backlight_level(void* platform, uint8_t* level);
IAPBool iap_platform_get_hold_switch_state(void* platform, IAPBool* state);
IAPBool iap_platform_get_indexed_track_info(void* platform, uint32_t index, struct IAPPlatformTrackInfo* info);

/* debugging */
void iap_platform_dump_hex(const void* ptr, size_t size);

#ifdef __cplusplus
}
#endif

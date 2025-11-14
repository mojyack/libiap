#include <unistd.h>

#include "iap/platform.h"
#include "macros/assert.hpp"
#include "platform.hpp"
#include "util/hexdump.hpp"

extern "C" {
void* iap_platform_malloc(void* platform, size_t size) {
    (void)platform;
    return malloc(size);
}

void iap_platform_free(void* platform, void* ptr) {
    (void)platform;
    free(ptr);
}

int iap_platform_send_hid_report(void* platform, const void* ptr, size_t size) {
    std::println("====== dev: {} bytes ======", size);
    dump_hex(std::span{(uint8_t*)ptr, size});
    const int fd = ((struct LinuxPlatformData*)platform)->fd;
    return write(fd, ptr, size);
}

IAPBool iap_platform_get_ipod_serial_num(void* platform, struct IAPSpan* serial) {
    (void)platform;
    static const char* serial_num = "000000000000";
    return iap_span_append(serial, serial_num, sizeof(serial_num));
}

IAPBool iap_platform_get_play_status(void* platform, struct IAPPlatformPlayStatus* status) {
    (void)platform;
    status->track_total_ms = 10 * 1000;
    status->track_pos_ms   = 5 * 1000;
    status->track_index    = 0;
    status->track_count    = 8;
    status->state          = IAPIPodStatePlayStatus_PlaybackPaused;
    return iap_true;
}

IAPBool iap_platform_control(void* platform, enum IAPPlatformControl control) {
    (void)platform;
    std::println("control {}", int(control));
    return iap_true;
}

IAPBool iap_platform_get_volume(void* platform, struct IAPPlatformVolumeStatus* status) {
    (void)platform;
    status->volume = 128;
    status->muted  = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_power_status(void* platform, struct IAPPlatformPowerStatus* status) {
    (void)platform;
    status->state         = IAPIPodStatePowerState_Internal;
    status->battery_level = 128;
    return iap_true;
}

IAPBool iap_platform_get_shuffle_setting(void* platform, uint8_t* status) {
    (void)platform;
    *status = IAPIPodStateShuffleSettingState_Off;
    return iap_true;
}

IAPBool iap_platform_set_shuffle_setting(void* platform, uint8_t status) {
    (void)platform;
    (void)status;
    return iap_true;
}

IAPBool iap_platform_get_repeat_setting(void* platform, uint8_t* status) {
    (void)platform;
    *status = IAPIPodStateRepeatSettingState_Off;
    return iap_true;
}

IAPBool iap_platform_set_repeat_setting(void* platform, uint8_t status) {
    (void)platform;
    (void)status;
    return iap_true;
}

IAPBool iap_platform_get_date_time(void* platform, struct IAPPlatformTime* time) {
    (void)platform;
    time->year    = 2025;
    time->month   = 11;
    time->day     = 12;
    time->hour    = 20;
    time->minute  = 27;
    time->seconds = 10;
    return iap_true;
}

IAPBool iap_platform_get_backlight_level(void* platform, uint8_t* level) {
    (void)platform;
    *level = 128;
    return iap_true;
}

IAPBool iap_platform_get_hold_switch_state(void* platform, IAPBool* state) {
    (void)platform;
    *state = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_indexed_track_info(void* platform, uint32_t index, struct IAPPlatformTrackInfo* info) {
    constexpr auto error_value = iap_false;

    (void)index;
    if(info->total_ms != NULL) {
        *info->total_ms = 10 * 1000;
    }
    if(info->release_date != NULL) {
        info->release_date->year    = 2000;
        info->release_date->month   = 0;
        info->release_date->day     = 0;
        info->release_date->hour    = 0;
        info->release_date->minute  = 0;
        info->release_date->seconds = 0;
    }
    if(info->artist != NULL) {
        static const char* str = "(artist)";
        ensure_v(iap_span_append(info->artist, str, sizeof(str)));
    }
    if(info->composer != NULL) {
        static const char* str = "(composer)";
        ensure_v(iap_span_append(info->composer, str, sizeof(str)));
    }
    if(info->album != NULL) {
        static const char* str = "(album)";
        ensure_v(iap_span_append(info->album, str, sizeof(str)));
    }
    if(info->title != NULL) {
        static const char* str = "(title)";
        ensure_v(iap_span_append(info->title, str, sizeof(str)));
    }
    return iap_true;
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    dump_hex(std::span{(uint8_t*)ptr, size});
}
}

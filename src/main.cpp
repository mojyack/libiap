#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "iap/iap.h"
#include "iap/platform.h"
#include "macros/assert.hpp"
#include "util/hexdump.hpp"

// platform implementation
extern "C" {
struct LinuxPlatformData {
    int fd;
};

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

IAPBool iap_platform_get_play_status(void* platform, struct IAPPlatformPlayStatus* status) {
    (void)platform;
    status->track_total_ms = 10 * 1000;
    status->track_pos_ms   = 5 * 1000;
    status->track_index    = 0;
    status->state          = IAPIPodStatePlayStatus_PlaybackPaused;
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
    (void)index;
    if(info->track_total_ms != NULL) {
        *info->track_total_ms = 10 * 1000;
    }
    if(info->release_date != NULL) {
        info->release_date->year    = 2000;
        info->release_date->month   = 0;
        info->release_date->day     = 0;
        info->release_date->hour    = 0;
        info->release_date->minute  = 0;
        info->release_date->seconds = 0;
    }
    if(info->composer != NULL) {
        constexpr auto     error_value = iap_false;
        static const char* composer    = "DUMMY";
        const auto         ptr         = iap_span_alloc(info->composer, sizeof(composer));
        ensure_v(ptr != NULL);
        memcpy(ptr, composer, sizeof(composer));
    }
    return iap_true;
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    dump_hex(std::span{(uint8_t*)ptr, size});
}
}

auto main() -> int {
    auto platform = LinuxPlatformData{
        .fd = open("/dev/iap0", O_RDWR | O_NONBLOCK),
    };
    ensure(platform.fd >= 0);
    auto pfds = std::array{pollfd{.fd = platform.fd}};
    auto ctx  = IAPContext{.platform = &platform};
    ensure(iap_init_ctx(&ctx));
loop:
    pfds[0].events = ctx.send_busy ? POLLIN | POLLOUT : POLLIN;
    ensure(poll(pfds.data(), pfds.size(), -1) >= 0);
    if(pfds[0].revents & POLLOUT) {
        iap_notify_send_complete(&ctx);
    }
    if(pfds[0].revents & POLLIN) {
        auto       buf = std::array<uint8_t, 256>();
        const auto ret = read(platform.fd, buf.data(), buf.size());
        ensure(ret > 0);
        std::println("====== acc: {} bytes ======", ret);
        dump_hex(std::span{buf.data(), size_t(ret)});
        iap_feed_hid_report(&ctx, buf.data(), ret);
    }
    goto loop;
    return 0;
}

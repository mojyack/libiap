#include <unistd.h>

#include "artwork.hpp"
#include "context.hpp"
#include "iap/platform.h"
#include "macros/assert.hpp"
#include "platform-macros.h"
#include "platform.hpp"
#include "util/hexdump.hpp"

extern "C" {
void* iap_platform_malloc(void* platform, size_t size, int flags) {
    (void)platform;
    (void)flags;
    return malloc(size);
}

void iap_platform_free(void* platform, void* ptr) {
    (void)platform;
    free(ptr);
}

int iap_platform_send_hid_report(void* platform, const void* ptr, size_t size) {
    std::println("====== dev: {} bytes ======", size);
    dump_hex(std::span{(uint8_t*)ptr, size});
    const auto fd = ((struct LinuxPlatformData*)platform)->fd;
    return write(fd, ptr, size);
}

IAPBool iap_platform_get_ipod_serial_num(void* platform, struct IAPSpan* serial) {
    (void)platform;
    static const char* serial_num = "000000000000";
    return iap_span_append(serial, serial_num, sizeof(serial_num));
}

IAPBool iap_platform_get_play_status(void* platform, struct IAPPlatformPlayStatus* status) {
    const auto& ctx = ((struct LinuxPlatformData*)platform)->ctx;
    if(ctx.play_state != PlayState::Stopped) {
        const auto& track      = ctx.tracks[ctx.current_track];
        status->track_total_ms = samples_to_ms(track.data.size());
        status->track_pos_ms   = samples_to_ms(ctx.pcm_cursor);
        status->track_index    = ctx.current_track;
        status->track_count    = ctx.tracks.size();
        status->track_caps     = (track.cover.empty() ? 0 : IAPIPodStateTrackCapBits_HasAlbumArts) | IAPIPodStateTrackCapBits_HasReleaseDate;
    }
    constexpr auto state_table = std::array{
        IAPIPodStatePlayStatus_PlaybackStopped,
        IAPIPodStatePlayStatus_Playing,
        IAPIPodStatePlayStatus_PlaybackPaused,
    };
    status->state = state_table[int(ctx.play_state)];
    return iap_true;
}

IAPBool iap_platform_control(void* platform, enum IAPPlatformControl control) {
    constexpr auto error_value = iap_false;

    auto& ctx = ((struct LinuxPlatformData*)platform)->ctx;
    std::println("control {}", int(control));
    switch(control) {
    case IAPPlatformControl_TogglePlayPause: {
        switch(ctx.play_state) {
        case PlayState::Stopped:
            bail_v("play/pause toggle while stopped");
        case PlayState::Playing:
            ensure_v(ctx.set_state(PlayState::Paused));
            break;
        case PlayState::Paused:
            ensure_v(ctx.set_state(PlayState::Playing));
            break;
        }
    } break;
    case IAPPlatformControl_Play: {
        ensure_v(ctx.set_state(PlayState::Paused));
    } break;
    case IAPPlatformControl_Pause: {
        ensure_v(ctx.set_state(PlayState::Paused));
    } break;
    case IAPPlatformControl_Stop: {
        ensure_v(ctx.set_state(PlayState::Stopped));
    } break;
    case IAPPlatformControl_Next: {
        ensure_v(ctx.current_track + 1 < ctx.tracks.size());
        ctx.current_track += 1;
        ctx.pcm_cursor = 0;
    } break;
    case IAPPlatformControl_Prev: {
        ensure_v(ctx.current_track > 1);
        ctx.current_track -= 1;
        ctx.pcm_cursor = 0;
    } break;
    }
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

IAPBool iap_platform_get_date_time(void* platform, struct IAPDateTime* time) {
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

    const auto& ctx = ((struct LinuxPlatformData*)platform)->ctx;
    ensure_v(index < ctx.tracks.size());
    ensure_v(ctx.play_state != PlayState::Stopped);
    const auto& track = ctx.tracks[index];
    if(info->total_ms != NULL) {
        *info->total_ms = samples_to_ms(track.data.size());
    }
    if(info->caps != NULL) {
        *info->caps = (track.cover.empty() ? 0 : IAPIPodStateTrackCapBits_HasAlbumArts) | IAPIPodStateTrackCapBits_HasReleaseDate;
    }
    if(info->release_date != NULL) {
        info->release_date->year    = track.year;
        info->release_date->month   = track.month;
        info->release_date->day     = track.day;
        info->release_date->hour    = 0;
        info->release_date->minute  = 0;
        info->release_date->seconds = 0;
    }
    if(info->artist != NULL) {
        ensure_v(iap_span_append(info->artist, track.artist.data(), track.artist.size() + 1));
    }
    if(info->composer != NULL) {
        ensure_v(iap_span_append(info->composer, track.artist.data(), track.artist.size() + 1));
    }
    if(info->album != NULL) {
        ensure_v(iap_span_append(info->album, track.album.data(), track.album.size() + 1));
    }
    if(info->title != NULL) {
        ensure_v(iap_span_append(info->title, track.title.data(), track.title.size() + 1));
    }
    return iap_true;
}

IAPBool iap_platform_open_artwork(void* platform, uint32_t index, uintptr_t* handle) {
    constexpr auto error_value = iap_false;

    const auto& ctx = ((struct LinuxPlatformData*)platform)->ctx;
    ensure_v(index < ctx.tracks.size());
    const auto& track = ctx.tracks[index];

    *handle = (uintptr_t)decode_blob(track.cover, IAP_ARTWORK_WIDTH, IAP_ARTWORK_WIDTH);
    ensure_v(*handle != 0);

    return iap_true;
}

IAPBool iap_platform_get_artwork_ptr(void* platform, uintptr_t handle, struct IAPSpan* span) {
    (void)platform;
    span->ptr  = (uint8_t*)handle;
    span->size = IAP_ARTWORK_WIDTH * IAP_ARTWORK_HEIGHT * 2;
    return iap_true;
}

IAPBool iap_platform_close_artwork(void* platform, uintptr_t handle) {
    (void)platform;
    delete[](std::byte*)handle;
    return iap_true;
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    dump_hex(std::span{(uint8_t*)ptr, size});
}
}

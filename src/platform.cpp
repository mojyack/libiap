#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "artwork.hpp"
#include "context.hpp"
#include "iap/iap.h"
#include "iap/platform.h"
#include "macros/unwrap.hpp"
#include "platform-macros.h"
#include "util/fd.hpp"
#include "util/hexdump.hpp"

extern "C" {
void* iap_platform_malloc(struct IAPContext* iap_ctx, size_t size, int flags) {
    (void)iap_ctx;
    (void)flags;
    return malloc(size);
}

void iap_platform_free(struct IAPContext* iap_ctx, void* ptr) {
    (void)iap_ctx;
    free(ptr);
}

int iap_platform_send_hid_report(struct IAPContext* iap_ctx, const void* ptr, size_t size) {
    // std::println("====== dev: {} bytes ======", size);
    const auto& ctx = *((struct Context*)iap_ctx->platform);
    // dump_hex(std::span{(uint8_t*)ptr, size});
    return write(ctx.fd, ptr, size);
}

IAPBool iap_platform_get_ipod_serial_num(struct IAPContext* iap_ctx, struct IAPSpan* serial) {
    (void)iap_ctx;
    static const char* serial_num = "000000000000";
    return iap_span_append(serial, serial_num, strlen(serial_num) + 1);
}

enum IAPPlatformUSBSpeed iap_platform_get_usb_speed(struct IAPContext* iap_ctx) {
    (void)iap_ctx;
    static auto cache = IAPPlatformUSBSpeed(-1);
    if(cache < 0) {
        constexpr auto error_value = IAPPlatformUSBSpeed_Full;
        const auto     fd          = FileDescriptor(open("/sys/module/g_ipod_hid/parameters/usb_hs", O_RDONLY));
        ensure_v(fd.as_handle() >= 0);
        unwrap_v(data, fd.read<char>());
        cache = data == 'Y' ? IAPPlatformUSBSpeed_High : IAPPlatformUSBSpeed_Full;
        PRINT("hs: {}", data);
    }
    return cache;
}

IAPBool iap_platform_get_play_status(struct IAPContext* iap_ctx, struct IAPPlatformPlayStatus* status) {
    const auto& ctx = *((struct Context*)iap_ctx->platform);
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

void iap_platform_control(struct IAPContext* iap_ctx, enum IAPPlatformControl control, struct IAPPlatformPendingControl pending) {
    auto& ctx = *((struct Context*)iap_ctx->platform);
    std::println("control {}", int(control));
    auto result = iap_true;
#define error_act result = iap_false;
    switch(control) {
    case IAPPlatformControl_TogglePlayPause: {
        switch(ctx.play_state) {
        case PlayState::Stopped:
            bail_a("play/pause toggle while stopped");
            break;
        case PlayState::Playing:
            ensure_a(ctx.set_state(PlayState::Paused));
            break;
        case PlayState::Paused:
            ensure_a(ctx.set_state(PlayState::Playing));
            break;
        }
    } break;
    case IAPPlatformControl_Play: {
        ensure_a(ctx.set_state(PlayState::Playing));
    } break;
    case IAPPlatformControl_Pause: {
        ensure_a(ctx.set_state(PlayState::Paused));
    } break;
    case IAPPlatformControl_Stop: {
        ensure_a(ctx.set_state(PlayState::Stopped));
    } break;
    case IAPPlatformControl_Next: {
        ensure_a(ctx.skip_track(1));
    } break;
    case IAPPlatformControl_Prev: {
        ensure_a(ctx.skip_track(-1));
    } break;
    }
#undef error_act
    ensure_v(iap_control_response(iap_ctx, pending, result));
    return;
}

IAPBool iap_platform_get_volume(struct IAPContext* iap_ctx, struct IAPPlatformVolumeStatus* status) {
    (void)iap_ctx;
    status->volume = 128;
    status->muted  = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_power_status(struct IAPContext* iap_ctx, struct IAPPlatformPowerStatus* status) {
    (void)iap_ctx;
    status->state         = IAPIPodStatePowerState_Internal;
    status->battery_level = 128;
    return iap_true;
}

IAPBool iap_platform_get_shuffle_setting(struct IAPContext* iap_ctx, uint8_t* status) {
    (void)iap_ctx;
    *status = IAPIPodStateShuffleSettingState_Off;
    return iap_true;
}

IAPBool iap_platform_set_shuffle_setting(struct IAPContext* iap_ctx, uint8_t status) {
    (void)iap_ctx;
    (void)status;
    return iap_true;
}

IAPBool iap_platform_get_repeat_setting(struct IAPContext* iap_ctx, uint8_t* status) {
    (void)iap_ctx;
    *status = IAPIPodStateRepeatSettingState_Off;
    return iap_true;
}

IAPBool iap_platform_set_repeat_setting(struct IAPContext* iap_ctx, uint8_t status) {
    (void)iap_ctx;
    (void)status;
    return iap_true;
}

IAPBool iap_platform_get_date_time(struct IAPContext* iap_ctx, struct IAPDateTime* time) {
    (void)iap_ctx;
    time->year    = 2025;
    time->month   = 11;
    time->day     = 12;
    time->hour    = 20;
    time->minute  = 27;
    time->seconds = 10;
    return iap_true;
}

IAPBool iap_platform_get_backlight_level(struct IAPContext* iap_ctx, uint8_t* level) {
    (void)iap_ctx;
    *level = 128;
    return iap_true;
}

IAPBool iap_platform_get_hold_switch_state(struct IAPContext* iap_ctx, IAPBool* state) {
    (void)iap_ctx;
    *state = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_indexed_track_info(struct IAPContext* iap_ctx, uint32_t index, struct IAPPlatformTrackInfo* info) {
    constexpr auto error_value = iap_false;

    auto& ctx = *((struct Context*)iap_ctx->platform);
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

IAPBool iap_platform_set_playing_track(struct IAPContext* iap_ctx, uint32_t index) {
    constexpr auto error_value = iap_false;

    auto& ctx = *((struct Context*)iap_ctx->platform);
    ensure_v(ctx.skip_track(int(index) - ctx.current_track));

    return iap_true;
}

IAPBool iap_platform_open_artwork(struct IAPContext* iap_ctx, uint32_t index, struct IAPPlatformArtwork* artwork) {
    constexpr auto error_value = iap_false;

    auto& ctx = *((struct Context*)iap_ctx->platform);
    ensure_v(index < ctx.tracks.size());
    const auto& track = ctx.tracks[index];

    artwork->opaque = (uintptr_t)decode_blob(track.cover, IAP_ARTWORK_WIDTH, IAP_ARTWORK_WIDTH);
    ensure_v(artwork->opaque != 0);
    artwork->color  = true;
    artwork->width  = IAP_ARTWORK_WIDTH;
    artwork->height = IAP_ARTWORK_HEIGHT;

    return iap_true;
}

IAPBool iap_platform_get_artwork_ptr(struct IAPContext* iap_ctx, struct IAPPlatformArtwork* artwork, struct IAPSpan* span) {
    (void)iap_ctx;
    span->ptr  = (uint8_t*)artwork->opaque;
    span->size = IAP_ARTWORK_WIDTH * IAP_ARTWORK_HEIGHT * 2;
    return iap_true;
}

IAPBool iap_platform_close_artwork(struct IAPContext* iap_ctx, struct IAPPlatformArtwork* artwork) {
    (void)iap_ctx;
    delete[](std::byte*)artwork->opaque;
    return iap_true;
}

IAPBool iap_platform_on_acc_samprs_received(struct IAPContext* iap_ctx, struct IAPSpan* samprs) {
    constexpr auto error_value = iap_false;

    while(samprs->size > 0) {
        uint32_t sample_rate;
        ensure_v(iap_span_read_32(samprs, &sample_rate));
        if(sample_rate == 44100) {
            ensure_v(iap_select_sampr(iap_ctx, sample_rate));
            return true;
        }
    }
    bail_v("accessory does not support 44100Hz");
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    dump_hex(std::span{(uint8_t*)ptr, size});
}
}

#include "context.h"
#include "endian.h"
#include "iap.h"
#include "macros.h"
#include "spec/iap.h"

void iap_notify_track_time_position(struct IAPContext* ctx, uint32_t pos_ms) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_TrackTimePositionMSec)) {
        ctx->notification_data.track_time_position_ms = pos_ms;
        ctx->notifications |= 1 << IAPIPodStateType_TrackTimePositionMSec;
    }
}

void iap_notify_track_playback_index(struct IAPContext* ctx, uint32_t index) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_TrackPlaybackIndex)) {
        ctx->notification_data.track_playback_index = index;
        ctx->notifications |= 1 << IAPIPodStateType_TrackPlaybackIndex;
    }
}

void iap_notify_track_caps(struct IAPContext* ctx, uint32_t caps) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_TrackCaps)) {
        ctx->notification_data.track_caps = caps;
        ctx->notifications |= 1 << IAPIPodStateType_TrackCaps;
    }
}

void iap_notify_tracks_count(struct IAPContext* ctx, uint32_t count) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_PlaybackEngineContents)) {
        ctx->notification_data.tracks_count = count;
        ctx->notifications |= 1 << IAPIPodStateType_PlaybackEngineContents;
    }
}

void iap_notify_play_status(struct IAPContext* ctx, uint8_t status) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_PlayStatus)) {
        ctx->notification_data.play_status = status;
        ctx->notifications |= 1 << IAPIPodStateType_PlayStatus;
    }
}

void iap_notify_volume(struct IAPContext* ctx, uint8_t volume, IAPBool muted) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_Volume)) {
        ctx->notification_data.volume     = volume;
        ctx->notification_data.mute_state = muted;
        ctx->notifications |= 1 << IAPIPodStateType_Volume;
    }
}

void iap_notify_power_state(struct IAPContext* ctx, uint8_t state, uint8_t battery_level) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_Power)) {
        ctx->notification_data.power_state   = state;
        ctx->notification_data.battery_level = battery_level;
        ctx->notifications |= 1 << IAPIPodStateType_Power;
    }
}

void iap_notify_shuffle_state(struct IAPContext* ctx, uint8_t state) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_ShuffleSetting)) {
        ctx->notification_data.shuffle_state = state;
        ctx->notifications |= 1 << IAPIPodStateType_ShuffleSetting;
    }
}

void iap_notify_repeat_state(struct IAPContext* ctx, uint8_t state) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_RepeatSetting)) {
        ctx->notification_data.repeat_state = state;
        ctx->notifications |= 1 << IAPIPodStateType_RepeatSetting;
    }
}

void iap_notify_time_setting(struct IAPContext* ctx, const struct IAPDateTime* time) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_DateTimeSetting)) {
        ctx->notification_data.time_setting = *time;
        ctx->notifications |= 1 << IAPIPodStateType_DateTimeSetting;
    }
}

void iap_notify_hold_switch_state(struct IAPContext* ctx, uint8_t state) {
    if(ctx->enabled_notifications & (1 << IAPIPodStateType_HoldSwitchState)) {
        ctx->notification_data.hold_switch_state = state;
        ctx->notifications |= 1 << IAPIPodStateType_HoldSwitchState;
    }
}

#define send_notify(PayloadType, StateType, set)                                                                                                                     \
    if(ctx->notifications & (1 << StateType)) {                                                                                                                      \
        struct PayloadType* payload = iap_span_alloc(&request, sizeof(*payload));                                                                                    \
        check_ret(payload != NULL, iap_false);                                                                                                                       \
        payload->type = StateType;                                                                                                                                   \
        set;                                                                                                                                                         \
        check_ret(_iap_send_packet(ctx, IAPLingoID_DisplayRemote, IAPDisplayRemoteCommandID_RemoteEventNotification, (ctx->trans_id += 1), request.ptr), iap_false); \
        ctx->notifications &= ~(1 << StateType);                                                                                                                     \
        return iap_true;                                                                                                                                             \
    }

IAPBool _iap_flush_notification(struct IAPContext* ctx) {
    struct IAPSpan request = _iap_get_buffer_for_send_payload(ctx);
    send_notify(IAPIPodStateTrackTimePositionMSecPayload,
                IAPIPodStateType_TrackTimePositionMSec,
                payload->position_ms = swap_32(ctx->notification_data.track_time_position_ms));
    send_notify(IAPIPodStateTrackPlaybackIndexPayload,
                IAPIPodStateType_TrackPlaybackIndex,
                payload->index = swap_32(ctx->notification_data.track_playback_index));
    send_notify(IAPIPodStateTrackCapsPayload,
                IAPIPodStateType_TrackCaps,
                payload->caps = swap_32(ctx->notification_data.track_caps));
    send_notify(IAPIPodStatePlaybackEngineContentsPayload,
                IAPIPodStateType_TrackCaps,
                payload->count = swap_32(ctx->notification_data.tracks_count));
    send_notify(IAPIPodStatePlayStatusPayload,
                IAPIPodStateType_PlayStatus,
                payload->status = ctx->notification_data.play_status);
    send_notify(IAPIPodStateVolumePayload,
                IAPIPodStateType_Volume,
                payload->mute_state = ctx->notification_data.mute_state;
                payload->ui_volume  = ctx->notification_data.volume);
    send_notify(IAPIPodStatePowerPayload,
                IAPIPodStateType_Power,
                payload->power_state   = ctx->notification_data.power_state;
                payload->battery_level = ctx->notification_data.battery_level);
    send_notify(IAPIPodStateShuffleSettingPayload,
                IAPIPodStateType_ShuffleSetting,
                payload->shuffle_state = ctx->notification_data.shuffle_state);
    send_notify(IAPIPodStateRepeatSettingPayload,
                IAPIPodStateType_RepeatSetting,
                payload->repeat_state = ctx->notification_data.repeat_state);
    send_notify(IAPIPodStateDateTimeSettingPayload,
                IAPIPodStateType_DateTimeSetting,
                payload->year   = swap_16(ctx->notification_data.time_setting.year);
                payload->month  = ctx->notification_data.time_setting.month;
                payload->day    = ctx->notification_data.time_setting.day;
                payload->hour   = ctx->notification_data.time_setting.hour;
                payload->minute = ctx->notification_data.time_setting.minute);

    ctx->flushing_notifications = iap_false;
    return iap_true;
}

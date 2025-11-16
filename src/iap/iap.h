#pragma once
#include "context.h"
#include "span.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iap.c */
IAPBool        iap_init_ctx(struct IAPContext* ctx);
IAPBool        iap_deinit_ctx(struct IAPContext* ctx);
IAPBool        _iap_feed_packet(struct IAPContext* ctx, const uint8_t* data, size_t size);
struct IAPSpan _iap_get_buffer_for_send_payload(struct IAPContext* ctx);
IAPBool        _iap_send_packet(struct IAPContext* ctx, uint8_t lingo, uint16_t command, int32_t trans_id, uint8_t* final_ptr);

/* hid.c */
IAPBool iap_feed_hid_report(struct IAPContext* ctx, const uint8_t* data, size_t size);
IAPBool iap_notify_send_complete(struct IAPContext* ctx);
IAPBool _iap_send_hid_reports(struct IAPContext* ctx, size_t begin, size_t end); /* data is passed by ctx->send_buf */
IAPBool _iap_send_next_report(struct IAPContext* ctx);

/* notification.c */
void iap_notify_track_time_position(struct IAPContext* ctx, uint32_t pos_ms);
void iap_notify_track_playback_index(struct IAPContext* ctx, uint32_t index);
void iap_notify_track_caps(struct IAPContext* ctx, uint32_t caps);
void iap_notify_tracks_count(struct IAPContext* ctx, uint32_t count);
void iap_notify_play_status(struct IAPContext* ctx, uint8_t status);
void iap_notify_volume(struct IAPContext* ctx, uint8_t volume, IAPBool muted);
void iap_notify_power_state(struct IAPContext* ctx, uint8_t state /* IAPIPodStatePowerState */, uint8_t battery_level);
void iap_notify_shuffle_state(struct IAPContext* ctx, uint8_t state /* IAPIPodStateShuffleSettingState */);
void iap_notify_repeat_state(struct IAPContext* ctx, uint8_t state /* IAPIPodStateRepeatSettingState */);
void iap_notify_time_setting(struct IAPContext* ctx, const struct IAPDateTime* time);
void iap_notify_hold_switch_state(struct IAPContext* ctx, uint8_t state);

IAPBool _iap_flush_notification(struct IAPContext* ctx);

#ifdef __cplusplus
}
#endif

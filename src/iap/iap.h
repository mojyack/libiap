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

#ifdef __cplusplus
}
#endif

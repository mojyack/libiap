#pragma once
#include <stddef.h>
#include <stdint.h>

#include "bool.h"

struct IAPContext;
struct IAPSpan;

typedef IAPBool (*IAPOnSendComplete)(struct IAPContext* ctx);
typedef int32_t (*IAPHandler)(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response);

struct IAPContext {
    void* platform; /* opaque to platform functions */

    /* iap_feed_hid_report */
    uint8_t* hid_recv_buf;
    size_t   hid_recv_buf_cursor;
    /* _iap_send_packet, _iap_send_hid_reports */
    uint8_t* send_buf;
    size_t   send_buf_sending_cursor;
    size_t   send_buf_sending_range_begin;
    size_t   send_buf_sending_range_end;
    /* _iap_send_next_report */
    IAPOnSendComplete on_send_complete;
    /* _iap_feed_packet */
    IAPHandler handler_override;
    /* iap.c */
    uint16_t trans_id;
    /* _iap_send_hid_reports */
    uint8_t hid_send_staging_buf[0x3F /* max hid report size */ + 1 /* report id */] __attribute__((aligned(32)));
    IAPBool send_busy;

    uint8_t phase; /* IAPPhase */
};


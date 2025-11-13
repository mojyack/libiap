#include "span.h"
#include "endian.h"
#include "macros.h"

const void* iap_span_read(struct IAPSpan* span, size_t count) {
    check_ret(span->size >= count, NULL);
    span->ptr += count;
    span->size -= count;
    return span->ptr - count;
}

void* iap_span_alloc(struct IAPSpan* span, size_t count) __attribute__((alias("iap_span_read")));

#define iap_span_template(width)                                                  \
    IAPBool iap_span_peek_##width(struct IAPSpan* span, uint##width##_t* value) { \
        check_ret(span->size >= width / 8, iap_false);                            \
        *value = swap_##width(*(uint##width##_t*)span->ptr);                      \
        return iap_true;                                                          \
    }                                                                             \
    IAPBool iap_span_read_##width(struct IAPSpan* span, uint##width##_t* value) { \
        const uint##width##_t* ptr = iap_span_read(span, width / 8);              \
        check_ret(ptr != NULL, iap_false);                                        \
        *value = swap_##width(*ptr);                                              \
        return iap_true;                                                          \
    }                                                                             \
    IAPBool iap_span_write_##width(struct IAPSpan* span, uint##width##_t value) { \
        uint##width##_t* ptr = iap_span_alloc(span, width / 8);                   \
        check_ret(ptr != NULL, iap_false);                                        \
        *ptr = swap_##width(value);                                               \
        return iap_true;                                                          \
    }
iap_span_template(8);
iap_span_template(16);
iap_span_template(32);
iap_span_template(64);
#undef iap_span_template

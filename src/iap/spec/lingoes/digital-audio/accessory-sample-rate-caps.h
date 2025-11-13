#pragma once
#include <stdint.h>

struct IAPRetRetAccessorySampleRateCapsPayload {
    uint32_t sample_rates[];
} __attribute__((packed));

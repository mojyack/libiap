#pragma once
#include <stdint.h>

struct IAPSetCurrentPlayingTrack {
    uint32_t index;
} __attribute__((packed));

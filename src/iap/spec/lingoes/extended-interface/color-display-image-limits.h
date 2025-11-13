#pragma once
#include <stdint.h>

/* [1] P.443 5.1.53 Command 0x003A: ReturnColorDisplayImageLimits */

struct IAPReturnColorDisplayImageLimitsPayload {
    struct {
        uint16_t max_width;
        uint16_t max_height;
        uint8_t  pixel_format; /* IAPArtworkPixelFormats */
    } __attribute__((packed)) limits[];
} __attribute__((packed));

#pragma once
#include <stdint.h>

inline auto swap(uint8_t num) -> uint8_t {
    return num;
}

inline auto swap(uint16_t num) -> uint16_t {
    return (num >> 8) | (num << 8);
}

inline auto swap(uint32_t num) -> uint32_t {
    return (num & 0xFF000000) >> 24 |
           (num & 0x00FF0000) >> 8 |
           (num & 0x0000FF00) << 8 |
           (num & 0x000000FF) << 24;
}

inline auto swap(uint64_t num) -> uint64_t {
    return (num & 0xFF00000000000000) >> 56 |
           (num & 0x00FF000000000000) >> 40 |
           (num & 0x0000FF0000000000) << 24 |
           (num & 0x000000FF00000000) << 8 |
           (num & 0x000000000FF00000) << 8 |
           (num & 0x00000000000FF000) << 24 |
           (num & 0x000000000000FF00) << 40 |
           (num & 0x00000000000000FF) << 56;
}

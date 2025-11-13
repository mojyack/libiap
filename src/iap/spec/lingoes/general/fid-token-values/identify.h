#pragma once
#include <stdint.h>

struct IAPFIDTokenValuesIdentifyToken {
    uint8_t length;
    uint8_t type;    /* = 0x00 */
    uint8_t subtype; /* = 0x00 */
    uint8_t num_lingoes;
    /*uint8_t lingoes[num_lingoes]; */
    uint32_t device_option; /* = 0b10 */
    uint32_t device_id;
} __attribute__((packed));

struct IAPFIDTokenValuesIdentifyAck {
    uint8_t length;  /* = 0x03 */
    uint8_t type;    /* = 0x00 */
    uint8_t subtype; /* = 0x00 */
    uint8_t status;  /* IAPFIDTokenValuesAckStatus */
    uint8_t lingo_ids[];
} __attribute__((packed));

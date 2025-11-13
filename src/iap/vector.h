#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "bool.h"

struct IAPVector {
    uint8_t* ptr;
    size_t   size;
    size_t   capacity;
};

void*   iap_vector_alloc(struct IAPVector* vec, size_t size, void* platform);
IAPBool iap_vector_append(struct IAPVector* vec, const void* ptr, size_t size, void* platform);
void    iap_vector_destroy(struct IAPVector* vec, void* platform);

#ifdef __cplusplus
}
#endif

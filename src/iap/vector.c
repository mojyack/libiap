#include <string.h>

#include "macros.h"
#include "platform.h"
#include "vector.h"

struct IAPVector _iap_construct_vector(void) {
    struct IAPVector ret = {0, 0, 0};
    return ret;
}

static IAPBool realloc(struct IAPVector* vec, size_t new_capacity, void* platform) {
    uint8_t* old_ptr = vec->ptr;
    uint8_t* new_ptr = iap_platform_malloc(platform, new_capacity);
    check_ret(new_ptr != NULL, iap_false);
    vec->ptr = new_ptr;
    if(old_ptr != NULL) {
        memcpy(vec->ptr, old_ptr, vec->size);
        iap_platform_free(platform, old_ptr);
    }
    vec->capacity = new_capacity;
    return iap_true;
}

void* iap_vector_alloc(struct IAPVector* vec, size_t size, void* platform) {
    const size_t required_capacity = vec->size + size;
    if(required_capacity > vec->capacity) {
        /* overalloc to reduce malloc() calls */
        check_ret(realloc(vec, required_capacity * 2, platform), NULL);
    }
    void* const ret = vec->ptr + vec->size;
    vec->size += size;
    return ret;
}

IAPBool iap_vector_append(struct IAPVector* vec, const void* ptr, size_t size, void* platform) {
    void* const dst = iap_vector_alloc(vec, size, platform);
    check_ret(dst != NULL, iap_false);
    memcpy(dst, ptr, size);
    return iap_true;
}

void iap_vector_destroy(struct IAPVector* vec, void* platform) {
    iap_platform_free(platform, vec->ptr);
}

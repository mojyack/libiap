#pragma once
#include <stdint.h>

#include "bool.h"

struct IAPSpan;
struct IAPVector;

/* fid-token-values.c */
int _iap_hanlde_set_fid_token_values(struct IAPSpan* request, struct IAPSpan* response);

/* debug.c */
const char* _iap_lingo_str(uint8_t lingo);
IAPBool     _iap_span_is_str(const struct IAPSpan* span);
const char* _iap_span_as_str(const struct IAPSpan* span);

#pragma once
#include "context.hpp"

struct IAPContext;

struct LinuxPlatformData {
    int         fd;
    Context     ctx;
    IAPContext* iap_ctx;
};

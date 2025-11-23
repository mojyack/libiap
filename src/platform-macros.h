#pragma once
#include <stdio.h>

#define IAP_LOGF(...)        \
    {                        \
        printf(__VA_ARGS__); \
        printf("\n");        \
    }
#define IAP_WARNF(...)       \
    {                        \
        printf("\x1B[91m");  \
        printf(__VA_ARGS__); \
        printf("\x1B[0m\n"); \
    }
#define IAP_ERRORF(...)      \
    {                        \
        printf("\x1B[93m");  \
        printf(__VA_ARGS__); \
        printf("\x1B[0m\n"); \
    }

#define IAP_ARTWORK_WIDTH  128
#define IAP_ARTWORK_HEIGHT 128
#define IAP_COLOR_ARTWORK  iap_true

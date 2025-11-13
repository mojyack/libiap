#pragma once

#define HID_BUFFER_SIZE  1024
#define SEND_BUFFER_SIZE 1024

enum IAPPhase {
    IAPPhase_Connected, /* initial state, waiting for StartIDPS */
    IAPPhase_IDPS,      /* idps started */
    IAPPhase_Auth,      /* idps completed, authenticating accessory */
    IAPPhase_Authed,    /* authentication completed, processing requests */
};

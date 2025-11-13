#include <stdint.h>

#include "macros.h"
#include "span.h"

const char* _iap_lingo_str(uint8_t lingo) {
    static const char* lingo_name_strs[] = {
        "General",
        "Microphone",
        "SimpleRemote",
        "DisplayRemote",
        "ExtendedInterface",
        "AccessoryPower",
        "USBHostMode",
        "RFTuner",
        "AccessoryEqualizer",
        "Sports",
        "DigitalAudio",
        NULL,
        "Storage",
        "IPodOut",
        "Location",
    };
    if(lingo >= array_size(lingo_name_strs) || lingo_name_strs[lingo] == NULL) {
        return "?";
    } else {
        return lingo_name_strs[lingo];
    }
}

IAPBool _iap_span_is_str(const struct IAPSpan* span) {
    return span->size > 0 && span->ptr[span->size - 1] == '\0';
}

const char* _iap_span_as_str(const struct IAPSpan* span) {
    return _iap_span_is_str(span) ? (char*)span->ptr : "(invalid)";
}

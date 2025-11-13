#include "_iap.h"
#include "endian.h"
#include "macros.h"
#include "span.h"
#include "spec/iap.h"
#include "vector.h"

#define pack_accepted(Ack)                                          \
    struct Ack* const ack = iap_span_alloc(response, sizeof(*ack)); \
    check_ret(ack != NULL, -IAPAckStatus_EOutOfResource);           \
    ack->length  = sizeof(struct Ack) - 1;                          \
    ack->type    = token_header->type;                              \
    ack->subtype = token_header->subtype;                           \
    ack->status  = IAPFIDTokenValuesIdentifyAckStatus_Accepted;

int _iap_hanlde_set_fid_token_values(struct IAPSpan* request, struct IAPSpan* response) {
    const struct IAPSetFIDTokenValuesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
    check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
    check_ret(iap_span_write_8(response, request_payload->num_token_values), -IAPAckStatus_EOutOfResource);
    for(int i = 0; i < request_payload->num_token_values; i += 1) {
        check_ret(request->size > sizeof(struct IAPFIDTokenValuesToken), -IAPAckStatus_EBadParameter);
        struct IAPFIDTokenValuesToken* token_header = (void*)request->ptr;
        struct IAPSpan                 token_span   = {
            request->ptr,
            token_header->length + 1 /* length does not include sizeof itself */,
        };
        check_ret(request->size >= token_span.size, -IAPAckStatus_EBadParameter);
        iap_span_read(request, token_span.size);

        switch(token_header->type << 8 | token_header->subtype) {
        case IAPFIDTokenTypes_Identify: {
            /* IAPFIDTokenValuesIdentifyToken contains vla, need to parse manually */
            iap_span_read(&token_span, sizeof(struct IAPFIDTokenValuesToken));
            uint8_t num_lingoes;
            check_ret(iap_span_read_8(&token_span, &num_lingoes), -IAPAckStatus_EBadParameter);
            print("accessory supported lingoes(%u):", num_lingoes);
            for(int i = 0; i < num_lingoes; i += 1) {
                uint8_t lingo_id;
                check_ret(iap_span_read_8(&token_span, &lingo_id), -IAPAckStatus_EBadParameter);
                IAP_LOGF("  %s(%u)", _iap_lingo_str(lingo_id), lingo_id);
            }
            uint32_t opt, id;
            check_ret(iap_span_read_32(&token_span, &opt), -IAPAckStatus_EBadParameter);
            check_ret(iap_span_read_32(&token_span, &id), -IAPAckStatus_EBadParameter);
            print("options=%04X device_id=%04X", opt, id);
            pack_accepted(IAPFIDTokenValuesIdentifyAck);
            if(opt != IAPIdentifyDeviceLingoesOptions_ImmediateAuth) {
                ack->status = IAPFIDTokenValuesIdentifyAckStatus_RequiredFailed;
            }
        } break;
        case IAPFIDTokenTypes_AccCaps: {
            const struct IAPFIDTokenValuesAccCapsToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            const uint64_t caps = swap_64(token->caps_bits);
            print("accessory caps: %lX", caps);
            pack_accepted(IAPFIDTokenValuesAccCapsAck);
        } break;
        case IAPFIDTokenTypes_AccInfo: {
            const struct IAPFIDTokenValuesAccInfoToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            switch(token->info_type) {
            case IAPFIDTokenValuesAccInfoTypes_AccName:
                print("accessory name: %s", _iap_span_as_str(&token_span));
                break;
            case IAPFIDTokenValuesAccInfoTypes_FirmwareVersion:
                check_ret(token_span.size == 3, -IAPAckStatus_EBadParameter);
                print("accessory firmware version: %X.%X.%X", token_span.ptr[0], token_span.ptr[1], token_span.ptr[2]);
                break;
            case IAPFIDTokenValuesAccInfoTypes_HardwareVersion:
                check_ret(token_span.size == 3, -IAPAckStatus_EBadParameter);
                print("accessory hardware version: %X.%X.%X", token_span.ptr[0], token_span.ptr[1], token_span.ptr[2]);
                break;
            case IAPFIDTokenValuesAccInfoTypes_Manufacture:
                print("accessory manufacture: %s", _iap_span_as_str(&token_span));
                break;
            case IAPFIDTokenValuesAccInfoTypes_ModelNumber:
                print("accessory model number: %s", _iap_span_as_str(&token_span));
                break;
            case IAPFIDTokenValuesAccInfoTypes_SerialNumber:
                print("accessory serial number: %s", _iap_span_as_str(&token_span));
                break;
            case IAPFIDTokenValuesAccInfoTypes_MaxPayloadSize: {
                uint16_t val;
                check_ret(iap_span_read_16(&token_span, &val), -IAPAckStatus_EBadParameter);
                print("accessory max payload size: %u", val);
            } break;
            case IAPFIDTokenValuesAccInfoTypes_AccStatus: {
                uint32_t val;
                check_ret(iap_span_read_32(&token_span, &val), -IAPAckStatus_EBadParameter);
                print("accessory status: %X", val);
            } break;
            case IAPFIDTokenValuesAccInfoTypes_RFCerts: {
                uint32_t val;
                check_ret(iap_span_read_32(&token_span, &val), -IAPAckStatus_EBadParameter);
                print("accessory rf cert: %X", val);
            } break;
            }
            pack_accepted(IAPFIDTokenValuesAccInfoAck);
            ack->info_type = token->info_type;
        } break;
        case IAPFIDTokenTypes_IPodPreference: {
            const struct IAPFIDTokenValuesIPodPreferenceToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("accessory setting %X=%X", token->class_id, token->setting_id);
            pack_accepted(IAPFIDTokenValuesIPodPreferenceAck);
            ack->class_id = token->class_id;
        } break;
        case IAPFIDTokenTypes_EAProtocol: {
            const struct IAPFIDTokenValuesEAProtocolToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("ea protocol %X=%s", token->protocol_index, _iap_span_as_str(&token_span));
            pack_accepted(IAPFIDTokenValuesEAProtocolAck);
            ack->protocol_index = token->protocol_index;
        } break;
        case IAPFIDTokenTypes_BundleSeedIDPref: {
            const struct IAPFIDTokenValuesBundleSeedIDPrefToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("bundle seed id %.10s", token->bundle_seed_id_string);
            pack_accepted(IAPFIDTokenValuesBundleSeedIDPrefAck);
        } break;
        case IAPFIDTokenTypes_ScreenInfo: {
            const struct IAPFIDTokenValuesScreenInfoToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("screen info:");
            IAP_LOGF("  screen size(inch): %ux%u", swap_16(token->total_screen_width_inches), swap_16(token->total_screen_height_inches));
            IAP_LOGF("  screen size(pixel): %ux%u", swap_16(token->total_screen_width_pixels), swap_16(token->total_screen_height_pixels));
            IAP_LOGF("  ipod out size(pixel): %ux%u", swap_16(token->ipod_out_screen_width_pixels), swap_16(token->ipod_out_screen_height_pixels));
            IAP_LOGF("  featurel: %X\n", token->screen_feature_mask);
            IAP_LOGF("  gamma: %u\n", token->screen_gamma_value);
            pack_accepted(IAPFIDTokenValuesScreenInfoAck);
        } break;
        case IAPFIDTokenTypes_EAProtocolMetadata: {
            const struct IAPFIDTokenValuesEAProtocolMetadataToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("ea protocol metadata %X=%X", token->protocol_index, token->metadata_type);
            pack_accepted(IAPFIDTokenValuesEAProtocolMetadataAck);
        } break;
        case IAPFIDTokenTypes_AccDigitalAudioSampleRates: {
            const struct IAPFIDTokenValuesAccDigitalAudioSampleRatesToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("accessory supported audio sample rates:");
            while(token_span.size > 0) {
                uint32_t rate;
                check_ret(iap_span_read_32(&token_span, &rate), -IAPAckStatus_EBadParameter);
                IAP_LOGF("  %u", rate);
            }
            pack_accepted(IAPFIDTokenValuesAccDigitalAudioSampleRatesAck);
        } break;
        case IAPFIDTokenTypes_AccDigitalAudioVideoDelay: {
            const struct IAPFIDTokenValuesAccDigitalAudioVideoDelayToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("accessory video delay: %u", token->delay);
            pack_accepted(IAPFIDTokenValuesAccDigitalAudioVideoDelayAck);
        } break;
        case IAPFIDTokenTypes_MicrophoneCaps: {
            const struct IAPFIDTokenValuesMicrophoneCapsToken* token = iap_span_read(&token_span, sizeof(*token));
            check_ret(token != NULL, -IAPAckStatus_EBadParameter);
            print("accessory microphone caps: %X", token->caps_bits);
            pack_accepted(IAPFIDTokenValuesMicrophoneCapsAck);
        } break;
        default:
            print("unknown fid %04X", token_header->type << 8 | token_header->subtype);
            return -IAPAckStatus_EBadParameter;
        }
    }
    return 0;
}

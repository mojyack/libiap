#pragma once
#include <string.h>

#include "_iap.h"
#include "constants.h"
#include "endian.h"
#include "iap.h"
#include "macros.h"
#include "pack-util.h"
#include "platform.h"
#include "span.h"
#include "spec/iap.h"

IAPBool iap_init_ctx(struct IAPContext* ctx) {
    ctx->hid_recv_buf = iap_platform_malloc(ctx->platform, HID_BUFFER_SIZE);
    check_ret(ctx->hid_recv_buf != NULL, iap_false);
    ctx->hid_recv_buf_cursor = 0;
    ctx->send_buf            = iap_platform_malloc(ctx->platform, SEND_BUFFER_SIZE);
    check_ret(ctx->send_buf != NULL, iap_false);
    ctx->send_buf_sending_cursor      = 0;
    ctx->send_buf_sending_range_begin = 0;
    ctx->send_buf_sending_range_end   = 0;
    ctx->on_send_complete             = NULL;
    ctx->trans_id                     = 0;
    ctx->send_busy                    = iap_false;
    ctx->phase                        = IAPPhase_Connected;
    return iap_true;
}

IAPBool iap_deinit_ctx(struct IAPContext* ctx) {
    iap_platform_free(ctx->platform, ctx->hid_recv_buf);
    iap_platform_free(ctx->platform, ctx->send_buf);
    return iap_true;
}

#define alloc_response_extra(Type, var, extra)                             \
    struct Type* var = iap_span_alloc(response, sizeof(*payload) + extra); \
    check_ret(payload != NULL, -IAPAckStatus_EOutOfResource);

#define alloc_response(Type, var) alloc_response_extra(Type, var, 0)

static int32_t handle_in_connected(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_StartIDPS: {
            alloc_response(IAPIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = command;

            ctx->phase = IAPPhase_IDPS;
            print("idps started");

            return IAPGeneralCommandID_IPodAck;
        } break;
        }
        break;
    }
    return -IAPAckStatus_EUnknownID;
}

static IAPBool transition_idps_to_auth_cb(struct IAPContext* ctx) {
    print("starting accessory authentication");
    check_ret(ctx->phase == IAPPhase_IDPS, iap_false);
    ctx->phase = IAPPhase_Auth;
    check_ret(_iap_send_packet(ctx, IAPLingoID_General, IAPGeneralCommandID_GetAccessoryAuthenticationInfo, (ctx->trans_id += 1), _iap_get_buffer_for_send_payload(ctx).ptr), iap_false);
    return iap_true;
}

static int32_t handle_in_idps(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_RequestTransportMaxPayloadSize: {
            alloc_response(IAPReturnTransportMaxPayloadSizePayload, payload);
            payload->max_payload_size = swap_16(HID_BUFFER_SIZE - 1 /*sync*/ - 1 /*sof*/ - 3 /*length*/ - 1 /*checksum*/);
            return IAPGeneralCommandID_ReturnTransportMaxPayloadSize;
        } break;
        case IAPGeneralCommandID_IdentifyDeviceLingoes: {
            const struct IAPIdentifyDeviceLingoesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);

            const uint32_t bits = swap_32(request_payload->lingoes_bits);
            print("acc supported lingos:");
            for(int i = 0; i < 32; i += 1) {
                if(bits & (1 << i)) {
                    IAP_LOGF("  %s", _iap_lingo_str(i));
                }
            }
            print("auth_option=%02X device_id=%08X\n", swap_32(request_payload->options), swap_32(request_payload->device_id));

            alloc_response(IAPIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = command;
            /* TODO: wip */
            return IAPGeneralCommandID_IPodAck;
        } break;
        case IAPGeneralCommandID_SetFIDTokenValues: {
            const int ret = _iap_hanlde_set_fid_token_values(request, response);
            check_ret(ret == 0, ret);
            return IAPGeneralCommandID_AckFIDTokenValues;
        } break;
        case IAPGeneralCommandID_EndIDPS: {
            const struct IAPEndIDPSPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("end idps status=0x%02X", request_payload->status);
            check_ret(request_payload->status == IAPEndIDPSStatus_Success, -IAPAckStatus_ECommandFailed);

            ctx->on_send_complete = transition_idps_to_auth_cb;

            alloc_response(IAPIDPSStatusPayload, payload);
            payload->status = IAPIDPSStatus_Success;
            return IAPGeneralCommandID_IDPSStatus;
        } break;
        case IAPGeneralCommandID_GetIPodOptionsForLingo: {
            const struct IAPGetIPodOptionsForLingoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);

            alloc_response(IAPRetIPodOptionsForLingoPayload, payload);
            payload->lingo_id = request_payload->lingo_id;
            print("ipod option for %d", request_payload->lingo_id);
            switch(request_payload->lingo_id) {
            case IAPLingoID_SimpleRemote:
                payload->bits = swap_64(IAPRetIPodOptionsForLingoSimpleRemoteBits_ContextSpecificControls);
                break;
            case IAPLingoID_General:
            case IAPLingoID_DisplayRemote:
            case IAPLingoID_ExtendedInterface:
            case IAPLingoID_DigitalAudio:
            case IAPLingoID_Storage: /* TODO: this is not supported */
                payload->bits = 0;
                break;
            case IAPLingoID_USBHostMode:
            case IAPLingoID_RFTuner:
            case IAPLingoID_Sports:
            case IAPLingoID_IPodOut:
            case IAPLingoID_Location: { /* not supported */
                return -IAPAckStatus_EBadParameter;
            }
            }
            return IAPGeneralCommandID_RetIPodOptionsForLingo;
        } break;
        }
        break;
    }
    return iap_false;
}

static IAPBool send_auth_challenge_sig(struct IAPContext* ctx) {
    check_ret(ctx->phase == IAPPhase_Auth, iap_false);
    struct IAPSpan                     request = _iap_get_buffer_for_send_payload(ctx);
    struct IAPGetAccAuthSigPayload2p0* payload = iap_span_alloc(&request, sizeof(*payload));
    check_ret(payload != NULL, iap_false);
    payload->retry = 1;
    check_ret(_iap_send_packet(ctx, IAPLingoID_General, IAPGeneralCommandID_GetAccessoryAuthenticationSignature, (ctx->trans_id += 1), request.ptr), iap_false);
    return iap_true;
}

static int32_t handle_in_auth(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_RetAccessoryAuthenticationInfo: {
            const struct IAPRetAccAuthInfoPayload2p0* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("accessory cert %u/%u", request_payload->cert_current_section_index, request_payload->cert_max_section_index);
            iap_platform_dump_hex(request->ptr, request->size);
            if(request_payload->cert_current_section_index < request_payload->cert_max_section_index) {
                alloc_response(IAPIPodAckPayload, payload);
                payload->status = IAPAckStatus_Success;
                payload->id     = command;
                return IAPGeneralCommandID_IPodAck;
            } else {
                ctx->on_send_complete = send_auth_challenge_sig;

                alloc_response(IAPAckAccAuthInfoPayload, payload);
                payload->status = IAPAckAccAuthInfoStatus_Supported;
                return IAPGeneralCommandID_AckAccessoryAuthenticationInfo;
            }
        } break;
        case IAPGeneralCommandID_RetAccessoryAuthenticationSignature: {
            print("accessory signature");
            iap_platform_dump_hex(request->ptr, request->size);

            alloc_response(IAPAckAccAuthSigPayload, payload);
            payload->status = IAPAckStatus_Success;

            ctx->phase = IAPPhase_Authed;

            return IAPGeneralCommandID_AckAccessoryAuthenticationStatus;
        } break;
        case IAPGeneralCommandID_SetEventNotification: {
            const struct IAPSetEventNotificationPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("event notification %lX", swap_64(request_payload->mask));

            alloc_response(IAPIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = command;
            return IAPGeneralCommandID_IPodAck;
        } break;
        case IAPGeneralCommandID_GetSupportedEventNotification: {
            alloc_response(IAPRetSupportedEventNotificationPayload, payload);
            payload->mask = swap_64(0);
            return IAPGeneralCommandID_RetSupportedEventNotification;
        } break;
        }
        break;
    }
    return -IAPAckStatus_EUnknownID;
}

static int32_t handle_in_authed(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_SetUIMode: {
            const struct IAPSetUIModePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("set ui mode 0x%02X", request_payload->ui_mode);

            alloc_response(IAPIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = command;
            return IAPGeneralCommandID_IPodAck;
        } break;
        }
        break;
    case IAPLingoID_DisplayRemote:
        switch(command) {
        case IAPDisplayRemoteCommandID_SetRemoteEventNotification: {
            const struct IAPSetRemoteEventNotificationPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("set remote event notification 0x%04X", swap_32(request_payload->mask));

            alloc_response(IAPIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = command;
            return IAPDisplayRemoteCommandID_IPodAck;
        };
        case IAPDisplayRemoteCommandID_GetIPodStateInfo: {
            const struct IAPGetIPodStateInfoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("get ipod state info type=0x%02X", request_payload->type);
#define ret_state_info                     \
    payload->type = request_payload->type; \
    return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            switch(request_payload->type) {
            case IAPIPodStateType_TrackTimePositionMSec: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackTimePositionMSecPayload, payload);
                ret_state_info;
            } break;
            case IAPIPodStateType_TrackPlaybackIndex: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackPlaybackIndexPayload, payload);
                payload->index = swap_32(status.track_index);
                ret_state_info;
            } break;
            case IAPIPodStateType_ChapterIndex: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateChapterIndexPayload, payload);
                payload->index = swap_32(status.track_index);
                /* no chapters */
                payload->chapter_count = 0;
                payload->chapter_index = -1;
                ret_state_info;
            } break;
            case IAPIPodStateType_PlayStatus: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStatePlayStatusPayload, payload);
                payload->status = status.state; /* TODO: convert enum */
                ret_state_info;
            } break;
            case IAPIPodStateType_Volume: {
                struct IAPPlatformVolumeStatus status;
                check_ret(iap_platform_get_volume(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateVolumePayload, payload);
                payload->mute_state = status.muted;
                payload->ui_volume  = status.volume;
                ret_state_info;
            } break;
            case IAPIPodStateType_Power: {
                struct IAPPlatformPowerStatus status;
                check_ret(iap_platform_get_power_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStatePowerPayload, payload);
                payload->power_state   = status.state; /* TODO: convert enum */
                payload->battery_level = status.battery_level;
                ret_state_info;
            } break;
            case IAPIPodStateType_EQSetting: {
                alloc_response(IAPIPodStateEQSettingPayload, payload);
                /* no eq setting support yet */
                payload->eq_index = 0;
                ret_state_info;
            } break;
            case IAPIPodStateType_ShuffleSetting: {
                alloc_response(IAPIPodStateShuffleSettingPayload, payload);
                check_ret(iap_platform_get_shuffle_setting(ctx->platform, &payload->shuffle_state), iap_false);
                ret_state_info;
            } break;
            case IAPIPodStateType_RepeatSetting: {
                alloc_response(IAPIPodStateRepeatSettingPayload, payload);
                check_ret(iap_platform_get_repeat_setting(ctx->platform, &payload->repeat_state), iap_false);
                ret_state_info;
            } break;
            case IAPIPodStateType_DateTimeSetting: {
                struct IAPPlatformTime time;
                check_ret(iap_platform_get_date_time(ctx->platform, &time), iap_false);
                alloc_response(IAPIPodStateDateTimeSettingPayload, payload);
                payload->year   = swap_16(time.year);
                payload->month  = time.month;
                payload->day    = time.day;
                payload->hour   = time.hour;
                payload->minute = time.minute;
                ret_state_info;
            } break;
            case IAPIPodStateType_AlarmSetting: {
                alloc_response(IAPIPodStateAlarmSettingPayload, payload);
                ret_state_info;
            } break;
            case IAPIPodStateType_BacklightLevel: {
                alloc_response(IAPIPodStateBacklightLevelPayload, payload);
                check_ret(iap_platform_get_backlight_level(ctx->platform, &payload->level), iap_false);
                ret_state_info;
            } break;
            case IAPIPodStateType_HoldSwitchState: {
                IAPBool state;
                check_ret(iap_platform_get_hold_switch_state(ctx->platform, &state), iap_false);
                alloc_response(IAPIPodStateHoldSwitchStatePayload, payload);
                payload->state = state;
                ret_state_info;
            } break;
            case IAPIPodStateType_SoundCheckState: {
                alloc_response(IAPIPodStateSoundCheckStatePayload, payload);
                payload->state = 0; /* no sound check */
                ret_state_info;
            } break;
            case IAPIPodStateType_AudiobookSpeeed: {
                alloc_response(IAPIPodStateAudiobookSpeeedPayload, payload);
                payload->speed = IAPIPodStateAudiobookSpeeed_Normal;
                ret_state_info;
            } break;
            case IAPIPodStateType_TrackTimePositionSec: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx->platform, &status), iap_false);
                alloc_response(IAPIPodStateTrackTimePositionSecPayload, payload);
                payload->position_s = swap_32(status.track_pos_ms / 1000);
                ret_state_info;
            } break;
            case IAPIPodStateType_AbsoluteVolume: {
                struct IAPPlatformVolumeStatus status;
                check_ret(iap_platform_get_volume(ctx->platform, &status), iap_false);
                alloc_response(IAPIPodStateAbsoluteVolumePayload, payload);
                payload->mute_state      = status.muted;
                payload->ui_volume       = status.volume;
                payload->absolute_volume = status.volume;
                ret_state_info;
            } break;
            case IAPIPodStateType_TrackCaps: {
                alloc_response(IAPIPodStateTrackCapsPayload, payload);
                payload->caps = 0; /* no caps */
                ret_state_info;
            } break;
            case IAPIPodStateType_PlaybackEngineContents: {
                alloc_response(IAPIPodStatePlaybackEngineContentsPayload, payload);
                payload->count = 0; /* TODO: shoud be supported? */
                ret_state_info;
            } break;
            default:
                warn("invalid request type 0x%02X", request_payload->type);
                return -IAPAckStatus_EBadParameter;
            }
#undef ret_state_info
        };
        }
        break;
    case IAPLingoID_ExtendedInterface:
        switch(command) {
        case IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackInfo: {
            const struct IAPExtendedGetIndexedPlayingTrackInfoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("get indexed playing track info type=0x%02X track=%d chapter=%d", request_payload->type, swap_32(request_payload->track_index), swap_16(request_payload->chapter_index));
#define ret_track_info                     \
    payload->type = request_payload->type; \
    return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            switch(request_payload->type) {
            case IAPExtendedIndexedPlayingTrackInfoType_TrackCapsInfo: {
                uint32_t                    length;
                struct IAPPlatformTrackInfo info = {.track_total_ms = &length};
                check_ret(iap_platform_get_indexed_track_info(ctx->platform, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackCapsInfoPayload, payload);
                payload->track_caps      = 0;
                payload->track_length_ms = swap_32(length);
                payload->chapter_count   = 0;
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_PodcastName: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoPodcastNamePayload, payload, 1);
                payload->name[0] = '\0';
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackReleaseDate: {
                struct IAPPlatformTime      time;
                struct IAPPlatformTrackInfo info = {.release_date = &time};
                check_ret(iap_platform_get_indexed_track_info(ctx->platform, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackReleaseDatePayload, payload);
                payload->seconds = time.seconds;
                payload->minutes = time.minute;
                payload->hours   = time.hour;
                payload->day     = time.day;
                payload->month   = time.month;
                payload->year    = swap_16(time.year);
                payload->weekday = 0; /* TODO: set weekday? */
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackDescription: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackDescriptionPayload, payload, 1);
                payload->info_bits      = 0;
                payload->index          = 0;
                payload->description[0] = '\0';
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackSongLyrics: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackSongLyricsPayload, payload, 1);
                payload->info_bits = 0;
                payload->index     = 0;
                payload->lyrics[0] = '\0';
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackGenre: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackGenrePayload, payload, 1);
                payload->genre[0] = '\0';
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackComposer: {
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackComposerPayload, payload);
                struct IAPPlatformTrackInfo info = {.composer = response};
                check_ret(iap_platform_get_indexed_track_info(ctx->platform, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                ret_track_info;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackArtworkCount: {
                // IAPArtworkPixelFormats_RGB565LE;
                // struct IAPExtendedRetIndexedPlayingTrackInfoTrackArtworkCountPayload payload = {
                //     .};
                warn("artwork not implemented");
                return -IAPAckStatus_ECommandFailed;
            } break;
            default:
                warn("invalid request type 0x%02X", request_payload->type);
                return -IAPAckStatus_EBadParameter;
            }
#undef ret_track_info
        } break;
        case IAPExtendedInterfaceCommandID_ResetDBSelection: {
            print("reset db selection");
            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetPlayStatus: {
            struct IAPPlatformPlayStatus status;
            check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
            alloc_response(IAPExtendedRetPlayStatusPayload, payload);
            payload->state          = status.state; /* TODO: convert enum */
            payload->track_pos_ms   = swap_32(status.track_pos_ms);
            payload->track_total_ms = swap_32(status.track_total_ms);
            return IAPExtendedInterfaceCommandID_ReturnPlayStatus;
        } break;
        case IAPExtendedInterfaceCommandID_GetCurrentPlayingTrackIndex: {
            struct IAPPlatformPlayStatus status;
            check_ret(iap_platform_get_play_status(ctx->platform, &status), -IAPAckStatus_ECommandFailed);
            alloc_response(IAPReturnCurrentPlayingTrackIndexPayload, payload);
            payload->index = swap_32(status.track_index);
            return IAPExtendedInterfaceCommandID_ReturnCurrentPlayingTrackIndex;
        } break;
        case IAPExtendedInterfaceCommandID_SetPlayStatusChangeNotification: {
            if(request->size == sizeof(struct IAPSetPlayStatusChangeNotification1BytePayload)) {
                const struct IAPSetPlayStatusChangeNotification1BytePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
                print("play status change notification %s", request_payload->enable ? "enabled" : "disabled");
            } else if(request->size == sizeof(struct IAPSetPlayStatusChangeNotification4BytesPayload)) {
                const struct IAPSetPlayStatusChangeNotification4BytesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
                print("play status change notification mask 0x%04X", swap_32(request_payload->mask));
            }
            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetShuffle: {
            alloc_response(IAPReturnShufflePayload, payload);
            check_ret(iap_platform_get_shuffle_setting(ctx->platform, &payload->mode), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnShuffle;
        } break;
        case IAPExtendedInterfaceCommandID_SetShuffle: {
            const struct IAPSetShufflePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("set shuffle 0x%02X", request_payload->mode);
            check_ret(iap_platform_set_shuffle_setting(ctx->platform, request_payload->mode), -IAPAckStatus_ECommandFailed);

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetRepeat: {
            alloc_response(IAPReturnRepeatPayload, payload);
            check_ret(iap_platform_get_repeat_setting(ctx->platform, &payload->mode), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnRepeat;
        } break;
        case IAPExtendedInterfaceCommandID_SetRepeat: {
            const struct IAPSetRepeatPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            print("set repeat 0x%02X", request_payload->mode);
            check_ret(iap_platform_set_repeat_setting(ctx->platform, request_payload->mode), -IAPAckStatus_ECommandFailed);

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        }
        break;
    }
    return -IAPAckStatus_EUnknownID;
}

static int32_t build_ipod_ack_response(uint8_t lingo, uint16_t command, uint8_t status, struct IAPSpan* response) {
    static int32_t ack_commmand_ids[] = {
        IAPGeneralCommandID_IPodAck,
        -1,
        IAPSimpleRemoteCommandID_IPodAck,
        IAPDisplayRemoteCommandID_IPodAck,
        IAPExtendedInterfaceCommandID_IPodAck,
        -1,
        IAPUSBHostModeCommandID_IPodAck,
        -1,
        -1,
        IAPSportsCommandID_IPodAck,
        IAPDigitalAudioCommandID_IPodAck,
        -1,
        IAPStorageCommandID_IPodAck,
        IAPIPodOutCommandID_IPodAck,
        IAPLocationCommandID_IPodAck,
    };
    check_ret(lingo < array_size(ack_commmand_ids) && ack_commmand_ids[lingo] >= 0, -1);
    switch(lingo) {
    case IAPLingoID_General:
    case IAPLingoID_SimpleRemote:
    case IAPLingoID_DisplayRemote:
    case IAPLingoID_USBHostMode:
    case IAPLingoID_Sports:
    case IAPLingoID_DigitalAudio:
    case IAPLingoID_IPodOut:
    case IAPLingoID_Location: {
        alloc_response(IAPIPodAckPayload, payload);
        payload->id     = command;
        payload->status = status;
    } break;
    case IAPLingoID_ExtendedInterface: {
        alloc_response(IAPExtendedIPodAckPayload, payload);
        payload->id     = swap_16(command);
        payload->status = status;
    } break;
    case IAPLingoID_Storage: {
        alloc_response(IAPStorageIPodAckPayload, payload);
        payload->id     = command;
        payload->status = status;
        payload->handle = -1; /* TODO: set proper handle */
    } break;
    }
    return ack_commmand_ids[lingo];
}

IAPBool _iap_feed_packet(struct IAPContext* ctx, const uint8_t* const data, const size_t size) {
    union {
        uint8_t  u8;
        uint16_t u16;
    } buf;
    struct IAPSpan span = {(uint8_t*)data, size};

    /* read sof byte */
    check_ret(iap_span_read_8(&span, &buf.u8), iap_false);
    if(buf.u8 == IAP_SYNC_BYTE) {
        /* skip sync byte */
        check_ret(iap_span_read_8(&span, &buf.u8), iap_false);
    }
    check_ret(buf.u8 == IAP_SOF_BYTE, iap_false, "%x != %x", buf.u8, IAP_SOF_BYTE);
    /* read size */
    check_ret(iap_span_read_8(&span, &buf.u8), iap_false);
    uint16_t length;
    if(buf.u8 == 0) {
        /* long packet */
        check_ret(iap_span_read_16(&span, &buf.u16), iap_false);
        length = buf.u16;
    } else {
        length = buf.u8;
    }
    /* read lingo id */
    check_ret(iap_span_read_8(&span, &buf.u8), iap_false);
    uint8_t lingo = buf.u8;
    /* read command id */
    uint16_t command;
    if(lingo == IAPLingoID_ExtendedInterface) {
        check_ret(iap_span_read_16(&span, &buf.u16), iap_false);
        command = buf.u16;
    } else {
        check_ret(iap_span_read_8(&span, &buf.u8), iap_false);
        command = buf.u8;
    }
    /* request payload (and maybe trans id) */
    struct IAPSpan request = {
        span.ptr,
        length - 1 /* lingo id */ - (lingo == IAPLingoID_ExtendedInterface ? 2 : 1 /* command id */),
    };

    /* read checksum */
    check_ret(span.size >= request.size + 1 /* checksum */, iap_false);
    const uint8_t checksum = data[request.size]; /* TODO: verify checksum */
    print("processing iap request 0x%02X(%s):0x%04X length=%d while phase=%d", lingo, _iap_lingo_str(lingo), command, length, ctx->phase);

    /* request handling */
    uint16_t trans_id;
    /* assume trans id is enabled */
    check_ret(iap_span_read_16(&request, &trans_id), iap_false);

    struct IAPSpan response = _iap_get_buffer_for_send_payload(ctx);
    int32_t        ret;
    switch(ctx->phase) {
    case IAPPhase_Connected:
        ret = handle_in_connected(ctx, lingo, command, &request, &response);
        break;
    case IAPPhase_IDPS:
        ret = handle_in_idps(ctx, lingo, command, &request, &response);
        break;
    case IAPPhase_Auth:
        ret = handle_in_auth(ctx, lingo, command, &request, &response);
        break;
    case IAPPhase_Authed:
        ret = handle_in_authed(ctx, lingo, command, &request, &response);
        break;
    }
    if(ret < 0) {
        /* handling failed, replace response with ipod ack */
        warn("command handling failed");
        response = _iap_get_buffer_for_send_payload(ctx);
        ret      = build_ipod_ack_response(lingo, command, -ret, &response);
        check_ret(ret >= 0, iap_false);
    }
    check_ret(_iap_send_packet(ctx, lingo, ret, trans_id, response.ptr), iap_false);
    return iap_true;
}

struct IAPSpan _iap_get_buffer_for_send_payload(struct IAPContext* ctx) {
    const size_t header_size = 1 /* sof */ +
                               3 /* long format length */ +
                               1 /* lingo */ +
                               2 /* largest command id */ +
                               2 /* trans id */;
    const size_t footer_size = 1 /* checksum */;

    struct IAPSpan buf = {ctx->send_buf + header_size, SEND_BUFFER_SIZE - header_size - footer_size};
    return buf;
}

IAPBool _iap_send_packet(struct IAPContext* ctx, uint8_t lingo, uint16_t command, int32_t trans_id, uint8_t* final_ptr) {
    uint8_t* ptr          = _iap_get_buffer_for_send_payload(ctx).ptr;
    size_t   payload_size = final_ptr - ptr;

#define pack_8(val) \
    ptr -= 1;       \
    *(uint8_t*)ptr = val;
#define pack_16(val) \
    ptr -= 2;        \
    *(uint16_t*)ptr = swap_16(val);

    /* fill header in reverse order */
    /* trans id */
    if(trans_id >= 0) {
        pack_16(trans_id);
    }
    /* command id */
    if(lingo == IAPLingoID_ExtendedInterface) {
        pack_16(command);
    } else {
        pack_8(command);
    }
    /* lingo */
    pack_8(lingo);
    /* length */
    const uint16_t length = 1 /*lingo*/ + (lingo == IAPLingoID_ExtendedInterface ? 2 : 1) /*command*/ + (trans_id >= 0 ? 2 : 0) + payload_size;
    if(length <= 0xFC) {
        pack_8(length);
    } else {
        pack_16(length);
        pack_8(0);
    }
    /* sof */
    pack_8(IAP_SOF_BYTE);

    /* set checksum */
    uint8_t checksum = 0;
    for(uint8_t* p = ptr + 1 /* exclude sof byte */; p < final_ptr; p += 1) {
        checksum += *p;
    }
    checksum *= -1;
    *final_ptr = checksum;

    check_ret(_iap_send_hid_reports(ctx, ptr - ctx->send_buf, final_ptr - ctx->send_buf + 1 /* include checksum */), iap_false);
    return iap_true;
}

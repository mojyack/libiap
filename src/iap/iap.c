#include <string.h>

#include "constants.h"
#include "endian.h"
#include "iap.h"
#include "macros.h"
#include "pack-util.h"
#include "platform.h"
#include "span.h"
#include "spec/iap.h"
#include "unaligned.h"

enum TransIDSupport {
    TransIDUnknown,
    TransIDSupported,
    TransIDNotSupported,
};

IAPBool iap_init_ctx(struct IAPContext* ctx) {
    const IAPBool  hs                      = iap_platform_get_usb_speed(ctx) == IAPPlatformUSBSpeed_High;
    const uint16_t max_input_hid_desc_size = hs ? 0x02FF : 0x3F;

    ctx->hid_recv_buf = iap_platform_malloc(ctx, HID_BUFFER_SIZE, 0);
    check_ret(ctx->hid_recv_buf != NULL, iap_false);
    ctx->hid_recv_buf_cursor = 0;
    ctx->send_buf            = iap_platform_malloc(ctx, SEND_BUFFER_SIZE, 0);
    check_ret(ctx->send_buf != NULL, iap_false);
    ctx->send_buf_sending_cursor      = 0;
    ctx->send_buf_sending_range_begin = 0;
    ctx->send_buf_sending_range_end   = 0;
    ctx->on_send_complete             = NULL;
    for(size_t i = 0; i < array_size(ctx->active_events); i += 1) {
        ctx->active_events[i].callback = NULL;
    }
    ctx->handling_trans_id       = -1;
    ctx->trans_id_support        = TransIDUnknown;
    ctx->artwork.valid           = iap_false;
    ctx->trans_id                = 0;
    ctx->enabled_notifications_3 = 0;
    ctx->notifications_3         = 0;
    ctx->enabled_notifications_4 = 0;
    ctx->notifications_4         = 0;
    ctx->notification_tick       = 0;
    ctx->hid_send_staging_buf    = iap_platform_malloc(ctx, max_input_hid_desc_size + 1 /* report id */, IAPPlatformMallocFlags_Uncached);
    check_ret(ctx->hid_send_staging_buf != NULL, iap_false);
    ctx->send_busy              = iap_false;
    ctx->flushing_notifications = iap_false;
    ctx->phase                  = IAPPhase_Connected;
    return iap_true;
}

IAPBool iap_deinit_ctx(struct IAPContext* ctx) {
    if(ctx->artwork.valid) {
        iap_platform_close_artwork(ctx, &ctx->artwork);
    }
    iap_platform_free(ctx, ctx->hid_send_staging_buf);
    iap_platform_free(ctx, ctx->hid_recv_buf);
    iap_platform_free(ctx, ctx->send_buf);
    return iap_true;
}

#define alloc_response_extra(Type, var, extra)                             \
    struct Type* var = iap_span_alloc(response, sizeof(*payload) + extra); \
    check_ret(payload != NULL, -IAPAckStatus_EOutOfResource);

#define alloc_response(Type, var) alloc_response_extra(Type, var, 0)

static uint32_t play_stage_change_notification_set_mask_to_type_mask(uint32_t mask) {
    uint32_t ret = 0;
    if(mask & IAPStatusChangeNotificationBits_Basic) {
        ret |= 1 << IAPStatusChangeNotificationType_PlaybackStopped |
               1 << IAPStatusChangeNotificationType_PlaybackFEWSeekStop |
               1 << IAPStatusChangeNotificationType_PlaybackREWSeekStop;
    }
    if(mask & IAPStatusChangeNotificationBits_Extended) {
        ret |= 1 << IAPStatusChangeNotificationType_PlaybackStatusExtended;
    }
    if(mask & IAPStatusChangeNotificationBits_TrackIndex) {
        ret |= 1 << IAPStatusChangeNotificationType_TrackIndex;
    }
    if(mask & IAPStatusChangeNotificationBits_TrackTimeOffsetMSec) {
        ret |= 1 << IAPStatusChangeNotificationType_TrackTimeOffsetMSec;
    }
    if(mask & IAPStatusChangeNotificationBits_TrackTimeOffsetSec) {
        ret |= 1 << IAPStatusChangeNotificationType_TrackTimeOffsetSec;
    }
    if(mask & IAPStatusChangeNotificationBits_PlaybackEngineContentsChanged) {
        ret |= 1 << IAPStatusChangeNotificationType_PlaybackEngineContentsChanged;
    }
    return ret;
}

static IAPBool send_artwork_chunk_cb(struct IAPContext* ctx) {
    struct IAPSpan request = _iap_get_buffer_for_send_payload(ctx);
    if(ctx->artwork_chunk_index == 0) {
        struct IAPRetTrackArtworkDataFirstPayload* payload = iap_span_alloc(&request, sizeof(*payload));

        payload->index                = swap_16(ctx->artwork_chunk_index);
        payload->pixel_format         = ctx->artwork.color ? IAPArtworkPixelFormats_RGB565LE : IAPArtworkPixelFormats_Mono;
        payload->pixel_width          = swap_16(ctx->artwork.width);
        payload->pixel_height         = swap_16(ctx->artwork.height);
        payload->inset_top_left_x     = 0;
        payload->inset_top_left_y     = 0;
        payload->inset_bottom_right_x = payload->pixel_width;
        payload->inset_bottom_right_y = payload->pixel_height;
        payload->stride               = swap_32(ctx->artwork.width * 2); /* TODO: support stride */
    } else {
        struct IAPRetTrackArtworkDataSubsequenctPayload* payload = iap_span_alloc(&request, sizeof(*payload));

        payload->index = swap_16(ctx->artwork_chunk_index);
    }
    struct IAPSpan artwork;
    size_t         copy_size = 0;
    if(!ctx->opts.artwork_single_report || ctx->artwork_chunk_index != 0) {
        check_ret(iap_platform_get_artwork_ptr(ctx, &ctx->artwork, &artwork), iap_false);
        check_ret(iap_span_read(&artwork, ctx->artwork_cursor) != NULL, iap_false); /* skip already read chunk */
        copy_size = min((ctx->opts.artwork_single_report ? 48 : request.size), artwork.size);
        memcpy(iap_span_alloc(&request, copy_size), iap_span_read(&artwork, copy_size), copy_size);
    }
    check_ret(_iap_send_packet(ctx, ctx->artwork_data_lingo, ctx->artwork_data_command, ctx->artwork_trans_id, request.ptr), iap_false);
    if(artwork.size > 0) {
        /* more to send, ask to call again */
        ctx->artwork_cursor += copy_size;
        ctx->artwork_chunk_index += 1;
        ctx->on_send_complete = send_artwork_chunk_cb;
        print("track artwork left %lu bytes", artwork.size);
    } else {
        /* finished, free artwork */
        check_ret(iap_platform_close_artwork(ctx, &ctx->artwork), iap_false);
        ctx->artwork.valid = iap_false;
        print("track artwork done");
    }
    return iap_true;
}

static int32_t start_artwork_data(struct IAPContext* ctx, struct IAPSpan* request, IAPBool ext) {
    const struct IAPGetTrackArtworkDataPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
    check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
    check_ret(request_payload->format_id == 0, -IAPAckStatus_EBadParameter);
    check_ret(request_payload->offset_ms == 0, -IAPAckStatus_EBadParameter);
    check_ret(!ctx->artwork.valid, -IAPAckStatus_EBadParameter);

    check_ret(iap_platform_open_artwork(ctx, swap_32(request_payload->track_index), &ctx->artwork), -IAPAckStatus_EBadParameter);
    ctx->artwork.valid       = iap_true;
    ctx->artwork_cursor      = 0;
    ctx->artwork_chunk_index = 0;
    ctx->artwork_trans_id    = ctx->handling_trans_id;
    if(ext) {
        ctx->artwork_data_lingo   = IAPLingoID_ExtendedInterface;
        ctx->artwork_data_command = IAPExtendedInterfaceCommandID_RetTrackArtworkData;
    } else {
        ctx->artwork_data_lingo   = IAPLingoID_DisplayRemote;
        ctx->artwork_data_command = IAPDisplayRemoteCommandID_RetTrackArtworkData;
    }
    check_ret(send_artwork_chunk_cb(ctx), -IAPAckStatus_ECommandFailed);
    return 0;
}

static int32_t ipod_ack(uint16_t command, enum IAPAckStatus status, struct IAPSpan* response, uint16_t ret) {
    alloc_response(IAPIPodAckPayload, payload);
    payload->status = status;
    payload->id     = command;
    return ret;
}

static int32_t handle_command(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_RequestIPodSoftwareVersion: {
            alloc_response(IAPReturnIPodSoftwareVersionPayload, payload);
            payload->major    = 18;
            payload->minor    = 7;
            payload->revision = 2;
            return IAPGeneralCommandID_ReturnIPodSoftwareVersion;
        } break;
        case IAPGeneralCommandID_RequestIPodSerialNum: {
            check_ret(iap_platform_get_ipod_serial_num(ctx, response), -IAPAckStatus_ECommandFailed);
            return IAPGeneralCommandID_ReturnIPodSerialNum;
        } break;
        case IAPGeneralCommandID_RequestIPodModelNum: {
            static const char* model_num = "MTAY2J/A";
            check_ret(iap_span_append(response, model_num, strlen(model_num) + 1), -IAPAckStatus_EOutOfResource);
            return IAPGeneralCommandID_ReturnIPodModelNum;
        } break;
        case IAPGeneralCommandID_RequestTransportMaxPayloadSize: {
            alloc_response(IAPReturnTransportMaxPayloadSizePayload, payload);
            payload->max_payload_size = swap_16(HID_BUFFER_SIZE - 1 /*sync*/ - 1 /*sof*/ - 3 /*length*/ - 1 /*checksum*/);
            return IAPGeneralCommandID_ReturnTransportMaxPayloadSize;
        } break;
        case IAPGeneralCommandID_RequestLingoProtocolVersion: {
            static const struct {
                uint8_t major;
                uint8_t minor;
            } table[] = {
                [IAPLingoID_General]            = {1, 9},
                [IAPLingoID_Microphone]         = {1, 1},
                [IAPLingoID_SimpleRemote]       = {1, 4},
                [IAPLingoID_DisplayRemote]      = {1, 5},
                [IAPLingoID_ExtendedInterface]  = {1, 14},
                [IAPLingoID_AccessoryPower]     = {1, 1},
                [IAPLingoID_USBHostMode]        = {1, 0},
                [IAPLingoID_RFTuner]            = {1, 1},
                [IAPLingoID_AccessoryEqualizer] = {1, 0},
                [IAPLingoID_Sports]             = {1, 1},
                [IAPLingoID_DigitalAudio]       = {1, 3},
                [IAPLingoID_Storage]            = {1, 2},
                [IAPLingoID_IPodOut]            = {1, 0},
                [IAPLingoID_Location]           = {1, 0},
            };

            const struct IAPRequestLingoProtocolVersionPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(request_payload->lingo < array_size(table), -IAPAckStatus_EBadParameter);

            alloc_response(IAPReturnLingoProtocolVersionPayload, payload);
            payload->lingo = request_payload->lingo;
            payload->major = table[request_payload->lingo].major;
            payload->minor = table[request_payload->lingo].minor;
            return IAPGeneralCommandID_ReturnLingoProtocolVersion;
        } break;
        case IAPGeneralCommandID_SetUIMode: {
            const struct IAPSetUIModePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
        } break;
        case IAPGeneralCommandID_GetIPodOptionsForLingo: {
            const struct IAPGetIPodOptionsForLingoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);

            alloc_response(IAPRetIPodOptionsForLingoPayload, payload);
            payload->lingo_id = request_payload->lingo_id;
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
        case IAPGeneralCommandID_GetSupportedEventNotification: {
            alloc_response(IAPRetSupportedEventNotificationPayload, payload);
            payload->mask = swap_64(IAPSetEventNotificationEvents_FlowControl);
            return IAPGeneralCommandID_RetSupportedEventNotification;
        } break;
        case IAPGeneralCommandID_SetAvailableCurrent: {
            const struct IAPSetAvailableCurrentPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
        } break;
        case IAPGeneralCommandID_SetEventNotification: {
            const struct IAPSetEventNotificationPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
        } break;
        }
        break;
    case IAPLingoID_DisplayRemote:
        switch(command) {
        case IAPDisplayRemoteCommandID_SetCurrentEQProfileIndex: {
            const struct IAPSetCurrentEQProfileIndexPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            return ipod_ack(command, IAPAckStatus_Success, response, IAPDisplayRemoteCommandID_IPodAck);
        } break;
        case IAPDisplayRemoteCommandID_SetRemoteEventNotification: {
            const struct IAPSetRemoteEventNotificationPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            ctx->notifications_3         = 0;
            ctx->enabled_notifications_3 = swap_32(request_payload->mask);
            return ipod_ack(command, IAPAckStatus_Success, response, IAPDisplayRemoteCommandID_IPodAck);
        } break;
        case IAPDisplayRemoteCommandID_GetIPodStateInfo: {
            const struct IAPGetIPodStateInfoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(response->size >= sizeof(struct IAPIPodStatePayload), -IAPAckStatus_EOutOfResource);
            ((struct IAPIPodStatePayload*)response->ptr)->type = request_payload->type;
            switch(request_payload->type) {
            case IAPIPodStateType_TrackTimePositionMSec: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackTimePositionMSecPayload, payload);
                payload->position_ms = swap_32(status.track_pos_ms);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_TrackPlaybackIndex: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackPlaybackIndexPayload, payload);
                payload->index = swap_32(status.track_index);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_ChapterIndex: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateChapterIndexPayload, payload);
                payload->index = swap_32(status.track_index);
                /* no chapters */
                payload->chapter_count = 0;
                payload->chapter_index = -1;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_PlayStatus: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStatePlayStatusPayload, payload);
                payload->status = status.state; /* TODO: convert enum */
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_Volume: {
                struct IAPPlatformVolumeStatus status;
                check_ret(iap_platform_get_volume(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateVolumePayload, payload);
                payload->mute_state = status.muted;
                payload->ui_volume  = status.volume;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_Power: {
                struct IAPPlatformPowerStatus status;
                check_ret(iap_platform_get_power_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStatePowerPayload, payload);
                payload->power_state   = status.state; /* TODO: convert enum */
                payload->battery_level = status.battery_level;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_EQSetting: {
                alloc_response(IAPIPodStateEQSettingPayload, payload);
                /* no eq setting support yet */
                payload->eq_index = 0;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_ShuffleSetting: {
                alloc_response(IAPIPodStateShuffleSettingPayload, payload);
                check_ret(iap_platform_get_shuffle_setting(ctx, &payload->shuffle_state), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_RepeatSetting: {
                alloc_response(IAPIPodStateRepeatSettingPayload, payload);
                check_ret(iap_platform_get_repeat_setting(ctx, &payload->repeat_state), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_DateTimeSetting: {
                struct IAPDateTime time;
                check_ret(iap_platform_get_date_time(ctx, &time), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateDateTimeSettingPayload, payload);
                payload->year   = swap_16(time.year);
                payload->month  = time.month;
                payload->day    = time.day;
                payload->hour   = time.hour;
                payload->minute = time.minute;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_AlarmSetting: {
                alloc_response(IAPIPodStateAlarmSettingPayload, payload);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_BacklightLevel: {
                alloc_response(IAPIPodStateBacklightLevelPayload, payload);
                check_ret(iap_platform_get_backlight_level(ctx, &payload->level), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_HoldSwitchState: {
                IAPBool state;
                check_ret(iap_platform_get_hold_switch_state(ctx, &state), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateHoldSwitchStatePayload, payload);
                payload->state = state;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_SoundCheckState: {
                alloc_response(IAPIPodStateSoundCheckStatePayload, payload);
                payload->state = 0; /* no sound check */
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_AudiobookSpeeed: {
                alloc_response(IAPIPodStateAudiobookSpeeedPayload, payload);
                payload->speed = IAPIPodStateAudiobookSpeeed_Normal;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_TrackTimePositionSec: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackTimePositionSecPayload, payload);
                payload->position_s = swap_16(status.track_pos_ms / 1000);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_AbsoluteVolume: {
                struct IAPPlatformVolumeStatus status;
                check_ret(iap_platform_get_volume(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateAbsoluteVolumePayload, payload);
                payload->mute_state      = status.muted;
                payload->ui_volume       = status.volume;
                payload->absolute_volume = status.volume;
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_TrackCaps: {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPIPodStateTrackCapsPayload, payload);
                payload->caps = swap_32(status.track_caps);
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            case IAPIPodStateType_PlaybackEngineContents: {
                alloc_response(IAPIPodStatePlaybackEngineContentsPayload, payload);
                payload->count = 0; /* TODO: shoud be supported? */
                return IAPDisplayRemoteCommandID_RetIPodStateInfo;
            } break;
            default:
                warn("invalid request type 0x%02X", request_payload->type);
                return -IAPAckStatus_EBadParameter;
            }
        } break;
        case IAPDisplayRemoteCommandID_GetIndexedPlayingTrackInfo: {
            const struct IAPGetIndexedPlayingTrackInfoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(response->size > sizeof(struct IAPRetIndexedPlayingTrackInfoPayload), -IAPAckStatus_EOutOfResource);
            ((struct IAPRetIndexedPlayingTrackInfoPayload*)response->ptr)->type = request_payload->type;
            switch(request_payload->type) {
            case IAPIndexedPlayingTrackInfoType_TrackCapsInfo: {
                uint32_t                    length;
                uint32_t                    caps;
                struct IAPPlatformTrackInfo info = {.total_ms = &length, .caps = &caps};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPRetIndexedPlayingTrackInfoTrackCapsInfoPayload, payload);
                payload->track_caps     = swap_32(caps);
                payload->track_total_ms = swap_32(length);
                payload->chapter_count  = 0;
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_ChapterTimeName: {
                return -IAPAckStatus_EBadParameter;
            } break;
            case IAPIndexedPlayingTrackInfoType_ArtistName: {
                alloc_response(IAPRetIndexedPlayingTrackInfoArtistNamePayload, payload);
                struct IAPPlatformTrackInfo info = {.artist = response};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_AlbumName: {
                alloc_response(IAPRetIndexedPlayingTrackInfoAlbumNamePayload, payload);
                struct IAPPlatformTrackInfo info = {.album = response};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_GenreName: {
                alloc_response_extra(IAPRetIndexedPlayingTrackInfoGenreNamePayload, payload, 1);
                payload->name[0] = '\0';
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_TrackTitle: {
                alloc_response(IAPRetIndexedPlayingTrackInfoTrackTitlePayload, payload);
                struct IAPPlatformTrackInfo info = {.title = response};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_ComposerName: {
                alloc_response(IAPRetIndexedPlayingTrackInfoComposerNamePayload, payload);
                struct IAPPlatformTrackInfo info = {.composer = response};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_Lyrics: {
                alloc_response_extra(IAPRetIndexedPlayingTrackInfoLyricsPayload, payload, 1);
                payload->info_bits = 0;
                payload->index     = 0;
                payload->lyrics[0] = '\0';
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            case IAPIndexedPlayingTrackInfoType_ArtworkCount: {
                alloc_response_extra(IAPRetIndexedPlayingTrackInfoArtworkCountPayload, payload, sizeof(struct IAPArtworkCount));
                payload->data[0].format = 0;
                payload->data[0].count  = swap_16(1);
                return IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo;
            } break;
            }
        } break;
        case IAPDisplayRemoteCommandID_GetArtworkFormats: {
            alloc_response_extra(IAPArtworkFormat, payload, sizeof(struct IAPArtworkFormat));
            payload->format_id    = 0;
            payload->pixel_format = IAP_COLOR_ARTWORK ? IAPArtworkPixelFormats_RGB565LE : IAPArtworkPixelFormats_Mono;
            payload->image_width  = swap_16(IAP_ARTWORK_WIDTH);
            payload->image_height = swap_16(IAP_ARTWORK_HEIGHT);
            return IAPDisplayRemoteCommandID_RetArtworkFormats;
        } break;
        case IAPDisplayRemoteCommandID_GetTrackArtworkData: {
            const int32_t ret = start_artwork_data(ctx, request, iap_false);
            check_ret(ret == 0, ret);
            /* responded in send_artwork_chunk_cb, no need to do it here */
            response->ptr = NULL;
            return 0;
        } break;
        case IAPDisplayRemoteCommandID_GetTrackArtworkTimes: {
            const struct IAPGetTrackArtworkTimesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            const uint16_t count = swap_16(request_payload->artwork_count);
            check_ret(count == 0 || count == 1, -IAPAckStatus_ECommandFailed, "not implemented");

            void* payload = iap_span_alloc(response, sizeof(uint32_t) * count);
            check_ret(payload != NULL, iap_false);
            memset(payload, 0, sizeof(uint32_t) * count);
            return IAPDisplayRemoteCommandID_RetTrackArtworkTimes;
        } break;
        }
        break;
    case IAPLingoID_ExtendedInterface:
        switch(command) {
        case IAPExtendedInterfaceCommandID_GetCurrentPlayingTrackChapterInfo: {
            alloc_response(IAPReturnCurrentPlayingTrackChapterInfoPayload, payload);
            /* no chapters */
            payload->count = 0;
            payload->index = -1;
            return IAPExtendedInterfaceCommandID_ReturnCurrentPlayingTrackChapterInfo;
        } break;
        case IAPExtendedInterfaceCommandID_GetAudiobookSpeed: {
            alloc_response(IAPRetAudiobookSpeedPayload, payload);
            payload->speed = IAPIPodStateAudiobookSpeeed_Normal;
            return IAPExtendedInterfaceCommandID_RetAudiobookSpeed;
        } break;
        case IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackInfo: {
            const struct IAPExtendedGetIndexedPlayingTrackInfoPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(response->size > sizeof(struct IAPExtendedRetIndexedPlayingTrackInfoPayload), -IAPAckStatus_EOutOfResource);
            ((struct IAPExtendedRetIndexedPlayingTrackInfoPayload*)response->ptr)->type = request_payload->type;
            switch(request_payload->type) {
            case IAPExtendedIndexedPlayingTrackInfoType_TrackCapsInfo: {
                uint32_t                    length;
                uint32_t                    caps;
                struct IAPPlatformTrackInfo info = {.total_ms = &length, .caps = &caps};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackCapsInfoPayload, payload);
                payload->track_caps     = swap_32(caps);
                payload->track_total_ms = swap_32(length);
                payload->chapter_count  = 0;
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_PodcastName: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoPodcastNamePayload, payload, 1);
                payload->name[0] = '\0';
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackReleaseDate: {
                struct IAPDateTime          time;
                struct IAPPlatformTrackInfo info = {.release_date = &time};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackReleaseDatePayload, payload);
                payload->seconds = time.seconds;
                payload->minutes = time.minute;
                payload->hours   = time.hour;
                payload->day     = time.day;
                payload->month   = time.month;
                payload->year    = swap_16(time.year);
                payload->weekday = 0; /* TODO: set weekday? */
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackDescription: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackDescriptionPayload, payload, 1);
                payload->info_bits      = 0;
                payload->index          = 0;
                payload->description[0] = '\0';
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackSongLyrics: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackSongLyricsPayload, payload, 1);
                payload->info_bits = 0;
                payload->index     = 0;
                payload->lyrics[0] = '\0';
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackGenre: {
                alloc_response_extra(IAPExtendedRetIndexedPlayingTrackInfoTrackGenrePayload, payload, 1);
                payload->genre[0] = '\0';
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
            } break;
            case IAPExtendedIndexedPlayingTrackInfoType_TrackComposer: {
                alloc_response(IAPExtendedRetIndexedPlayingTrackInfoTrackComposerPayload, payload);
                struct IAPPlatformTrackInfo info = {.composer = response};
                check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->track_index), &info), -IAPAckStatus_ECommandFailed);
                return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo;
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
        } break;
        case IAPExtendedInterfaceCommandID_GetArtworkFormats: {
            /* same as DisplayRemote::GetArtworkFormats */
            const int32_t ret = handle_command(ctx, IAPLingoID_DisplayRemote, IAPDisplayRemoteCommandID_GetArtworkFormats, request, response);
            check_ret(ret == IAPDisplayRemoteCommandID_RetArtworkFormats, ret);
            return IAPExtendedInterfaceCommandID_RetArtworkFormats;
        } break;
        case IAPExtendedInterfaceCommandID_GetTrackArtworkData: {
            const int32_t ret = start_artwork_data(ctx, request, iap_true);
            check_ret(ret == 0, ret);
            /* responded in send_artwork_chunk_cb, no need to do it here */
            response->ptr = NULL;
            return 0;
        } break;
        case IAPExtendedInterfaceCommandID_ResetDBSelection: {
            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetNumberCategorizedDBRecords: {
            const struct IAPGetNumberCategorizedDBRecordsPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);

            uint32_t count;
            if(request_payload->type == IAPDatabaseType_Track) {
                struct IAPPlatformPlayStatus status;
                check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
                /* track_count is invalid while stopped.
                 * return non-zero dummy value in this case, because reporting zero tracks
                 * may cause empty library error. */
                /* TODO: maybe add dedicated platform callback? */
                count = status.state == IAPIPodStatePlayStatus_PlaybackStopped ? 99 : status.track_count;
            } else {
                warn("unsupported type 0x%02X", request_payload->type);
                count = 0;
            }

            alloc_response(IAPReturnNumberCategorizedDBRecordsPayload, payload);
            payload->count = swap_32(count);
            return IAPExtendedInterfaceCommandID_ReturnNumberCategorizedDBRecords;
        } break;
        case IAPExtendedInterfaceCommandID_GetPlayStatus: {
            struct IAPPlatformPlayStatus status;
            check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
            alloc_response(IAPExtendedRetPlayStatusPayload, payload);
            payload->state          = status.state; /* TODO: convert enum */
            payload->track_pos_ms   = swap_32(status.track_pos_ms);
            payload->track_total_ms = swap_32(status.track_total_ms);
            return IAPExtendedInterfaceCommandID_ReturnPlayStatus;
        } break;
        case IAPExtendedInterfaceCommandID_GetCurrentPlayingTrackIndex: {
            struct IAPPlatformPlayStatus status;
            check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
            alloc_response(IAPReturnCurrentPlayingTrackIndexPayload, payload);
            payload->index = swap_32(status.state == IAPIPodStatePlayStatus_PlaybackStopped ? -1 : status.track_index);
            return IAPExtendedInterfaceCommandID_ReturnCurrentPlayingTrackIndex;
        } break;
        case IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackTitle: {
            const struct IAPGetIndexedPlayingTrackStringPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            struct IAPPlatformTrackInfo info = {.title = response};
            check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->index), &info), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackTitle;
        } break;
        case IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackArtistName: {
            const struct IAPGetIndexedPlayingTrackStringPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            struct IAPPlatformTrackInfo info = {.artist = response};
            check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->index), &info), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackArtistName;
        } break;
        case IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackAlbumName: {
            const struct IAPGetIndexedPlayingTrackStringPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            struct IAPPlatformTrackInfo info = {.album = response};
            check_ret(iap_platform_get_indexed_track_info(ctx, swap_32(request_payload->index), &info), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackAlbumName;
        } break;
        case IAPExtendedInterfaceCommandID_SetPlayStatusChangeNotification: {
            if(request->size == sizeof(struct IAPSetPlayStatusChangeNotification1BytePayload)) {
                const struct IAPSetPlayStatusChangeNotification1BytePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
                check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
                ctx->notifications_4 = 0;
                if(request_payload->enable) {
                    ctx->enabled_notifications_4 = 1 << IAPStatusChangeNotificationType_PlaybackStopped |
                                                   1 << IAPStatusChangeNotificationType_TrackIndex |
                                                   1 << IAPStatusChangeNotificationType_PlaybackFEWSeekStop |
                                                   1 << IAPStatusChangeNotificationType_PlaybackREWSeekStop |
                                                   1 << IAPStatusChangeNotificationType_TrackTimeOffsetMSec |
                                                   1 << IAPStatusChangeNotificationType_ChapterIndex;
                } else {
                    ctx->enabled_notifications_4 = 0;
                }
            } else if(request->size == sizeof(struct IAPSetPlayStatusChangeNotification4BytesPayload)) {
                const struct IAPSetPlayStatusChangeNotification4BytesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
                check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
                ctx->enabled_notifications_4 = play_stage_change_notification_set_mask_to_type_mask(swap_32(request_payload->mask));
            }
            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_PlayCurrentSelection: {
            const struct IAPPlayCurrentSelectionPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            const struct IAPPlatformPendingControl       pending         = {
                              .req_command = command,
                              .ack_command = IAPExtendedInterfaceCommandID_IPodAck,
                              .trans_id    = ctx->handling_trans_id,
                              .lingo       = lingo,
            };
            iap_platform_control(ctx, IAPPlatformControl_Play, pending);
            response->ptr = NULL;
            return 0;
        } break;
        case IAPExtendedInterfaceCommandID_PlayControl: {
            const struct IAPPlayControlPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            static const int enum_table[][2] = {
                {IAPPlayControlCode_TogglePlayPause, IAPPlatformControl_TogglePlayPause},
                {IAPPlayControlCode_Stop, IAPPlatformControl_Stop},
                {IAPPlayControlCode_NextTrack, IAPPlatformControl_Next},
                {IAPPlayControlCode_PrevTrack, IAPPlatformControl_Prev},
                {IAPPlayControlCode_StartFF, -1},
                {IAPPlayControlCode_StartRew, -1},
                {IAPPlayControlCode_EndFFRew, -1},
                {IAPPlayControlCode_Next, IAPPlatformControl_Next},
                {IAPPlayControlCode_Prev, IAPPlatformControl_Prev},
                {IAPPlayControlCode_Play, IAPPlatformControl_Play},
                {IAPPlayControlCode_Pause, IAPPlatformControl_Pause},
                {IAPPlayControlCode_NextChapter, IAPPlatformControl_Next},
                {IAPPlayControlCode_PrevChapter, IAPPlatformControl_Prev},
                {IAPPlayControlCode_ResumeIPod, -1},
            };
            int control = -1;
            for(size_t i = 0; i < array_size(enum_table); i += 1) {
                if(enum_table[i][0] == request_payload->code) {
                    control = enum_table[i][1];
                    break;
                }
            }
            if(control >= 0) {
                const struct IAPPlatformPendingControl pending = {
                    .req_command = command,
                    .ack_command = IAPExtendedInterfaceCommandID_IPodAck,
                    .trans_id    = ctx->handling_trans_id,
                    .lingo       = lingo,
                };
                iap_platform_control(ctx, control, pending);
                response->ptr = NULL;
                return 0;
            }

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetShuffle: {
            alloc_response(IAPReturnShufflePayload, payload);
            check_ret(iap_platform_get_shuffle_setting(ctx, &payload->mode), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnShuffle;
        } break;
        case IAPExtendedInterfaceCommandID_SetShuffle: {
            const struct IAPSetShufflePayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(iap_platform_set_shuffle_setting(ctx, request_payload->mode), -IAPAckStatus_ECommandFailed);

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetRepeat: {
            alloc_response(IAPReturnRepeatPayload, payload);
            check_ret(iap_platform_get_repeat_setting(ctx, &payload->mode), -IAPAckStatus_ECommandFailed);
            return IAPExtendedInterfaceCommandID_ReturnRepeat;
        } break;
        case IAPExtendedInterfaceCommandID_SetRepeat: {
            const struct IAPSetRepeatPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(iap_platform_set_repeat_setting(ctx, request_payload->mode), -IAPAckStatus_ECommandFailed);

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        case IAPExtendedInterfaceCommandID_GetNumPlayingTracks: {
            struct IAPPlatformPlayStatus status;
            check_ret(iap_platform_get_play_status(ctx, &status), -IAPAckStatus_ECommandFailed);
            alloc_response(IAPRetNumPlayingTracksPayload, payload);
            payload->num_playing_tracks = swap_32(status.track_count);
            return IAPExtendedInterfaceCommandID_ReturnNumPlayingTracks;
        } break;
        case IAPExtendedInterfaceCommandID_SetCurrentPlayingTrack: {
            const struct IAPSetCurrentPlayingTrackPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(iap_platform_set_playing_track(ctx, swap_32(request_payload->index)), -IAPAckStatus_ECommandFailed);

            alloc_response(IAPExtendedIPodAckPayload, payload);
            payload->status = IAPAckStatus_Success;
            payload->id     = swap_16(command);
            return IAPExtendedInterfaceCommandID_IPodAck;
        } break;
        }
        break;
    case IAPLingoID_DigitalAudio:
        switch(command) {
        case IAPDigitalAudioCommandID_AccessoryAck: {
            const struct IAPAccAckPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);

            response->ptr = NULL;
            check_ret(request_payload->status == IAPAckStatus_Success, 0);
            return 0;
        } break;
        case IAPDigitalAudioCommandID_RetAccessorySampleRateCaps: {
            check_ret(iap_platform_on_acc_samprs_received(ctx, request), -IAPAckStatus_ECommandFailed);
            response->ptr = NULL; /* no response */
            return 0;
        } break;
        }
        break;
    }

    return -IAPAckStatus_EUnknownID;
}

static IAPBool transition_idps_to_auth_cb(struct IAPContext* ctx) {
    print("starting accessory authentication");
    ctx->phase = IAPPhase_Auth;
    check_ret(_iap_send_packet(ctx, IAPLingoID_General, IAPGeneralCommandID_GetAccessoryAuthenticationInfo, _iap_next_trans_id(ctx), _iap_get_buffer_for_send_payload(ctx).ptr), iap_false);
    return iap_true;
}

static int32_t handle_in_connected(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_IdentifyDeviceLingoes: {
            const struct IAPIdentifyDeviceLingoesPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            switch(swap_32(request_payload->options)) {
            case IAPIdentifyDeviceLingoesOptions_NoAuth:
                break;
            case IAPIdentifyDeviceLingoesOptions_DeferAuth:
                warn("unsupported option 0x%04X", swap_32(request_payload->options));
                return -IAPAckStatus_EBadParameter;
            case IAPIdentifyDeviceLingoesOptions_ImmediateAuth:
                ctx->on_send_complete = transition_idps_to_auth_cb;
                break;
            }
            return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
        } break;
        case IAPGeneralCommandID_StartIDPS: {
            ctx->phase = IAPPhase_IDPS;
            return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
        } break;
        }
        break;
    }
    return -IAPAckStatus_EUnknownID;
}

static int32_t handle_in_idps(struct IAPContext* ctx, uint8_t lingo, uint16_t command, struct IAPSpan* request, struct IAPSpan* response) {
    switch(lingo) {
    case IAPLingoID_General:
        switch(command) {
        case IAPGeneralCommandID_SetFIDTokenValues: {
            const int ret = _iap_hanlde_set_fid_token_values(request, response);
            check_ret(ret == 0, ret);
            return IAPGeneralCommandID_AckFIDTokenValues;
        } break;
        case IAPGeneralCommandID_EndIDPS: {
            const struct IAPEndIDPSPayload* request_payload = iap_span_read(request, sizeof(*request_payload));
            check_ret(request_payload != NULL, -IAPAckStatus_EBadParameter);
            check_ret(request_payload->status == IAPEndIDPSStatus_Success, -IAPAckStatus_ECommandFailed);
            ctx->on_send_complete = transition_idps_to_auth_cb;
            alloc_response(IAPIDPSStatusPayload, payload);
            payload->status = IAPIDPSStatus_Success;
            return IAPGeneralCommandID_IDPSStatus;
        } break;
        }
        break;
    }
    return iap_false;
}

static IAPBool send_auth_challenge_sig_cb(struct IAPContext* ctx) {
    check_ret(ctx->phase == IAPPhase_Auth, iap_false);
    struct IAPSpan                     request = _iap_get_buffer_for_send_payload(ctx);
    struct IAPGetAccAuthSigPayload2p0* payload = iap_span_alloc(&request, sizeof(*payload));
    check_ret(payload != NULL, iap_false);
    payload->retry = 1;
    check_ret(_iap_send_packet(ctx, IAPLingoID_General, IAPGeneralCommandID_GetAccessoryAuthenticationSignature, _iap_next_trans_id(ctx), request.ptr), iap_false);
    return iap_true;
}

static IAPBool send_sample_rate_caps_cb(struct IAPContext* ctx) {
    struct IAPSpan request = _iap_get_buffer_for_send_payload(ctx);
    check_ret(_iap_send_packet(ctx, IAPLingoID_DigitalAudio, IAPDigitalAudioCommandID_GetAccessorySampleRateCaps, _iap_next_trans_id(ctx), request.ptr), iap_false);
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
            /* iap_platform_dump_hex(request->ptr, request->size); */
            if(request_payload->cert_current_section_index < request_payload->cert_max_section_index) {
                return ipod_ack(command, IAPAckStatus_Success, response, IAPGeneralCommandID_IPodAck);
            } else {
                ctx->on_send_complete = send_auth_challenge_sig_cb;
                alloc_response(IAPAckAccAuthInfoPayload, payload);
                payload->status = IAPAckAccAuthInfoStatus_Supported;
                return IAPGeneralCommandID_AckAccessoryAuthenticationInfo;
            }
        } break;
        case IAPGeneralCommandID_RetAccessoryAuthenticationSignature: {
            print("accessory signature");
            /* iap_platform_dump_hex(request->ptr, request->size); */

            alloc_response(IAPAckAccAuthSigPayload, payload);
            payload->status = IAPAckStatus_Success;

            ctx->phase            = IAPPhase_Authed;
            ctx->on_send_complete = send_sample_rate_caps_cb;

            return IAPGeneralCommandID_AckAccessoryAuthenticationStatus;
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

    /* request handling */
    if(ctx->trans_id_support == TransIDUnknown) {
        check_ret(lingo == IAPLingoID_General, iap_false);
        if(command == IAPGeneralCommandID_StartIDPS) {
            ctx->trans_id_support = TransIDSupported;
        }
        if(command == IAPGeneralCommandID_IdentifyDeviceLingoes) {
            ctx->trans_id_support  = TransIDNotSupported;
            ctx->handling_trans_id = -1;
        } else {
            warn("the first command(%02X:%04X) must be StartIDPS or IdentifyDeviceLingoes", lingo, command);
            return iap_false;
        }
    }
    if(ctx->trans_id_support == TransIDSupported) {
        check_ret(iap_span_read_16(&request, &buf.u16), iap_false);
        ctx->handling_trans_id = buf.u16;
    }

    IAP_LOGF("==== acc ====");
    _iap_dump_packet(lingo, command, ctx->handling_trans_id, request);

    struct IAPSpan response = _iap_get_buffer_for_send_payload(ctx);
    int32_t        ret      = handle_command(ctx, lingo, command, &request, &response);
    if(response.ptr == NULL) {
        /* handler disabled response */
        return iap_true;
    }
    if(ret >= 0) {
        /* handled successfully */
        goto respond;
    }
    if(ret != -IAPAckStatus_EUnknownID) {
        /* handled, but error */
        goto error_ack;
    }

    /* not a standard request, try authentication handlers */
    ret      = -IAPAckStatus_EBadParameter;
    response = _iap_get_buffer_for_send_payload(ctx);
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
    }
    if(response.ptr == NULL) {
        /* handler disabled response */
        return iap_true;
    }
    if(ret >= 0) {
        /* handled successfully */
        goto respond;
    }
error_ack:
    /* handling failed, replace response with ipod ack */
    warn("command handling failed 0x%02X(%s):0x%04X", lingo, _iap_lingo_str(lingo), command);
    response = _iap_get_buffer_for_send_payload(ctx);
    ret      = build_ipod_ack_response(lingo, command, -ret, &response);
    check_ret(ret >= 0, iap_false);
respond:
    check_ret(_iap_send_packet(ctx, lingo, ret, ctx->handling_trans_id, response.ptr), iap_false);
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

int32_t _iap_next_trans_id(struct IAPContext* ctx) {
    if(ctx->trans_id_support == TransIDSupported) {
        return ctx->trans_id += 1;
    } else {
        return -1;
    }
}

IAPBool _iap_send_packet(struct IAPContext* ctx, uint8_t lingo, uint16_t command, int32_t trans_id, uint8_t* final_ptr) {
    uint8_t* ptr          = _iap_get_buffer_for_send_payload(ctx).ptr;
    size_t   payload_size = final_ptr - ptr;

    {
        IAP_LOGF("==== dev ====");
        struct IAPSpan payload_span = {
            .ptr  = _iap_get_buffer_for_send_payload(ctx).ptr,
            .size = payload_size,
        };
        _iap_dump_packet(lingo, command, trans_id, payload_span);
    }

#define pack_8(val) \
    ptr -= 1;       \
    *(uint8_t*)ptr = val;
#define pack_16(val) \
    ptr -= 2;        \
    *(uu16*)ptr = swap_16(val);

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

static IAPBool push_active_event(struct IAPContext* ctx, struct IAPActiveEvent event) {
    if(!ctx->send_busy) {
        check_ret(event.callback(ctx, &event), iap_false);
        return iap_true;
    }

    for(size_t i = 0; i < array_size(ctx->active_events); i += 1) {
        if(ctx->active_events[i].callback == NULL) {
            ctx->active_events[i] = event;
            return iap_true;
        }
    }
    return iap_false;
}

static IAPBool process_select_sampr(struct IAPContext* ctx, struct IAPActiveEvent* event) {
    struct IAPSpan                            request = _iap_get_buffer_for_send_payload(ctx);
    struct IAPTrackNewAudioAttributesPayload* payload = iap_span_alloc(&request, sizeof(*payload));
    payload->sample_rate                              = swap_32(event->sampr);
    payload->sound_check                              = 0;
    payload->volume_adjustment                        = 0;
    check_ret(_iap_send_packet(ctx, IAPLingoID_DigitalAudio, IAPDigitalAudioCommandID_TrackNewAudioAttributes, _iap_next_trans_id(ctx), request.ptr), iap_false);
    return iap_true;
}

IAPBool iap_select_sampr(struct IAPContext* ctx, uint32_t sampr) {
    struct IAPActiveEvent event = {
        .callback = process_select_sampr,
        .sampr    = sampr,
    };
    check_ret(push_active_event(ctx, event), iap_false);
    return iap_true;
}

static IAPBool process_control_response(struct IAPContext* ctx, struct IAPActiveEvent* event) {
    struct IAPSpan                          request = _iap_get_buffer_for_send_payload(ctx);
    const struct IAPPlatformPendingControl* ctrl    = &event->control_response.control;
    if(ctrl->lingo == IAPLingoID_ExtendedInterface) {
        struct IAPExtendedIPodAckPayload* payload = iap_span_alloc(&request, sizeof(*payload));
        payload->status                           = event->control_response.result ? IAPAckStatus_Success : IAPAckStatus_ECommandFailed;
        payload->id                               = swap_16(ctrl->req_command);
    } else {
        struct IAPIPodAckPayload* payload = iap_span_alloc(&request, sizeof(*payload));
        payload->status                   = event->control_response.result ? IAPAckStatus_Success : IAPAckStatus_ECommandFailed;
        payload->id                       = ctrl->req_command;
    }
    check_ret(_iap_send_packet(ctx, ctrl->lingo, ctrl->ack_command, ctrl->trans_id, request.ptr), iap_false);
    return iap_true;
}

IAPBool iap_control_response(struct IAPContext* ctx, struct IAPPlatformPendingControl pending, IAPBool result) {
    struct IAPActiveEvent event = {
        .callback         = process_control_response,
        .control_response = {pending, result},
    };
    check_ret(push_active_event(ctx, event), iap_false);

    return iap_true;
}

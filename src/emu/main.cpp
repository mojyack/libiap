#include <iostream>
#include <optional>
#include <vector>

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "cast.hpp"
#include "endian.hpp"
#include "iap.hpp"
#include "macros/assert.hpp"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "pw.hpp"
#include "spec/iap.h"
#include "util/argument-parser.hpp"
#include "util/charconv.hpp"
#include "util/concat.hpp"
#include "util/coroutine.hpp"
#include "util/critical.hpp"
#include "util/hexdump.hpp"
#include "util/pair-table.hpp"
#include "util/split.hpp"

// hid.cpp
extern bool hs;

auto parse_hid_report(BytesArray& buf, BytesRef ref) -> bool;
auto encode_to_hid_reports(BytesRef ref) -> std::vector<BytesArray>;

namespace {
declare_autoptr(SndPCM, snd_pcm_t, snd_pcm_close);

auto find_rockbox_card_index() -> std::optional<int> {
    auto name = (char*)(nullptr);
    for(auto i = 0;; i += 1) {
        const auto ret = snd_card_get_name(i, &name);
        ensure(ret == 0);
        PRINT("{} name {}", i, name);
        const auto found = std::string_view(name) == "Rockbox media player";
        free(name);
        if(found) {
            return i;
        }
    }
}

auto to_bytes(std::string_view str) -> std::optional<std::vector<std::byte>> {
    auto ret = std::vector<std::byte>();
    for(const auto e : split(str, ",")) {
        unwrap(n, from_chars<uint8_t>(e, 16));
        ret.push_back(std::byte(n));
    }
    return ret;
}

auto iap_fd   = 0;
auto trans_id = uint16_t(0);
auto hidraw   = true;

auto send_command(const uint16_t lingo, const uint16_t command, const void* const payload = nullptr, const size_t payload_size = 0) -> bool {
    auto frame   = build_iap_frame(lingo, command, trans_id, payload, payload_size);
    auto reports = encode_to_hid_reports(frame);
    for(auto& report : reports) {
        std::println("==== acc ====");
        dump_hex(report);
        if(hidraw) {
            report = concat(std::array{std::byte{0}}, report);
        }
        const auto ret = write(iap_fd, report.data(), report.size());
        ensure(ret == (int)report.size(), "{} != {} {}({})", ret, report.size(), errno, strerror(errno));
    }
    trans_id += 1;
    return true;
}

template <class T>
auto push_payload(BytesArray& buf, T data) -> void {
    const auto prev_size = buf.size();
    buf.resize(prev_size + sizeof(T));
    memcpy(buf.data() + prev_size, &data, sizeof(T));
}

auto push_string(BytesArray& buf, const std::string_view str) -> void {
    const auto prev_size = buf.size();
    buf.resize(prev_size + str.size() + 1);
    memcpy(buf.data() + prev_size, str.data(), str.size() + 1);
}

template <uint16_t lingo, uint16_t command, class T>
auto extract_payload(const ParsedIAPFrame& frame) -> const T* {
    ensure(frame.lingo == lingo);
    ensure(frame.command == command);
    unwrap(payload, bytes_as<T>(frame.payload));
    return &payload;
}

#define cmd(Lingo, Command) IAPLingoID_##Lingo, IAP##Lingo##CommandID_##Command

auto auth_task_main(const ParsedIAPFrame& frame) -> CoGenerator<bool> {
    constexpr auto error_value = false;

    {
        co_ensure_v(send_command(cmd(General, StartIDPS)));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<cmd(General, IPodAck), IAPIPodAckPayload>(frame)));
        co_ensure_v(payload.status == IAPAckStatus_Success);
    }

    {
        co_ensure_v(send_command(cmd(General, RequestTransportMaxPayloadSize)));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<cmd(General, ReturnTransportMaxPayloadSize), IAPReturnTransportMaxPayloadSizePayload>(frame)));
        PRINT("max payload size {}", swap(payload.max_payload_size));
    }

    for(const auto lingo : std::array{
            IAPLingoID_General,
            IAPLingoID_SimpleRemote,
            IAPLingoID_DisplayRemote,
            IAPLingoID_ExtendedInterface,
            IAPLingoID_DigitalAudio,
            IAPLingoID_IPodOut,
        }) {
        const auto request = IAPGetIPodOptionsForLingoPayload{
            .lingo_id = uint8_t(lingo),
        };
        co_ensure_v(send_command(cmd(General, GetIPodOptionsForLingo), &request, 1));
        co_yield true;
        auto payload = extract_payload<cmd(General, RetIPodOptionsForLingo), IAPRetIPodOptionsForLingoPayload>(frame);
        if(payload) {
            co_ensure_v(payload->lingo_id == lingo);
            PRINT("options for lingo {:02X}: {:08X}", payload->lingo_id, payload->bits);
        } else {
            PRINT("lingo {:02X} not supported", int(lingo));
        }
    }

    {
        auto request = BytesArray();

        push_payload(request, IAPSetFIDTokenValuesPayload{.num_token_values = 0});

        const auto lingoes = std::array{
            uint8_t(IAPLingoID_General),
            uint8_t(IAPLingoID_SimpleRemote),
            uint8_t(IAPLingoID_DisplayRemote),
            uint8_t(IAPLingoID_ExtendedInterface),
        };
        push_payload(request, IAPFIDTokenValuesIdentifyTokenHead{
                                  .length = sizeof(IAPFIDTokenValuesIdentifyTokenHead) - 1 +
                                            lingoes.size() +
                                            sizeof(IAPFIDTokenValuesIdentifyTokenTail),
                                  .type        = IAPFIDTokenTypes_Identify >> 8,
                                  .subtype     = IAPFIDTokenTypes_Identify & 0xff,
                                  .num_lingoes = lingoes.size(),
                              });
        push_payload(request, lingoes);
        push_payload(request, IAPFIDTokenValuesIdentifyTokenTail{
                                  .device_option = swap(uint32_t(0b10)),
                                  .device_id     = 0xdeadbeaf,
                              });
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        push_payload(request, IAPFIDTokenValuesAccCapsToken{
                                  .length    = sizeof(IAPFIDTokenValuesAccCapsToken) - 1,
                                  .type      = IAPFIDTokenTypes_AccCaps >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccCaps & 0xff,
                                  .caps_bits = 0,
                              });
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto name = std::string_view("AV Receiver");
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length    = uint8_t(sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                                       name.size() + 1),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_AccName,
                              });
        push_string(request, name);
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto fw_version = std::array<uint8_t, 3>{1, 0, 0};
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length = sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                            fw_version.size(),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_FirmwareVersion,
                              });
        push_payload(request, fw_version);
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto hw_version = std::array<uint8_t, 3>{1, 0, 0};
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length = sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                            hw_version.size(),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_HardwareVersion,
                              });
        push_payload(request, hw_version);
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto manufacture = std::string_view("Pioneer");
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length    = uint8_t(sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                                       manufacture.size() + 1),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_Manufacture,
                              });
        push_string(request, manufacture);
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto model = std::string_view("DB091");
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length    = uint8_t(sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                                       model.size() + 1),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_ModelNumber,
                              });
        push_string(request, model);
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        const auto rf_cert = uint32_t(0);
        push_payload(request, IAPFIDTokenValuesAccInfoToken{
                                  .length    = uint8_t(sizeof(IAPFIDTokenValuesAccInfoToken) - 1 +
                                                       sizeof(rf_cert)),
                                  .type      = IAPFIDTokenTypes_AccInfo >> 8,
                                  .subtype   = IAPFIDTokenTypes_AccInfo & 0xff,
                                  .info_type = IAPFIDTokenValuesAccInfoTypes_RFCerts,
                              });
        push_payload(request, swap(rf_cert));
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        push_payload(request, IAPFIDTokenValuesIPodPreferenceToken{
                                  .length          = sizeof(IAPFIDTokenValuesIPodPreferenceToken) - 1,
                                  .type            = IAPFIDTokenTypes_IPodPreference >> 8,
                                  .subtype         = IAPFIDTokenTypes_IPodPreference & 0xff,
                                  .class_id        = IAPIPodPereferenceClassID_VideoOut,
                                  .setting_id      = IAPIPodPreferenceVideoOutSettingID_On,
                                  .restore_on_exit = 1,
                              });
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        push_payload(request, IAPFIDTokenValuesIPodPreferenceToken{
                                  .length          = sizeof(IAPFIDTokenValuesIPodPreferenceToken) - 1,
                                  .type            = IAPFIDTokenTypes_IPodPreference >> 8,
                                  .subtype         = IAPFIDTokenTypes_IPodPreference & 0xff,
                                  .class_id        = IAPIPodPereferenceClassID_VideoFormat,
                                  .setting_id      = IAPIPodPreferenceVideoFormatSettingID_NTSC,
                                  .restore_on_exit = 1,
                              });
        std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values += 1;

        co_ensure_v(send_command(cmd(General, SetFIDTokenValues), request.data(), request.size()));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<cmd(General, AckFIDTokenValues), IAPAckFIDTokenValuesPayload>(frame)));
        co_ensure_v(payload.num_token_value_acks == std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values);
    }
    {
        const auto request = IAPEndIDPSPayload{.status = IAPEndIDPSStatus_Success};
        co_ensure_v(send_command(cmd(General, EndIDPS), &request, sizeof(request)));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<cmd(General, IDPSStatus), IAPIDPSStatusPayload>(frame)));
        co_ensure_v(payload.status == IAPIDPSStatus_Success);
    }

    // auth
    co_yield true;
    {
        co_ensure_v(frame.lingo == IAPLingoID_General);
        co_ensure_v(frame.command == IAPGeneralCommandID_GetAccessoryAuthenticationInfo);

        auto request = BytesArray();
        push_payload(request, IAPRetAccAuthInfoPayload2p0{
                                  .protocol_major             = 2,
                                  .protocol_minor             = 0,
                                  .cert_current_section_index = 1,
                                  .cert_max_section_index     = 1,
                              });
        push_payload(request, std::array<uint8_t, 4>{1, 2, 3, 4});
        co_ensure_v(send_command(cmd(General, RetAccessoryAuthenticationInfo), request.data(), request.size()));

        co_yield true;

        co_unwrap_v(response, (extract_payload<cmd(General, AckAccessoryAuthenticationInfo), IAPAckAccAuthInfoPayload>(frame)));
        co_ensure_v(response.status == IAPAckAccAuthInfoStatus_Supported);
    }
    co_yield true;
    {
        co_unwrap_v(request, (extract_payload<cmd(General, GetAccessoryAuthenticationSignature), IAPGetAccAuthSigPayload2p0>(frame)));
        PRINT("requested challenge:");
        dump_hex(std::span(request.challenge));

        auto response = std::array<uint8_t, 4>{5, 6, 7, 8};
        co_ensure_v(send_command(cmd(General, RetAccessoryAuthenticationSignature), response.data(), response.size()));
        co_yield true;

        co_unwrap_v(ack, (extract_payload<cmd(General, AckAccessoryAuthenticationStatus), IAPAckAccAuthSigPayload>(frame)));
        co_ensure_v(ack.status == IAPAckStatus_Success);
    }
    co_return true;
}

constexpr auto pfds_stdin = 0;
constexpr auto pfds_iap   = 1;
constexpr auto pfds_alsa  = 2;

auto pfds = std::array{
    pollfd{.fd = fileno(stdin), .events = POLLIN},
    pollfd{.events = POLLIN}, /* iAP HID */
    pollfd{},                 /* ALSA capture */
};

auto snd    = AutoSndPCM();
auto pw_ctx = (pw::Context*)(nullptr);

auto start_audio(const uint32_t sample_rate) -> bool {
    PRINT("starting audio samplr={}", sample_rate);
    /* capture */
    snd.reset();
    unwrap(card, find_rockbox_card_index());
    const auto card_name = std::format("hw:{},0", card);
    ensure(snd_pcm_open(std::inout_ptr(snd), card_name.data(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) == 0, "{}({})", errno, strerror(errno));
    ensure(snd_pcm_set_params(snd.get(), SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, sample_rate, 0, 1'000'000) == 0);
    ensure(snd_pcm_prepare(snd.get()) == 0);
    ensure(snd_pcm_poll_descriptors_count(snd.get()) == 1);
    snd_pcm_poll_descriptors(snd.get(), &pfds[pfds_alsa], 1);
    ensure(snd_pcm_start(snd.get()) == 0);

    /* playback */
    if(pw_ctx != nullptr) {
        pw::finish(pw_ctx);
    }
    pw_ctx = pw::init(0, sample_rate);
    ensure(pw_ctx != nullptr);
    pw::run(pw_ctx);
    return true;
}

auto handle_frame(ParsedIAPFrame frame) -> bool {
    PRINT("handling 0x{:02X}:0x{:04X}", frame.lingo, frame.command);
    switch(frame.lingo) {
    case IAPLingoID_DisplayRemote:
        switch(frame.command) {
        case IAPDisplayRemoteCommandID_RemoteEventNotification: {
            unwrap(resp, bytes_as<IAPIPodStatePayload>(frame.payload));
            switch(resp.type) {
            case IAPIPodStateType_TrackTimePositionMSec: {
                unwrap(resp, bytes_as<IAPIPodStateTrackTimePositionMSecPayload>(frame.payload));
                PRINT("notify: track time position {}ms", swap(resp.position_ms));
            } break;
            case IAPIPodStateType_TrackPlaybackIndex: {
                unwrap(resp, bytes_as<IAPIPodStateTrackPlaybackIndexPayload>(frame.payload));
                PRINT("notify: track playback index {}", swap(resp.index));
            } break;
            case IAPIPodStateType_PlaybackEngineContents: {
                unwrap(resp, bytes_as<IAPIPodStatePlaybackEngineContentsPayload>(frame.payload));
                PRINT("notify: track playback count {}", swap(resp.count));
            } break;
            case IAPIPodStateType_PlayStatus: {
                unwrap(resp, bytes_as<IAPIPodStatePlayStatusPayload>(frame.payload));
                PRINT("notify: playback status {}", swap(resp.status));
            } break;
            case IAPIPodStateType_Volume: {
                unwrap(resp, bytes_as<IAPIPodStateVolumePayload>(frame.payload));
                PRINT("notify: volume {} mute {}", resp.ui_volume, resp.mute_state);
            } break;
            case IAPIPodStateType_Power: {
                unwrap(resp, bytes_as<IAPIPodStatePowerPayload>(frame.payload));
                PRINT("notify: power {} battery {}", resp.power_state, resp.battery_level);
            } break;
            case IAPIPodStateType_ShuffleSetting: {
                unwrap(resp, bytes_as<IAPIPodStateShuffleSettingPayload>(frame.payload));
                PRINT("notify: shuffle {}", resp.shuffle_state);
            } break;
            case IAPIPodStateType_RepeatSetting: {
                unwrap(resp, bytes_as<IAPIPodStateRepeatSettingPayload>(frame.payload));
                PRINT("notify: repeat {}", resp.repeat_state);
            } break;
            case IAPIPodStateType_DateTimeSetting: {
                unwrap(resp, bytes_as<IAPIPodStateDateTimeSettingPayload>(frame.payload));
                PRINT("notify: datetime {}-{}-{} {}:{}", swap(resp.year), resp.month, resp.day, resp.hour, resp.day);
            } break;
            case IAPIPodStateType_HoldSwitchState: {
                unwrap(resp, bytes_as<IAPIPodStateHoldSwitchStatePayload>(frame.payload));
                PRINT("notify: holdswitch {}", resp.state);
            } break;
            case IAPIPodStateType_TrackTimePositionSec: {
                unwrap(resp, bytes_as<IAPIPodStateTrackTimePositionSecPayload>(frame.payload));
                PRINT("notify: track time position {}s", swap(resp.position_s));
            } break;
            default:
                PRINT("unhandled notification type={}", resp.type);
                break;
            }
            return true;
        } break;
        }
        break;
    case IAPLingoID_ExtendedInterface:
        switch(frame.command) {
        case IAPExtendedInterfaceCommandID_IPodAck: {
            unwrap(ack, bytes_as<IAPExtendedIPodAckPayload>(frame.payload));
            PRINT("extended ack command=0x{:04X} status=0x{:02X}", ack.id, ack.status);
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo: {
            unwrap(resp, bytes_as<IAPExtendedRetIndexedPlayingTrackInfoPayload>(frame.payload));
            switch(resp.type) {
            case IAPExtendedIndexedPlayingTrackInfoType_TrackReleaseDate: {
                unwrap(resp, bytes_as<IAPExtendedRetIndexedPlayingTrackInfoTrackReleaseDatePayload>(frame.payload));
                PRINT("track release date {}-{}-{} {}:{}.{} week {}", swap(resp.year), resp.month, resp.day, resp.hours, resp.minutes, resp.seconds, resp.weekday);
                return true;
            } break;
            default:
                bail("unhandled track info type={}", resp.type);
            }
        } break;
        case IAPExtendedInterfaceCommandID_ReturnPlayStatus: {
            unwrap(resp, bytes_as<IAPExtendedRetPlayStatusPayload>(frame.payload));
            PRINT("play status st={} pos={}/{}", resp.state, swap(resp.track_pos_ms), swap(resp.track_total_ms));
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnCurrentPlayingTrackIndex: {
            unwrap(resp, bytes_as<IAPReturnCurrentPlayingTrackIndexPayload>(frame.payload));
            PRINT("track index {}", swap(resp.index));
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackTitle: {
            ensure(char(frame.payload.back()) == '\0');
            PRINT("track title: {}", std::bit_cast<const char*>(frame.payload.data()));
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackArtistName: {
            ensure(char(frame.payload.back()) == '\0');
            PRINT("track artist: {}", std::bit_cast<const char*>(frame.payload.data()));
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackAlbumName: {
            ensure(char(frame.payload.back()) == '\0');
            PRINT("track album: {}", std::bit_cast<const char*>(frame.payload.data()));
            return true;
        } break;
        case IAPExtendedInterfaceCommandID_ReturnNumPlayingTracks: {
            unwrap(resp, bytes_as<IAPRetNumPlayingTracksPayload>(frame.payload));
            PRINT("track count {}", swap(resp.num_playing_tracks));
            return true;
        } break;
        }
        break;
    case IAPLingoID_DigitalAudio:
        switch(frame.command) {
        case IAPDigitalAudioCommandID_IPodAck: {
            unwrap(ack, bytes_as<IAPAckAccAuthSigPayload>(frame.payload));
            ensure(ack.status == IAPAckStatus_Success);
            return true;
        } break;
        case IAPDigitalAudioCommandID_GetAccessorySampleRateCaps: {
            const auto response = std::array{
                swap(uint32_t(32000)),
                swap(uint32_t(44100)),
                swap(uint32_t(48000)),
            };
            ensure(send_command(cmd(DigitalAudio, RetAccessorySampleRateCaps), &response, sizeof(response)));
            return true;
        } break;
        case IAPDigitalAudioCommandID_TrackNewAudioAttributes: {
            unwrap(payload, bytes_as<IAPTrackNewAudioAttributesPayload>(frame.payload));
            PRINT("sample rate={}", swap(payload.sample_rate));

            ensure(start_audio(swap(payload.sample_rate)));

            const auto response = IAPAccAckPayload{
                .status = IAPAckStatus_Success,
                .id     = uint8_t(frame.command),
            };
            ensure(send_command(cmd(DigitalAudio, AccessoryAck), &response, sizeof(response)));
            return true;
        } break;
        }
        break;
    }
    return false;
}

auto handle_stdin(const std::string_view input) -> bool {
    if(input.empty()) {
        return true;
    }
    const auto elms = split(input, " ");
    if(elms[0] == "raw") {
        ensure(elms.size() == 2);
        unwrap(bin, to_bytes(elms[1]));
        PRINT("wrote {} bytes ({})", write(iap_fd, bin.data(), bin.size()), strerror(errno));
    } else if(elms[0] == "sampr") {
        ensure(elms.size() == 2);
        unwrap(sampr, from_chars<uint32_t>(elms[1]));
        ensure(start_audio(sampr));
    } else if(elms[0] == "play") {
        const auto request = IAPPlayControlPayload{IAPPlayControlCode_Play};
        ensure(send_command(cmd(ExtendedInterface, PlayControl), &request, sizeof(request)));
    } else if(elms[0] == "ctrl") {
        ensure(elms.size() == 2);
        static const auto table = make_pair_table<std::string_view, uint8_t>({
            {"play", IAPPlayControlCode_Play},
            {"stop", IAPPlayControlCode_Stop},
            {"pause", IAPPlayControlCode_Pause},
            {"toggle", IAPPlayControlCode_TogglePlayPause},
            {"next", IAPPlayControlCode_Next},
            {"prev", IAPPlayControlCode_Prev},
        });
        ensure(elms.size() == 2);
        unwrap(act, table.find(elms[1]), "invalid control {}", elms[1]);
        const auto request = IAPPlayControlPayload{act};
        ensure(send_command(cmd(ExtendedInterface, PlayControl), &request, sizeof(request)));
    } else if(elms[0] == "status") {
        ensure(send_command(cmd(ExtendedInterface, GetPlayStatus)));
    } else if(elms[0] == "index") {
        ensure(send_command(cmd(ExtendedInterface, GetCurrentPlayingTrackIndex)));
    } else if(elms[0] == "count") {
        ensure(send_command(cmd(ExtendedInterface, GetNumPlayingTracks)));
    } else if(elms[0] == "string") { /* string INDEX TYPE */
        ensure(elms.size() == 3);
        unwrap(index, from_chars<uint32_t>(elms[1]));
        static const auto table = make_pair_table<std::string_view, uint8_t>({
            {"title", IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackTitle},
            {"album", IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackAlbumName},
            {"artist", IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackArtistName},
        });
        unwrap(command, table.find(elms[2]), "invalid track info {}", elms[2]);
        const auto request = IAPGetIndexedPlayingTrackStringPayload{.index = swap(index)};
        ensure(send_command(IAPLingoID_ExtendedInterface, command, &request, sizeof(request)));
    } else if(elms[0] == "info") { /* info INDEX */
        ensure(elms.size() == 3);
        unwrap(index, from_chars<uint32_t>(elms[1]));
        static const auto table = make_pair_table<std::string_view, uint8_t>({
            {"release", IAPExtendedIndexedPlayingTrackInfoType_TrackReleaseDate},
        });
        unwrap(type, table.find(elms[2]), "invalid track info {}", elms[2]);
        const auto request = IAPExtendedGetIndexedPlayingTrackInfoPayload{
            .type          = type,
            .track_index   = swap(index),
            .chapter_index = 0,
        };
        ensure(send_command(cmd(ExtendedInterface, GetIndexedPlayingTrackInfo), &request, sizeof(request)));
    } else if(elms[0] == "notify") { /* notify NAME */
        ensure(elms.size() == 2);
        static const auto table = make_pair_table<std::string_view, uint8_t>({
            {"msec", IAPIPodStateType_TrackTimePositionMSec},
            {"index", IAPIPodStateType_TrackPlaybackIndex},
            {"count", IAPIPodStateType_PlaybackEngineContents},
            {"status", IAPIPodStateType_PlayStatus},
            {"volume", IAPIPodStateType_Volume},
            {"power", IAPIPodStateType_Power},
            {"shuffle", IAPIPodStateType_ShuffleSetting},
            {"repeat", IAPIPodStateType_RepeatSetting},
            {"date", IAPIPodStateType_DateTimeSetting},
            {"hold", IAPIPodStateType_HoldSwitchState},
            {"sec", IAPIPodStateType_TrackTimePositionSec},
        });
        unwrap(type, table.find(elms[1]), "invalid notification name {}", elms[1]);

        static auto set = uint32_t(0);

        const auto mask    = (1u << type);
        set                = (set & mask) ? set & ~mask : set | mask;
        const auto request = IAPSetRemoteEventNotificationPayload{
            .mask = swap(set),
        };
        ensure(send_command(cmd(DisplayRemote, SetRemoteEventNotification), &request, sizeof(request)));
    } else {
        bail("invalid command {}", elms[0]);
    }
    return true;
}
} // namespace

auto prebuffering     = true;
auto critical_pcm_buf = Critical<std::vector<int16_t>>();
auto total_captured   = 0;
auto total_played     = 0;
auto epoch            = std::chrono::system_clock::now();

constexpr auto buffer_min = 48000;
constexpr auto buffer_max = 48000 * 2;

auto tick() -> bool {
    static auto time = std::chrono::system_clock::now();
    const auto  now  = std::chrono::system_clock::now();
    if(now - time >= std::chrono::seconds(1)) {
        time = now;
        return true;
    } else {
        return false;
    }
}

namespace pw {
auto on_capture(const int16_t*, const size_t, const size_t) -> void {
}

auto on_playback(int16_t* const buffer, const size_t num_samples) -> size_t {
    auto [lock, pcm_buf] = critical_pcm_buf.access();

    if(prebuffering && pcm_buf.size() <= buffer_min) {
        return 0;
    } else if(pcm_buf.empty()) {
        prebuffering = true;
        return 0;
    }
    prebuffering = false;

    const auto copy = std::min(pcm_buf.size(), num_samples);
    std::memcpy(buffer, pcm_buf.data(), copy * sizeof(int16_t));
    pcm_buf.erase(pcm_buf.begin(), pcm_buf.begin() + copy);
    total_played += copy / 2;
    return copy;
}
} // namespace pw

auto main(const int argc, const char* const* argv) -> int {
    auto hiddev   = (const char*)(nullptr);
    auto authed   = false;
    auto no_audio = false;
    {
        auto parser = args::Parser<>();
        parser.arg(&hiddev, "PATH", "path to hid device");
        parser.kwflag(&authed, {"-a"}, "assume authed");
        parser.kwflag(&no_audio, {"-n"}, "disable audio streaming");
        parser.kwflag(&hs, {"-f"}, "assume usb fullspeed", {.invert_flag_value = true});
        ensure(parser.parse(argc, argv));
    }
    iap_fd = open(hiddev, O_RDWR);
    ensure(iap_fd >= 0);

    pfds[pfds_iap].fd = iap_fd;

    auto hid_buf   = BytesArray();
    auto iap_frame = ParsedIAPFrame();
    auto auth_task = CoRoutine<bool>(auth_task_main(iap_frame));

    if(!authed) {
        auth_task.resume();
    } else if(!no_audio) {
        ensure(start_audio(48000));
    }

loop:
    const auto ret = poll(pfds.data(), pfds.size(), -1);
    ensure(ret > 0);
    if(pfds[pfds_stdin].revents & POLLIN) {
        auto line = std::string();
        std::getline(std::cin, line);
        handle_stdin(line);
    }
    if(pfds[pfds_iap].revents & POLLIN) {
        auto buf = std::array<std::byte, 1024>();
        auto len = read(iap_fd, buf.data(), buf.size());
        std::println("==== dev ==== {} bytes", len);
        dump_hex(std::span(buf.data(), len));
        if(len > 0 && parse_hid_report(hid_buf, BytesRef(buf.data(), len))) {
            unwrap(frame, parse_iap_frame(hid_buf));
            iap_frame = frame;
            if(!authed) {
                ensure(auth_task.resume());
                if(auth_task.done()) {
                    PRINT("authentication done");
                    authed = true;
                }
            } else {
                handle_frame(iap_frame);
            }
            hid_buf.clear();
        }
    }

    if(!snd) {
        goto loop;
    }

    auto revents = (unsigned short)(0);
    ensure(snd_pcm_poll_descriptors_revents(snd.get(), &pfds[pfds_alsa], 1, &revents) == 0);
    if(revents & POLLIN) {
        const auto frames = snd_pcm_avail(snd.get());
        if(frames <= 0) {
            ensure(snd_pcm_recover(snd.get(), frames, 0) == 0);
            ensure(snd_pcm_start(snd.get()) == 0);
            goto loop;
        }
        auto [lock, pcm_buf] = critical_pcm_buf.access();
        if(pcm_buf.size() + frames * 2 > buffer_max) {
            PRINT("overflow");
            pcm_buf.resize(std::min<int>(buffer_max - frames * 2, 0));
        }
        const auto prev_size = pcm_buf.size();
        pcm_buf.resize(prev_size + frames * 2);
        const auto ret = snd_pcm_readi(snd.get(), &pcm_buf[prev_size], frames);
        if(ret > 0) {
            total_captured += ret;
        } else if(ret == -EPIPE) {
            ensure(snd_pcm_recover(snd.get(), frames, 0) == 0);
            ensure(snd_pcm_start(snd.get()) == 0);
        } else {
            bail("alsa error {}({})", ret, strerror(-ret));
        }
    } else if(revents & POLLERR) {
        return -1;
    }

    if(tick()) {
        PRINT("stat: captured={} played={} remain={}", total_captured, total_played, critical_pcm_buf.unsafe_access().size());
        total_played   = 0;
        total_captured = 0;
    }

    goto loop;

    return 0;
}

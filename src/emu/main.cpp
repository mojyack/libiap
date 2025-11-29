#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "cast.hpp"
#include "endian.hpp"
#include "iap.hpp"
#include "macros/unwrap.hpp"
#include "pw.hpp"
#include "spec/iap.h"
#include "util/argument-parser.hpp"
#include "util/charconv.hpp"
#include "util/concat.hpp"
#include "util/coroutine.hpp"
#include "util/hexdump.hpp"
#include "util/split.hpp"

// hid.cpp
auto parse_hid_report(BytesArray& buf, BytesRef ref) -> bool;
auto encode_to_hid_reports(BytesRef ref) -> std::vector<BytesArray>;

namespace {
auto to_bytes(std::string_view str) -> std::optional<std::vector<std::byte>> {
    auto ret = std::vector<std::byte>();
    for(const auto e : split(str, " ")) {
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

auto auth_task_main(const ParsedIAPFrame& frame) -> CoGenerator<bool> {
    constexpr auto error_value = false;

    {
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_StartIDPS));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_IPodAck, IAPIPodAckPayload>(frame)));
        co_ensure_v(payload.status == IAPAckStatus_Success);
    }

    {
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_RequestTransportMaxPayloadSize));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_ReturnTransportMaxPayloadSize, IAPReturnTransportMaxPayloadSizePayload>(frame)));
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
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_GetIPodOptionsForLingo, &request, 1));
        co_yield true;
        auto payload = extract_payload<IAPLingoID_General, IAPGeneralCommandID_RetIPodOptionsForLingo, IAPRetIPodOptionsForLingoPayload>(frame);
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

        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_SetFIDTokenValues, request.data(), request.size()));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_AckFIDTokenValues, IAPAckFIDTokenValuesPayload>(frame)));
        co_ensure_v(payload.num_token_value_acks == std::bit_cast<IAPSetFIDTokenValuesPayload*>(request.data())->num_token_values);
    }
    {
        const auto request = IAPEndIDPSPayload{.status = IAPEndIDPSStatus_Success};
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_EndIDPS, &request, sizeof(request)));
        co_yield true;
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_IDPSStatus, IAPIDPSStatusPayload>(frame)));
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
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_RetAccessoryAuthenticationInfo, request.data(), request.size()));

        co_yield true;

        co_unwrap_v(response, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_AckAccessoryAuthenticationInfo, IAPAckAccAuthInfoPayload>(frame)));
        co_ensure_v(response.status == IAPAckAccAuthInfoStatus_Supported);
    }
    co_yield true;
    {
        co_unwrap_v(request, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_GetAccessoryAuthenticationSignature, IAPGetAccAuthSigPayload2p0>(frame)));
        PRINT("requested challenge:");
        dump_hex(std::span(request.challenge));

        auto response = std::array<uint8_t, 4>{5, 6, 7, 8};
        co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_RetAccessoryAuthenticationSignature, response.data(), response.size()));
        co_yield true;

        co_unwrap_v(ack, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_AckAccessoryAuthenticationStatus, IAPAckAccAuthSigPayload>(frame)));
        co_ensure_v(ack.status == IAPAckStatus_Success);
    }
    co_return true;
}

auto pw_ctx    = (pw::Context*)(nullptr);
auto pw_thread = std::thread();

auto start_audio(const uint32_t sample_rate) -> bool {
    ensure(pw_ctx == nullptr);
    pw_ctx = pw::init(sample_rate, 444100);
    ensure(pw_ctx != nullptr);
    pw_thread = std::thread(pw::run, pw_ctx);
    return true;
}

auto handle_frame(ParsedIAPFrame frame) -> bool {
    PRINT("handling {:02X}:{:04X}", frame.lingo, frame.command);
    switch(frame.lingo) {
    case IAPLingoID_DigitalAudio:
        switch(frame.command) {
        case IAPDigitalAudioCommandID_IPodAck: {
            unwrap(ack, (extract_payload<IAPLingoID_DigitalAudio, IAPDigitalAudioCommandID_IPodAck, IAPAckAccAuthSigPayload>(frame)));
            ensure(ack.status == IAPAckStatus_Success);
            return true;
        } break;
        case IAPDigitalAudioCommandID_GetAccessorySampleRateCaps: {
            const auto response = std::array{
                swap(uint32_t(32000)),
                swap(uint32_t(44100)),
                swap(uint32_t(48000)),
            };
            ensure(send_command(frame.lingo, IAPDigitalAudioCommandID_RetAccessorySampleRateCaps, &response, sizeof(response)));
            return true;
        } break;
        case IAPDigitalAudioCommandID_TrackNewAudioAttributes: {
            unwrap(payload, (extract_payload<IAPLingoID_DigitalAudio, IAPDigitalAudioCommandID_TrackNewAudioAttributes, IAPTrackNewAudioAttributesPayload>(frame)));
            PRINT("sample rate={}", swap(payload.sample_rate));

            ensure(start_audio(swap(payload.sample_rate)));

            const auto response = IAPAccAckPayload{
                .status = IAPAckStatus_Success,
                .id     = uint8_t(frame.command),
            };
            ensure(send_command(frame.lingo, IAPDigitalAudioCommandID_AccessoryAck, &response, sizeof(response)));
            return true;
        } break;
        }
        break;
    }
    return false;
}
} // namespace

namespace pw {
auto pcm_buf = std::vector<int16_t>();
auto on_capture(const int16_t* buffer, const size_t num_samples, const size_t num_channels) -> void {
    // PRINT("got {} samples", num_samples);
    pcm_buf = concat<int16_t>(pcm_buf, std::span(buffer, num_samples * num_channels));
}

auto on_playback(int16_t* const buffer, const size_t num_samples) -> size_t {
    if(pcm_buf.empty()) {
        return 0;
    }

    const auto copy = std::min(pcm_buf.size(), num_samples);
    std::memcpy(buffer, pcm_buf.data(), copy * sizeof(int16_t));
    pcm_buf.erase(pcm_buf.begin(), pcm_buf.begin() + copy);
    PRINT("requested {}, copyied {}", num_samples, copy);
    return copy;
}
} // namespace pw

auto main(const int argc, const char* const* argv) -> int {
    auto hiddev = (const char*)(nullptr);
    auto authed = false;
    {
        auto parser = args::Parser<>();
        parser.arg(&hiddev, "PATH", "path to hid device");
        parser.kwflag(&authed, {"-a"}, "assume authed");
        ensure(parser.parse(argc, argv));
    }
    iap_fd = open(hiddev, O_RDWR);
    ensure(iap_fd >= 0);

    auto pfds = std::array{
        pollfd{.fd = fileno(stdin), .events = POLLIN},
        pollfd{.fd = iap_fd, .events = POLLIN},
    };

    auto hid_buf   = BytesArray();
    auto iap_frame = ParsedIAPFrame();
    auto auth_task = CoRoutine<bool>(auth_task_main(iap_frame));

    if(!authed) {
        auth_task.resume();
    } else {
        ensure(start_audio(44100));
    }

loop:
    const auto ret = poll(pfds.data(), pfds.size(), -1);
    ensure(ret > 0);
    if(pfds[0].revents & POLLIN) {
        auto line = std::string();
        std::getline(std::cin, line);
        if(auto a = to_bytes(line)) {
            PRINT("wrote {} bytes ({})", write(iap_fd, a->data(), a->size()), strerror(errno));
        }
    }
    if(pfds[1].revents & POLLIN) {
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
                ensure(handle_frame(iap_frame));
            }
            hid_buf.clear();
        }
    }
    goto loop;

    return 0;
}

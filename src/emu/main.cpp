#include <iostream>
#include <optional>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "cast.hpp"
#include "endian.h"
#include "endian.hpp"
#include "iap.hpp"
#include "macros/unwrap.hpp"
#include "spec/iap.h"
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

auto send_command(const uint16_t lingo, const uint16_t command, const void* const payload = nullptr, const size_t payload_size = 0) -> bool {
    const auto frame   = build_iap_frame(lingo, command, trans_id, payload, payload_size);
    const auto reports = encode_to_hid_reports(frame);
    for(const auto& report : reports) {
        ensure(write(iap_fd, report.data(), report.size()) == (int)report.size());
    }
    trans_id += 1;
    return true;
}

template <class T>
auto push_payload(BytesArray& buf, const T data) -> void {
    const auto prev_size = buf.size();
    buf.resize(prev_size + sizeof(T));
    memcpy(buf.data() + prev_size, &data, sizeof(T));
}

template <uint16_t lingo, uint16_t command, class T>
auto extract_payload(const ParsedIAPFrame& frame) -> const T* {
    ensure(frame.command == lingo);
    ensure(frame.command == command);
    unwrap(payload, bytes_as<T>(frame.payload));
    return &payload;
}

auto iap_task_main(const ParsedIAPFrame& frame) -> CoGenerator<bool> {
    constexpr auto error_value = false;

    co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_StartIDPS));
    co_yield true;
    {
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_IPodAck, IAPIPodAckPayload>(frame)));
        co_ensure_v(payload.status == IAPAckStatus_Success);
    }

    co_ensure_v(send_command(IAPLingoID_General, IAPGeneralCommandID_RequestTransportMaxPayloadSize));
    co_yield true;
    {
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
        co_unwrap_v(payload, (extract_payload<IAPLingoID_General, IAPGeneralCommandID_RetIPodOptionsForLingo, IAPRetIPodOptionsForLingoPayload>(frame)));
        co_ensure_v(payload.lingo_id == IAPLingoID_General);
        PRINT("options for lingo {:02X}: {:08X}", payload.lingo_id, payload.bits);
    }

    {
        auto request = BytesArray();
        auto lingoes = std::array{uint8_t(IAPLingoID_General)};
        push_payload(request, IAPFIDTokenValuesIdentifyTokenHead{
                                  .length = sizeof(IAPFIDTokenValuesIdentifyTokenHead) +
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
    }
    co_return true;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    const int fd = open(argv[1], O_RDWR);
    ensure(fd >= 0);

    auto pfds = std::array{
        pollfd{.fd = fileno(stdin), .events = POLLIN},
        pollfd{.fd = fd, .events = POLLIN},
    };

    auto hid_buf  = BytesArray();
    auto iap_task = CoRoutine<bool>(iap_task_main(hid_buf));

loop:
    const auto ret = poll(pfds.data(), pfds.size(), -1);
    ensure(ret > 0);
    if(pfds[0].revents & POLLIN) {
        auto line = std::string();
        std::getline(std::cin, line);
        if(auto a = to_bytes(line)) {
            PRINT("wrote {} bytes ({})", write(fd, a->data(), a->size()), strerror(errno));
        }
    }
    if(pfds[1].revents & POLLIN) {
        auto buf = std::array<std::byte, 1024>();
        auto len = read(fd, buf.data(), buf.size());
        PRINT("read {} bytes:", len);
        if(len > 0 && parse_hid_report(hid_buf, BytesRef(buf.data(), len))) {
            iap_task.resume();
        }
    }
    goto loop;
    return 0;
}

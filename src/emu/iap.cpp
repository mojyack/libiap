#include "spec/iap.h"
#include "endian.hpp"
#include "iap.hpp"
#include "macros/unwrap.hpp"
#include "util/concat.hpp"

namespace {
template <class T>
using UA = __attribute__((aligned(1))) T;

template <class T>
auto pop(BytesRef& ref) -> std::optional<T> {
    ensure(ref.size() >= sizeof(T));
    const auto var = swap(*((UA<T>*)ref.data()));
    ref            = ref.subspan(sizeof(T));
    return var;
}
} // namespace

auto parse_iap_frame(BytesRef ref) -> std::optional<ParsedIAPFrame> {
    unwrap(sof, pop<uint8_t>(ref));
    ensure(sof == IAP_SOF_BYTE);

    // verify checksum
    ensure(ref.size() >= 2);
    auto checksum = uint8_t(0);
    for(const auto b : ref) {
        checksum += uint8_t(b);
    }
    ensure(checksum == 0);

    auto length = uint16_t();
    unwrap(length_short, pop<uint8_t>(ref));
    if(length_short != 0) {
        length = length_short;
    } else {
        unwrap(length_long, pop<uint16_t>(ref));
        length = length_long;
    }

    unwrap(lingo, pop<uint8_t>(ref));

    auto command = uint16_t();
    if(lingo == IAPLingoID_ExtendedInterface) {
        unwrap(c, pop<uint16_t>(ref));
        command = c;
    } else {
        unwrap(c, pop<uint8_t>(ref));
        command = c;
    }

    auto payload = BytesRef(ref.data(), length - 1 - (lingo == IAPLingoID_ExtendedInterface ? 2 : 1));

    // assume trans id
    unwrap(trans_id, pop<uint16_t>(payload));

    return ParsedIAPFrame{lingo, command, trans_id, payload};
}

auto build_iap_frame(uint16_t lingo, uint16_t command, int32_t trans_id, const void* payload, size_t payload_size) -> BytesArray {
    auto ret = BytesArray();
#define pack_8(val) \
    ret.push_back(std::byte(val));
#define pack_16(val)                               \
    ret.push_back(std::byte((val & 0xff00) >> 8)); \
    ret.push_back(std::byte(val & 0x00ff));

    pack_8(IAP_SOF_BYTE);
    const auto length = 1 /*lingo*/ + (lingo == IAPLingoID_ExtendedInterface ? 2 : 1) /*command*/ + (trans_id >= 0 ? 2 : 0) + payload_size;
    if(length <= 0xfc) {
        pack_8(length);
    } else {
        pack_8(0);
        pack_16(length);
    }
    pack_8(lingo);
    if(lingo == IAPLingoID_ExtendedInterface) {
        pack_16(command);
    } else {
        pack_8(command);
    }
    if(trans_id >= 0) {
        pack_16(trans_id);
    }

    ret = concat(ret, BytesRef((std::byte*)payload, payload_size));

    auto checksum = uint8_t(0);
    for(const auto b : std::span(ret).subspan(1)) {
        checksum += uint8_t(b);
    }
    checksum *= -1;
    pack_8(checksum);

    return ret;
}

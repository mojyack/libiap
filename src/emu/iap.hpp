#pragma once
#include <optional>

#include "types.hpp"

struct ParsedIAPFrame {
    uint16_t lingo;
    uint16_t command;
    int32_t  trans_id;
    BytesRef payload;
};

auto parse_iap_frame(BytesRef ref) -> std::optional<ParsedIAPFrame>;
auto build_iap_frame(uint16_t lingo, uint16_t command, int32_t trans_id, const void* payload = nullptr, size_t payload_size = 0) -> BytesArray;

#include <vector>

#include "cast.hpp"
#include "macros/unwrap.hpp"
#include "spec/hid.h"
#include "util/concat.hpp"

auto hs = true;

namespace {
struct ReportSize {
    uint8_t  id;
    uint16_t size; /* including link control byte */
};

const auto input_report_size_table_fs = std::array{
    ReportSize{.id = 0x01, .size = 0x0C},
    ReportSize{.id = 0x02, .size = 0x0E},
    ReportSize{.id = 0x03, .size = 0x14},
    ReportSize{.id = 0x04, .size = 0x3F},
};

const auto output_report_size_table_fs = std::array{
    ReportSize{.id = 0x05, .size = 0x08},
    ReportSize{.id = 0x06, .size = 0x0A},
    ReportSize{.id = 0x07, .size = 0x0E},
    ReportSize{.id = 0x08, .size = 0x14},
    ReportSize{.id = 0x09, .size = 0x3F},
};

const auto input_report_size_table_hs = std::array{
    ReportSize{.id = 0x01, .size = 0x0005},
    ReportSize{.id = 0x02, .size = 0x0009},
    ReportSize{.id = 0x03, .size = 0x000D},
    ReportSize{.id = 0x04, .size = 0x0011},
    ReportSize{.id = 0x05, .size = 0x0019},
    ReportSize{.id = 0x06, .size = 0x0031},
    ReportSize{.id = 0x07, .size = 0x005F},
    ReportSize{.id = 0x08, .size = 0x00C1},
    ReportSize{.id = 0x09, .size = 0x0101},
    ReportSize{.id = 0x0A, .size = 0x0181},
    ReportSize{.id = 0x0B, .size = 0x0201},
    ReportSize{.id = 0x0C, .size = 0x02FF},
};

const auto output_report_size_table_hs = std::array{
    ReportSize{.id = 0x0D, .size = 0x05},
    ReportSize{.id = 0x0E, .size = 0x09},
    ReportSize{.id = 0x1F, .size = 0x0D},
    ReportSize{.id = 0x10, .size = 0x11},
    ReportSize{.id = 0x11, .size = 0x19},
    ReportSize{.id = 0x12, .size = 0x31},
    ReportSize{.id = 0x13, .size = 0x5F},
    ReportSize{.id = 0x14, .size = 0xC1},
    ReportSize{.id = 0x15, .size = 0xFF},
};

auto find_input_report_size( const uint8_t id) -> std::optional<uint8_t> {
    const auto ptr = hs ? input_report_size_table_hs.data() : input_report_size_table_fs.data();
    const auto len = hs ? input_report_size_table_hs.size() : input_report_size_table_fs.size();
    for(auto i = 0uz; i < len; i += 1) {
        if(ptr[i].id == id) {
            return ptr[i].size;
        }
    }
    return std::nullopt;
}

auto find_optimal_report_size( const size_t size) -> ReportSize {
    const auto ptr = hs ? output_report_size_table_hs.data() : output_report_size_table_fs.data();
    const auto len = hs ? output_report_size_table_hs.size() : output_report_size_table_fs.size();
    for(auto i = 0uz; i < len; i += 1) {
        if(ptr[i].size >= size + 1 /* link control byte*/) {
            return ptr[i];
        }
    }
    return ptr[len - 1];
}
} // namespace

auto parse_hid_report(BytesArray& buf, BytesRef ref) -> bool {
    unwrap(report, bytes_as<IAPHIDReport>(ref));
    unwrap(report_size, find_input_report_size(report.report_id));
    ensure(report_size == ref.size() - 1);

    const auto payload_size = report_size - 1;

    if(!(report.link_control & IAPHIDReportLinkControlBits_Continue)) {
        /* not continue, first packet */
        buf.clear();
    } else {
        ensure(!buf.empty());
    }
    buf = concat(buf, std::span(report.data, payload_size));
    return !(report.link_control & IAPHIDReportLinkControlBits_MoreToFollow);
}

auto encode_to_hid_reports(BytesRef ref) -> std::vector<BytesArray> {
    auto ret = std::vector<BytesArray>();

    for(auto i = 0uz; i < ref.size();) {
        const auto left        = ref.size() - i;
        const auto report_size = find_optimal_report_size(left);
        const auto copy_size   = std::min<size_t>(report_size.size - 1, left);

        const auto is_first = ret.empty();
        const auto is_last  = i + copy_size == ref.size();

        auto& report = ret.emplace_back(1 + report_size.size);
        report[0]    = std::byte(report_size.id);
        report[1]    = std::byte((!is_first ? IAPHIDReportLinkControlBits_Continue : 0) |
                                 (!is_last ? IAPHIDReportLinkControlBits_MoreToFollow : 0));
        memcpy(&report[2], &ref[i], copy_size);
        i += copy_size;
    }

    return ret;
}

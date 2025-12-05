#include <vector>

#include "cast.hpp"
#include "macros/unwrap.hpp"
#include "spec/hid.h"
#include "util/concat.hpp"

namespace {
struct ReportSize {
    uint8_t id;
    uint8_t size; /* including link control byte */
};

const auto output_report_size_table = std::array{
    ReportSize{.id = 0x05, .size = 0x08},
    ReportSize{.id = 0x06, .size = 0x0A},
    ReportSize{.id = 0x07, .size = 0x0E},
    ReportSize{.id = 0x08, .size = 0x14},
    ReportSize{.id = 0x09, .size = 0x3F},
};

/* sorted by size */
const auto input_report_size_table = std::array{
    ReportSize{.id = 0x01, .size = 0x0C},
    ReportSize{.id = 0x02, .size = 0x0E},
    ReportSize{.id = 0x03, .size = 0x14},
    ReportSize{.id = 0x04, .size = 0x3F},
};

auto find_input_report_size(const uint8_t id) -> std::optional<uint8_t> {
    for(const auto e : input_report_size_table) {
        if(e.id == id) {
            return e.size;
        }
    }
    return std::nullopt;
}

auto find_optimal_report_size(const size_t size) -> ReportSize {
    for(const auto& e : output_report_size_table) {
        if(e.size >= size + 1 /* link control byte*/) {
            return e;
        }
    }
    return output_report_size_table.back();
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

#include "track.hpp"
#include "macros/unwrap.hpp"
#include "util/charconv.hpp"
#include "util/split.hpp"

namespace {
auto set_comment_value(std::string& dest, std::string_view comment, std::string_view key) -> void {
    if(comment.starts_with(key)) {
        dest = std::string(comment.substr(key.size()));
    }
}
} // namespace

auto build_track(AudioFile audio) -> std::optional<Track> {
    auto ret = Track{
        .total_samples = audio.total_samples,
        .sample_rate   = audio.sample_rate,
        .channels      = audio.channels,
        .cover         = std::move(audio.cover),
    };
    // parse comments
    auto release = std::string();
    for(const auto& comment : audio.comments) {
        set_comment_value(ret.album, comment, "ALBUM=");
        set_comment_value(ret.artist, comment, "ARTIST=");
        set_comment_value(ret.title, comment, "TITLE=");
        set_comment_value(release, comment, "DATE=");
    }
    if(!release.empty()) {
        const auto elms = split(release, "-");
        ensure(elms.size() == 3);
        unwrap(year, from_chars<uint16_t>(elms[0]));
        unwrap(month, from_chars<uint8_t>(elms[1]));
        unwrap(day, from_chars<uint8_t>(elms[2]));
        ret.year  = year;
        ret.month = month;
        ret.day   = day;
    }
    return ret;
}

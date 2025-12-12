#pragma once
#include "flac.hpp"

struct Track {
    std::string            file;
    size_t                 total_samples;
    uint32_t               sample_rate;
    uint8_t                channels;
    std::vector<int16_t>   data;
    std::vector<std::byte> cover;
    std::string            album;
    std::string            artist;
    std::string            title;
    uint16_t               year  = 0;
    uint8_t                month = 0;
    uint8_t                day   = 0;
};

auto build_track(AudioFile audio) -> std::optional<Track>;

constexpr auto samples_to_ms(const size_t samples) -> size_t {
    return 1000 * samples / 44100 / 2 /* LR */;
}

#pragma once
#include "flac.hpp"

struct Track {
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

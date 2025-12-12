#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct AudioFile {
    size_t                   total_samples;
    uint32_t                 sample_rate;
    uint8_t                  channels;
    std::vector<int16_t>     data;
    std::vector<std::byte>   cover;
    std::vector<std::string> comments;
};

auto decode_flac(const char* path) -> std::optional<AudioFile>;

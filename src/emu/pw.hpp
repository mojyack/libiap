#pragma once
#include <cstddef>
#include <cstdint>

namespace pw {
struct Context;

auto init(size_t capture_rate, size_t playback_rate) -> Context*;
auto run(Context* context) -> void;
auto finish(Context* context) -> void;

// callbacks
auto on_capture(const int16_t* buffer, size_t num_samples, size_t num_channels) -> void;
auto on_playback(int16_t* buffer, size_t num_samples) -> size_t; // returns copied samples
} // namespace sound


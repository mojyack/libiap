#pragma once
#include "track.hpp"

enum class PlayState {
    Stopped = 0,
    Playing,
    Paused,
};

struct Context {
    std::vector<Track> tracks;
    size_t             current_track;
    size_t             pcm_cursor;
    PlayState          play_state = PlayState::Stopped;

    auto set_state(PlayState new_state) -> bool;
    auto skip_track(int diff) -> bool;
};

#pragma once
#include "track.hpp"

struct Context {
    std::vector<Track> tracks;
    size_t             current_track;
    size_t             pcm_cursor;
};

inline auto context = Context();

#include <iostream>

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "flac.hpp"
#include "iap/iap.h"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "platform.hpp"
#include "util/charconv.hpp"
#include "util/hexdump.hpp"
#include "util/split.hpp"

declare_autoptr(SndPCM, snd_pcm_t, snd_pcm_close);

namespace {
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

auto tracks        = std::vector<Track>();
auto current_track = 0uz;
auto pcm_cursor    = 0uz;

auto set_comment_value(std::string& dest, std::string_view comment, std::string_view key) -> void {
    if(comment.starts_with(key)) {
        dest = std::string(comment.substr(key.size()));
    }
}

auto build_track(AudioFile audio) -> std::optional<Track> {
    auto ret = Track{
        .total_samples = audio.total_samples,
        .sample_rate   = audio.sample_rate,
        .channels      = audio.channels,
        .data          = std::move(audio.data),
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

auto read_stdin() -> bool {
    auto line = std::string();
    std::getline(std::cin, line);
    return true;
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    auto snd = AutoSndPCM();
    ensure(snd_pcm_open(std::inout_ptr(snd), "hw:0,0", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0);
    ensure(snd_pcm_set_params(snd.get(), SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 0, 50000) == 0);
    const auto pcm_pfds_count = snd_pcm_poll_descriptors_count(snd.get());

    ensure(argc > 1);
    for(auto i = 1; i < argc; i += 1) {
        unwrap_mut(audio, decode_flac(argv[1]));
        unwrap_mut(track, build_track(std::move(audio)));
        PRINT("title={}", track.title);
        PRINT("album={}", track.album);
        PRINT("artist={}", track.artist);
        PRINT("date={}-{}-{}", track.year, track.month, track.day);
        PRINT("samples={}", track.total_samples);
        tracks.push_back(std::move(track));
    }

    auto platform = LinuxPlatformData{
        .fd = open("/dev/iap0", O_RDWR | O_NONBLOCK),
    };
    ensure(platform.fd >= 0);
    auto ctx = IAPContext{.platform = &platform};
    ensure(iap_init_ctx(&ctx));

    auto  pfds      = std::vector<pollfd>(2 + pcm_pfds_count);
    auto& stdin_pfd = pfds[0];
    auto& iap_pfd   = pfds[1];
    stdin_pfd       = {.fd = fileno(stdin), .events = POLLIN};
    iap_pfd         = {.fd = platform.fd};
    snd_pcm_poll_descriptors(snd.get(), &pfds[2], pfds.size() - 2);
    ensure(fcntl(fileno(stdin), F_SETFL, O_NONBLOCK) == 0);
loop:
    iap_pfd.events = ctx.send_busy ? POLLIN | POLLOUT : POLLIN;
    ensure(poll(pfds.data(), pfds.size(), -1) >= 0);
    if(stdin_pfd.revents & POLLIN) {
        ensure(read_stdin());
    }
    if(iap_pfd.revents & POLLOUT) {
        iap_notify_send_complete(&ctx);
    }
    if(iap_pfd.revents & POLLIN) {
        auto       buf = std::array<uint8_t, 256>();
        const auto ret = read(platform.fd, buf.data(), buf.size());
        ensure(ret > 0);
        std::println("====== acc: {} bytes ======", ret);
        dump_hex(std::span{buf.data(), size_t(ret)});
        iap_feed_hid_report(&ctx, buf.data(), ret);
    }
    auto revents = (unsigned short)(0);
    ensure(snd_pcm_poll_descriptors_revents(snd.get(), &pfds[2], pcm_pfds_count, &revents) == 0);
    if(revents & POLLOUT) {
        const auto& pcm = tracks[current_track].data;
        const auto  ret = snd_pcm_writei(snd.get(), pcm.data() + pcm_cursor, pcm.size() - pcm_cursor);
        ensure(ret > 0);
    }
    goto loop;
    return 0;
}

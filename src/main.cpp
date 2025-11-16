#include <iostream>

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "context.hpp"
#include "flac.hpp"
#include "iap/iap.h"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "platform.hpp"
#include "util/hexdump.hpp"

declare_autoptr(SndPCM, snd_pcm_t, snd_pcm_close);

namespace {
auto read_stdin() -> bool {
    auto line = std::string();
    std::getline(std::cin, line);
    return true;
}

auto snd = AutoSndPCM();
auto ctx = (Context*)(nullptr);
} // namespace

auto Context::set_state(const PlayState new_state) -> bool {
    switch(play_state) {
    case PlayState::Stopped:
        switch(new_state) {
        case PlayState::Stopped:
            break;
        case PlayState::Playing:
        case PlayState::Paused:
            ctx->current_track = 0;
            ctx->pcm_cursor    = 0;
            ensure(snd_pcm_prepare(snd.get()) == 0);
            break;
        }
    case PlayState::Playing:
        switch(new_state) {
        case PlayState::Stopped:
        case PlayState::Paused:
            ensure(snd_pcm_drop(snd.get()) == 0);
            break;
        case PlayState::Playing:
            break;
        }
    case PlayState::Paused:
        switch(new_state) {
        case PlayState::Stopped:
            break;
        case PlayState::Playing:
        case PlayState::Paused:
            ensure(snd_pcm_prepare(snd.get()) == 0);
            break;
        }
    }
    play_state = new_state;
    return true;
}

auto main(const int argc, const char* const* argv) -> int {
    auto platform = LinuxPlatformData{
        .fd = open("/dev/iap0", O_RDWR | O_NONBLOCK),
    };
    ensure(platform.fd >= 0);
    auto iap_ctx = IAPContext{.platform = &platform};
    ensure(iap_init_ctx(&iap_ctx));

    ctx = &platform.ctx;

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
        ctx->tracks.push_back(std::move(track));
    }

    auto  pfds      = std::vector<pollfd>(2 + pcm_pfds_count);
    auto& stdin_pfd = pfds[0];
    auto& iap_pfd   = pfds[1];
    stdin_pfd       = {.fd = fileno(stdin), .events = POLLIN};
    iap_pfd         = {.fd = platform.fd};
    snd_pcm_poll_descriptors(snd.get(), &pfds[2], pfds.size() - 2);
    ensure(fcntl(fileno(stdin), F_SETFL, O_NONBLOCK) == 0);
loop:
    iap_pfd.events = iap_ctx.send_busy ? POLLIN | POLLOUT : POLLIN;
    ensure(poll(pfds.data(), ctx->play_state == PlayState::Playing ? pfds.size() : 2uz, -1) >= 0);
    if(stdin_pfd.revents & POLLIN) {
        ensure(read_stdin());
    }
    if(iap_pfd.revents & POLLOUT) {
        iap_notify_send_complete(&iap_ctx);
    }
    if(iap_pfd.revents & POLLIN) {
        auto       buf = std::array<uint8_t, 256>();
        const auto ret = read(platform.fd, buf.data(), buf.size());
        ensure(ret > 0);
        std::println("====== acc: {} bytes ======", ret);
        dump_hex(std::span{buf.data(), size_t(ret)});
        iap_feed_hid_report(&iap_ctx, buf.data(), ret);
    }
    if(ctx->play_state != PlayState::Playing) {
        goto loop;
    }
    auto revents = (unsigned short)(0);
    ensure(snd_pcm_poll_descriptors_revents(snd.get(), &pfds[2], pcm_pfds_count, &revents) == 0);
    if(revents & POLLOUT) {
        const auto& pcm = ctx->tracks[ctx->current_track].data;
        const auto  ret = snd_pcm_writei(snd.get(), pcm.data() + ctx->pcm_cursor, pcm.size() - ctx->pcm_cursor);
        ensure(ret > 0);
    }
    goto loop;
    return 0;
}

#include <chrono>
#include <iostream>

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "artwork.hpp"
#include "context.hpp"
#include "flac.hpp"
#include "iap/iap.h"
#include "iap/spec/iap.h"
#include "macros/autoptr.hpp"
#include "macros/unwrap.hpp"
#include "util/fd.hpp"
#include "util/hexdump.hpp"
#include "util/split.hpp"

declare_autoptr(SndPCM, snd_pcm_t, snd_pcm_close);

namespace {
auto is_ipod_hid_hs() -> std::optional<bool> {
    const auto fd = FileDescriptor(open("/sys/module/g_ipod_hid/parameters/usb_hs", O_RDONLY));
    ensure(fd.as_handle() >= 0);
    unwrap(data, fd.read<char>());
    return data == 'Y';
}

auto time_now() -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::now();
}

auto diff_ms(std::chrono::system_clock::time_point a, std::chrono::system_clock::time_point b) -> size_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(a - b).count();
}

auto snd     = AutoSndPCM();
auto ctx     = Context();
auto iap_ctx = IAPContext();

auto handle_stdin(const std::string_view input) -> bool {
    if(input.empty()) {
        return true;
    }
    const auto elms = split(input, " ");
    if(elms[0] == "next") {
        ensure(ctx.skip_track(1));
    } else if(elms[0] == "prev") {
        ensure(ctx.skip_track(-1));
    } else if(elms[0] == "pause") {
        ensure(ctx.set_state(PlayState::Paused));
    } else if(elms[0] == "play") {
        ensure(ctx.set_state(PlayState::Playing));
    } else if(elms[0] == "stop") {
        ensure(ctx.set_state(PlayState::Stopped));
    } else {
        bail("invalid command {}", elms[0]);
    }
    return true;
}
} // namespace

auto Context::set_state(const PlayState new_state) -> bool {
    switch(play_state) {
    case PlayState::Stopped:
        switch(new_state) {
        case PlayState::Stopped:
            break;
        case PlayState::Playing:
        case PlayState::Paused:
            current_track = 0;
            pcm_cursor    = 0;
            ensure(snd_pcm_prepare(snd.get()) == 0);
            iap_notify_play_status(&iap_ctx, IAPIPodStatePlayStatus_Playing);
            break;
        }
    case PlayState::Playing:
        switch(new_state) {
        case PlayState::Stopped:
            ensure(snd_pcm_drop(snd.get()) == 0);
            iap_notify_play_status(&iap_ctx, IAPIPodStatePlayStatus_PlaybackStopped);
            break;
        case PlayState::Paused:
            ensure(snd_pcm_drop(snd.get()) == 0);
            iap_notify_play_status(&iap_ctx, IAPIPodStatePlayStatus_PlaybackPaused);
            break;
        case PlayState::Playing:
            break;
        }
    case PlayState::Paused:
        switch(new_state) {
        case PlayState::Stopped:
            break;
        case PlayState::Playing:
            ensure(snd_pcm_prepare(snd.get()) == 0);
            iap_notify_play_status(&iap_ctx, IAPIPodStatePlayStatus_Playing);
            break;
        case PlayState::Paused:
            ensure(snd_pcm_prepare(snd.get()) == 0);
            iap_notify_play_status(&iap_ctx, IAPIPodStatePlayStatus_PlaybackPaused);
            break;
        }
    }
    play_state = new_state;
    return true;
}

auto Context::skip_track(const int diff) -> bool {
    const auto new_track = int(current_track) + diff;
    ensure(new_track >= 0 && new_track < int(tracks.size()));
    current_track = new_track;
    pcm_cursor    = 0;
    iap_notify_track_playback_index(&iap_ctx, current_track);
    return true;
}

auto main(const int argc, const char* const* argv) -> int {
    ctx.fd = open("/dev/iap0", O_RDWR);
    ensure(ctx.fd >= 0);
    unwrap(hs, is_ipod_hid_hs());
    ensure(iap_init_ctx(&iap_ctx, {
                                      .usb_highspeed         = hs,
                                      .ignore_hid_report_id  = 1,
                                      .artwork_single_report = !hs,
                                      .enable_packet_dump    = 1,
                                  },
                        &ctx));

    ensure(snd_pcm_open(std::inout_ptr(snd), "hw:2,0", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0);
    ensure(snd_pcm_set_params(snd.get(), SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 0, 50000) == 0);
    const auto pcm_pfds_count = snd_pcm_poll_descriptors_count(snd.get());

    ensure(argc > 1);
    for(auto i = 1; i < argc; i += 1) {
        unwrap_mut(audio, decode_flac(argv[i], true));
        unwrap_mut(track, build_track(std::move(audio)));
        track.file = argv[i];
        PRINT("title={}", track.title);
        PRINT("album={}", track.album);
        PRINT("artist={}", track.artist);
        PRINT("date={}-{}-{}", track.year, track.month, track.day);
        PRINT("samples={}", track.total_samples);
        ctx.tracks.push_back(std::move(track));
    }

    auto  pfds      = std::vector<pollfd>(2 + pcm_pfds_count);
    auto& stdin_pfd = pfds[0];
    auto& iap_pfd   = pfds[1];
    stdin_pfd       = {.fd = fileno(stdin), .events = POLLIN};
    iap_pfd         = {.fd = ctx.fd};
    snd_pcm_poll_descriptors(snd.get(), &pfds[2], pfds.size() - 2);
    ensure(fcntl(fileno(stdin), F_SETFL, O_NONBLOCK) == 0);
    auto last_tick_called = time_now();
loop:
    const auto now     = time_now();
    const auto elapsed = diff_ms(now, last_tick_called);
    if(elapsed >= 100) {
        ensure(iap_periodic_tick(&iap_ctx));
        last_tick_called = now;
        goto loop;
    }

    iap_pfd.events = iap_ctx.send_busy ? POLLIN | POLLOUT : POLLIN;
    const auto ret = poll(pfds.data(), ctx.play_state == PlayState::Playing ? pfds.size() : 2uz, 100 - elapsed);
    ensure(ret >= 0);
    if(ret == 0) {
        goto loop; /* timed out */
    }
    if(stdin_pfd.revents & POLLIN) {
        auto line = std::string();
        std::getline(std::cin, line);
        handle_stdin(line);
    }
    if(iap_pfd.revents & POLLOUT) {
        iap_notify_send_complete(&iap_ctx);
    }
    if(iap_pfd.revents & POLLIN) {
        auto       buf = std::array<uint8_t, 256>();
        const auto ret = read(ctx.fd, buf.data(), buf.size());
        ensure(ret > 0);
        // std::println("====== acc: {} bytes ======", ret);
        // dump_hex(std::span{buf.data(), size_t(ret)});
        iap_feed_hid_report(&iap_ctx, buf.data(), ret);
    }
    if(ctx.play_state != PlayState::Playing) {
        goto loop;
    }
    auto revents = (unsigned short)(0);
    ensure(snd_pcm_poll_descriptors_revents(snd.get(), &pfds[2], pcm_pfds_count, &revents) == 0);
    if(revents & POLLOUT) {
        auto& track = ctx.tracks[ctx.current_track];
        if(track.data.empty()) {
            PRINT("decoding track {}", ctx.current_track);
            unwrap_mut(audio, decode_flac(track.file.data(), false));
            track.data = std::move(audio.data);
        }
        const auto ret = snd_pcm_writei(snd.get(), track.data.data() + ctx.pcm_cursor, (track.data.size() - ctx.pcm_cursor) / 2);
        if(ret > 0) {
            ctx.pcm_cursor += ret * 2;
            iap_notify_track_time_position(&iap_ctx, samples_to_ms(ctx.pcm_cursor));
            if(ctx.pcm_cursor >= track.data.size()) {
                ensure(ctx.skip_track(1));
            }
        } else if(ret == -EPIPE) {
            /* underrun */
            ensure(snd_pcm_prepare(snd.get()) == 0);
        } else {
            bail("alsa error {}({})", ret, strerror(-ret));
        }
    }
    goto loop;
    return 0;
}

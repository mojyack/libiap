#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "flac.hpp"
#include "iap/iap.h"
#include "macros/unwrap.hpp"
#include "platform.hpp"
#include "util/hexdump.hpp"

auto main(const int argc, const char* const* argv) -> int {
    ensure(argc > 1);
    unwrap(audio, decode_flac(argv[1]));
    PRINT("samples={}", audio.total_samples);
    PRINT("commnets={}", audio.comments);
    return 0;
    auto platform = LinuxPlatformData{
        .fd = open("/dev/iap0", O_RDWR | O_NONBLOCK),
    };
    ensure(platform.fd >= 0);
    auto pfds = std::array{pollfd{.fd = platform.fd}};
    auto ctx  = IAPContext{.platform = &platform};
    ensure(iap_init_ctx(&ctx));
loop:
    pfds[0].events = ctx.send_busy ? POLLIN | POLLOUT : POLLIN;
    ensure(poll(pfds.data(), pfds.size(), -1) >= 0);
    if(pfds[0].revents & POLLOUT) {
        iap_notify_send_complete(&ctx);
    }
    if(pfds[0].revents & POLLIN) {
        auto       buf = std::array<uint8_t, 256>();
        const auto ret = read(platform.fd, buf.data(), buf.size());
        ensure(ret > 0);
        std::println("====== acc: {} bytes ======", ret);
        dump_hex(std::span{buf.data(), size_t(ret)});
        iap_feed_hid_report(&ctx, buf.data(), ret);
    }
    goto loop;
    return 0;
}

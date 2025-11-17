#include <ImageMagick-7/Magick++.h>

#include "artwork.hpp"
#include "macros/assert.hpp"

auto decode_blob(std::span<const std::byte> blob, size_t width, size_t height) -> std::byte* {
    auto tmp = std::vector<std::byte>(width * height * 4);
    try {
        auto image = Magick::Image(Magick::Blob(blob.data(), blob.size()));
        image.resize(Magick::Geometry{width, height});
        image.write(0, 0, width, height, "RGBA", Magick::CharPixel, tmp.data());
    } catch(const Magick::Exception& e) {
        bail("imagemagick error: {}", e.what());
    }
    // rgba to rgb565le
    auto ret = new std::byte[width * height * 2];
    for(auto y = 0uz; y < height; y += 1) {
        for(auto x = 0uz; x < width; x += 1) {
            const auto src = (uint8_t*)(tmp.data() + (y * width + x) * 4);
            const auto dst = (uint16_t*)(ret + (y * width + x) * 2);

            const auto r = (31 * src[0] + (src[0] >> 3)) >> 8;
            const auto g = (63 * src[1] + (src[1] >> 2)) >> 8;
            const auto b = (31 * src[2] + (src[2] >> 3)) >> 8;

            *dst = (r << 11) | (g << 5) | b;
        }
    }

    return ret;
}

#include <ImageMagick-7/Magick++.h>

#include "macros/assert.hpp"
#include "types.hpp"

auto save_rgb565le(const BytesRef ref, const uint16_t width, const uint16_t height, const char* const file) -> bool {
    auto tmp = BytesArray(width * height * 4);
    // rgb565le to rgba
    for(auto y = 0uz; y < height; y += 1) {
        for(auto x = 0uz; x < width; x += 1) {
            const auto src = *(uint16_t*)(ref.data() + (y * width + x) * 2);
            const auto dst = (uint8_t*)(tmp.data() + (y * width + x) * 4);

            const auto r = (src & 0b11111000'00000000) >> 11;
            const auto g = (src & 0b00000111'11100000) >> 5;
            const auto b = (src & 0b00000000'00011111) >> 0;

            dst[0] = r * 255 / 31;
            dst[1] = g * 255 / 63;
            dst[2] = b * 255 / 31;
            dst[3] = 255;
        }
    }
    try {
        auto image = Magick::Image();
        image.read(width, height, "RGBA", Magick::CharPixel, tmp.data());
        image.magick("PNG");
        image.write(file);
    } catch(const Magick::Exception& e) {
        bail("imagemagick error: {}", e.what());
    }

    return true;
}

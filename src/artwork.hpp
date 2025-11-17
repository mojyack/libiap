#include <cstddef>
#include <span>

auto decode_blob(std::span<const std::byte> blob, size_t width, size_t height) -> std::byte*;

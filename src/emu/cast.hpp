#pragma once
#include "macros/assert.hpp"
#include "types.hpp"

template <class T>
auto bytes_as(BytesRef bytes) -> const T* {
    ensure(bytes.size() >= sizeof(T));
    return std::bit_cast<const T*>(bytes.data());
}

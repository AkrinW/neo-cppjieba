#pragma once

#include "neo/util/Logging.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace neo_cppjieba::detail {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "ByteOrder: mixed-endian targets are not supported");

// Decode an unsigned integer stored little-endian at the front of bytes; callers check the length first.
// Copying the bytes keeps unaligned reads well defined.
template <std::unsigned_integral T>
[[nodiscard]]
auto load_le(std::span<const std::byte> bytes) noexcept -> T {
    assert_check([&] { return bytes.size() >= sizeof(T); }, "ByteOrder: read past the end of a buffer");
    auto value = T{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

// Store an unsigned integer little-endian at the front of out; callers size out first.
template <std::unsigned_integral T>
auto store_le(std::span<std::byte> out, T value) noexcept -> void {
    assert_check([&] { return out.size() >= sizeof(T); }, "ByteOrder: write past the end of a buffer");
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(out.data(), &value, sizeof(T));
}

// Append an unsigned integer in little-endian byte order.
template <std::unsigned_integral T>
auto append_le(std::vector<std::byte> &out, T value) -> void {
    const auto offset = out.size();
    out.resize(offset + sizeof(T));
    store_le(std::span{out}.subspan(offset), value);
}

} // namespace neo_cppjieba::detail

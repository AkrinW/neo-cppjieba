#pragma once

#include "neo/util/ByteOrder.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace neo_cppjieba::detail {

// XXH64 primes from the reference specification.
inline constexpr auto kXxh64Prime1 = std::uint64_t{0x9E3779B185EBCA87};
inline constexpr auto kXxh64Prime2 = std::uint64_t{0xC2B2AE3D27D4EB4F};
inline constexpr auto kXxh64Prime3 = std::uint64_t{0x165667B19E3779F9};
inline constexpr auto kXxh64Prime4 = std::uint64_t{0x85EBCA77C2B2AE63};
inline constexpr auto kXxh64Prime5 = std::uint64_t{0x27D4EB2F165667C5};

// Mix one 8-byte input into an accumulator; stripes, lane merging and tail words share this round.
[[nodiscard]] constexpr
auto xxh64_round(std::uint64_t acc, std::uint64_t input) noexcept -> std::uint64_t {
    acc += input * kXxh64Prime2;
    acc = std::rotl(acc, 31);
    return acc * kXxh64Prime1;
}

// Fold one stripe lane into the converged hash.
[[nodiscard]] constexpr
auto xxh64_merge(std::uint64_t hash, std::uint64_t lane) noexcept -> std::uint64_t {
    hash ^= xxh64_round(0, lane);
    return hash * kXxh64Prime1 + kXxh64Prime4;
}

// Consume all complete 32-byte stripes with four independent lanes and converge them.
[[nodiscard]] inline
auto xxh64_stripes(std::span<const std::byte> &rest, std::uint64_t seed) noexcept -> std::uint64_t {
    auto v1 = seed + kXxh64Prime1 + kXxh64Prime2;
    auto v2 = seed + kXxh64Prime2;
    auto v3 = seed;
    auto v4 = seed - kXxh64Prime1;
    while (rest.size() >= 32) {
        v1 = xxh64_round(v1, load_le<std::uint64_t>(rest));
        v2 = xxh64_round(v2, load_le<std::uint64_t>(rest.subspan(8)));
        v3 = xxh64_round(v3, load_le<std::uint64_t>(rest.subspan(16)));
        v4 = xxh64_round(v4, load_le<std::uint64_t>(rest.subspan(24)));
        rest = rest.subspan(32);
    }
    auto hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    hash = xxh64_merge(hash, v1);
    hash = xxh64_merge(hash, v2);
    hash = xxh64_merge(hash, v3);
    return xxh64_merge(hash, v4);
}

// Mix the final bytes that do not fill a stripe, then avalanche.
[[nodiscard]] inline
auto xxh64_finish(std::uint64_t hash, std::span<const std::byte> rest) noexcept -> std::uint64_t {
    while (rest.size() >= 8) {
        hash ^= xxh64_round(0, load_le<std::uint64_t>(rest));
        hash = std::rotl(hash, 27) * kXxh64Prime1 + kXxh64Prime4;
        rest = rest.subspan(8);
    }
    if (rest.size() >= 4) {
        hash ^= static_cast<std::uint64_t>(load_le<std::uint32_t>(rest)) * kXxh64Prime1;
        hash = std::rotl(hash, 23) * kXxh64Prime2 + kXxh64Prime3;
        rest = rest.subspan(4);
    }
    for (const auto byte : rest) {
        hash ^= std::to_integer<std::uint64_t>(byte) * kXxh64Prime5;
        hash = std::rotl(hash, 11) * kXxh64Prime1;
    }
    hash ^= hash >> 33;
    hash *= kXxh64Prime2;
    hash ^= hash >> 29;
    hash *= kXxh64Prime3;
    return hash ^ (hash >> 32);
}

/// XXH64 of bytes, bit-identical to the reference implementation so external tools can verify checksums.
/// It detects corruption and torn reads; it is not a cryptographic hash.
[[nodiscard]] inline
auto xxh64(std::span<const std::byte> bytes, std::uint64_t seed) noexcept -> std::uint64_t {
    auto rest = bytes;
    auto hash = rest.size() >= 32 ? xxh64_stripes(rest, seed) : seed + kXxh64Prime5;
    hash += static_cast<std::uint64_t>(bytes.size());
    return xxh64_finish(hash, rest);
}

} // namespace neo_cppjieba::detail

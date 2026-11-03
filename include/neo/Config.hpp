#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace neo_cppjieba {

// Selects segmentation rules independently of the runtime CutMode.
enum class SegmentationStyle : std::uint8_t { CPP, RUST, PYTHON };

namespace compile_config {
// Build-wide algorithm policy; rebuild every translation unit with the same value.
inline constexpr auto segmentation_style = SegmentationStyle::CPP;

static_assert(segmentation_style == SegmentationStyle::CPP || segmentation_style == SegmentationStyle::RUST
              || segmentation_style == SegmentationStyle::PYTHON);

#ifdef NDEBUG
inline constexpr auto is_debug_build = false;
#else
inline constexpr auto is_debug_build = true;
#endif
} // namespace compile_config

// Capacity types are compile-time settings; rebuild all translation units with the same definitions.
// Callers guarantee that their workload fits these capacities. Debug builds assert this contract;
// release builds do not check capacity overflow. Unicode and external-data validation remain enabled.

#if defined(__SIZEOF_INT128__)
// Native 128-bit capacity storage on supporting targets; C++23 has no std::uint128_t.
// __extension__ marks the GNU type as intended, so -Wpedantic builds stay quiet.
__extension__ using uint128_t = unsigned __int128;
#endif

namespace detail {

// Before 1.1.0, CPP used float weights; Rust and Python used double precision.
// All styles now use double precision to preserve legacy CPP path scoring.
using WordWeight = double;

// Explicit type selection also accepts native 128-bit integers in strict C++23 mode.
template <typename T>
concept CapacityInteger = std::same_as<T, std::uint8_t> || std::same_as<T, std::uint16_t>
                          || std::same_as<T, std::uint32_t> || std::same_as<T, std::uint64_t>
#if defined(__SIZEOF_INT128__)
                          || std::same_as<T, uint128_t>
#endif
    ;

// Check inclusive capacity limits without comparing a host size to an out-of-range constant.
template <CapacityInteger T>
constexpr auto size_fits_capacity(std::size_t size) noexcept -> bool {
    if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<std::size_t>::digits) {
        return size <= std::numeric_limits<T>::max();
    } else {
        return true;
    }
}

// Keep the maximum id reserved, including when capacity storage is wider than the host size.
template <CapacityInteger T>
constexpr auto size_below_capacity(std::size_t size) noexcept -> bool {
    if constexpr (std::numeric_limits<T>::digits <= std::numeric_limits<std::size_t>::digits) {
        return size < std::numeric_limits<T>::max();
    } else {
        return true;
    }
}

// Convert a rune position, or arithmetic on rune positions, to a container index. Positions never exceed the size of
// the container they index, so the value is kept even when RuneIndex is wider than size_t, as with 128 bits.
template <typename Index>
    requires std::integral<Index> || CapacityInteger<Index>
[[nodiscard]]
constexpr auto to_index(Index position) noexcept -> std::size_t {
    return static_cast<std::size_t>(position);
}

} // namespace detail

// Size this for the total decoded code points in one segmentation input, including separators.
// Encoded input also depends on SourceOffset; cut_runes does not use source offsets.
using RuneIndex = std::uint32_t;

// Size this for the original string length: UTF-8 bytes, UTF-16 code units, or UTF-32 code units.
// The default permits up to 4 GiB - 1 byte of UTF-8 input, subject to rune, DAG and memory limits.
using SourceOffset = std::uint32_t;

// Size this for the longest separator-free segment and its dictionary match density.
// An n-code-point segment needs n edges plus longer-word matches, at most n * (n + 1) / 2.
// With every substring matched, 8/16/32 bits cover segments of 22/361/92,681 code points.
using DagOffset = std::uint32_t;
// Only Trie::find_dag materializes these offsets; segmentation matches the trie directly and never builds a DAG.

// Size this for the combined main/user dictionary: word count, word length and shared prefixes.
// For words of at most four code points, 8/16 bits cover at least 84/21,844 entries by node capacity.
// The bundled main dictionary (about 4.84 MiB, 349k entries) needs at least 32 bits; see README tables.
using TrieNodeId = std::uint32_t;

// All four capacities support 8, 16, 32, 64 and native 128 bits; container indices still use size_t.
// Container lengths, allocation sizes and iterator differences retain size_t/ptrdiff_t.
// Choosing 64/128 bits does not lift host address-space, container or memory limits.

// README translates these settings into input lengths and dictionary workloads, with sizing assumptions.
static_assert(detail::CapacityInteger<RuneIndex>);
static_assert(detail::CapacityInteger<SourceOffset>);
static_assert(detail::CapacityInteger<DagOffset>);
static_assert(detail::CapacityInteger<TrieNodeId>);

// LogLevel represents the severity level of a log message, ranging from debug to fatal.
enum class LogLevel : uint8_t { LL_DEBUG, LL_INFO, LL_WARNING, LL_ERROR, LL_FATAL };

// Unicode input validation is always enabled because it also protects memory access bounds.

// Log failures use an application exception that accepts the formatted message.
template <typename Exception>
concept LogException = std::derived_from<Exception, std::exception> && std::constructible_from<Exception, std::string>;

// A configuration supplies an exception type and a boolean usable in constant expressions.
// Its log threshold and positive buffer capacity must also be compile-time constants.
template <typename Config>
concept LogConfiguration = requires {
    requires LogException<typename Config::Exception>;
    { Config::show_source_location } -> std::same_as<const bool &>;
    typename std::bool_constant<Config::show_source_location>;
    { Config::min_level } -> std::same_as<const LogLevel &>;
    typename std::integral_constant<LogLevel, Config::min_level>;
    requires(Config::min_level >= LogLevel::LL_DEBUG && Config::min_level <= LogLevel::LL_FATAL);
    { Config::buffer_size } -> std::same_as<const std::size_t &>;
    typename std::integral_constant<std::size_t, Config::buffer_size>;
    requires(Config::buffer_size > 0 && Config::buffer_size <= std::numeric_limits<std::ptrdiff_t>::max());
};

// Default diagnostics expose source locations and throw a standard runtime error.
// Edit these defaults here to configure the library globally.
// Individual logging and check calls may still pass an explicit configuration type.
struct LogConfig {
    static constexpr auto show_source_location = true;
    static constexpr auto min_level = compile_config::is_debug_build ? LogLevel::LL_DEBUG : LogLevel::LL_INFO;
    static constexpr auto buffer_size = std::size_t{512};
    using Exception = std::runtime_error;
};

} // namespace neo_cppjieba

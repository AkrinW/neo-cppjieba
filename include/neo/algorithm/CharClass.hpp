#pragma once

#include "neo/Config.hpp"
#include "neo/value/Rune.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <ranges>
#include <span>

namespace neo_cppjieba::detail {

// Locale-independent ASCII classes shared by the reference segmentation rules.
[[nodiscard]] constexpr auto is_ascii_letter(Rune rune) noexcept -> bool {
    return (rune >= U'a' && rune <= U'z') || (rune >= U'A' && rune <= U'Z');
}

// Only ASCII digits participate in MP's alphanumeric joining.
[[nodiscard]] constexpr auto is_ascii_digit(Rune rune) noexcept -> bool {
    return rune >= U'0' && rune <= U'9';
}

// Both reference implementations merge consecutive ASCII alphanumeric singletons.
[[nodiscard]] constexpr auto is_ascii_alphanumeric(Rune rune) noexcept -> bool {
    return is_ascii_letter(rune) || is_ascii_digit(rune);
}

// Python finalseg and jieba-rs use this narrower range for HMM input.
[[nodiscard]] constexpr auto is_hmm_han(Rune rune) noexcept -> bool {
    return rune >= 0x4E00 && rune <= 0x9FD5;
}

// jieba-rs 0.11.0 includes these CJK extensions in dictionary blocks.
[[nodiscard]] constexpr auto is_rust_cjk(Rune rune) noexcept -> bool {
    return (rune >= 0x4E00 && rune <= 0x9FFF) || (rune >= 0x3400 && rune <= 0x4DBF)
           || (rune >= 0xF900 && rune <= 0xFAFF) || (rune >= 0x20000 && rune <= 0x2A6DF)
           || (rune >= 0x2A700 && rune <= 0x2B73F) || (rune >= 0x2B740 && rune <= 0x2B81F)
           || (rune >= 0x2B820 && rune <= 0x2CEAF) || (rune >= 0x2CEB0 && rune <= 0x2EBEF)
           || (rune >= 0x2F800 && rune <= 0x2FA1F);
}

// The precise/search dictionary block also accepts the reference ASCII punctuation set.
[[nodiscard]] constexpr auto is_style_dictionary_rune(Rune rune) noexcept -> bool {
    const auto han = [rune] {
        if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
            return is_rust_cjk(rune);
        } else {
            return is_hmm_han(rune);
        }
    }();
    return han || is_ascii_alphanumeric(rune) || rune == U'+' || rune == U'#' || rune == U'&' || rune == U'.'
           || rune == U'_' || rune == U'%' || rune == U'-';
}

// Python's Unicode whitespace class includes the ASCII information separators.
[[nodiscard]] constexpr auto is_python_space(Rune rune) noexcept -> bool {
    return (rune >= 0x09 && rune <= 0x0D) || (rune >= 0x1C && rune <= 0x20) || rune == 0x85 || rune == 0xA0
           || rune == 0x1680 || (rune >= 0x2000 && rune <= 0x200A) || rune == 0x2028 || rune == 0x2029 || rune == 0x202F
           || rune == 0x205F || rune == 0x3000;
}

// Decimal digit blocks from Unicode 16.0, as used by the benchmark's Python 3.14.
[[nodiscard]] constexpr auto is_python_decimal(Rune rune) noexcept -> bool {
    if (is_ascii_digit(rune)) {
        return true;
    }
    static constexpr auto zeros = std::to_array<Rune>({
        0x30,    0x660,   0x6F0,   0x7C0,   0x966,   0x9E6,   0xA66,   0xAE6,   0xB66,   0xBE6,   0xC66,
        0xCE6,   0xD66,   0xDE6,   0xE50,   0xED0,   0xF20,   0x1040,  0x1090,  0x17E0,  0x1810,  0x1946,
        0x19D0,  0x1A80,  0x1A90,  0x1B50,  0x1BB0,  0x1C40,  0x1C50,  0xA620,  0xA8D0,  0xA900,  0xA9D0,
        0xA9F0,  0xAA50,  0xABF0,  0xFF10,  0x104A0, 0x10D30, 0x10D40, 0x11066, 0x110F0, 0x11136, 0x111D0,
        0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x116D0, 0x116DA, 0x11730, 0x118E0, 0x11950, 0x11BF0,
        0x11C50, 0x11D50, 0x11DA0, 0x11F50, 0x16130, 0x16A60, 0x16AC0, 0x16B50, 0x16D70, 0x1CCF0, 0x1D7CE,
        0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0, 0x1E5F1, 0x1E950, 0x1FBF0,
    });
    // Zeros not above the rune; the last one starts its decimal block.
    const auto lower = std::ranges::subrange{zeros.begin(), std::ranges::upper_bound(zeros, rune)};
    return !lower.empty() && rune - lower.back() < 10;
}

// Rust's HMM and search paths recognize connectors inside alphanumeric compounds.
[[nodiscard]] constexpr auto is_rust_connector(Rune rune) noexcept -> bool {
    return rune == U'.' || rune == U'_' || rune == U'-';
}

// Match the non-Chinese HMM token starting at an ASCII alphanumeric rune.
[[nodiscard]] inline auto style_hmm_ascii_end(std::span<const Rune> runes, RuneIndex begin) noexcept -> RuneIndex {
    assert(begin < runes.size() && is_ascii_alphanumeric(runes[begin]));
    auto end = begin;
    while (end < runes.size() && is_ascii_alphanumeric(runes[end])) {
        ++end;
    }
    if constexpr (compile_config::segmentation_style == SegmentationStyle::RUST) {
        while (end < runes.size() && end + 1 < runes.size() && is_rust_connector(runes[end])
               && is_ascii_alphanumeric(runes[end + 1])) {
            ++end;
            while (end < runes.size() && is_ascii_alphanumeric(runes[end])) {
                ++end;
            }
        }
    } else {
        if (end < runes.size() && static_cast<size_t>(end + 1) < runes.size() && runes[end] == U'.'
            && is_python_decimal(runes[end + 1])) {
            ++end;
            while (end < runes.size() && is_python_decimal(runes[end])) {
                ++end;
            }
        }
    }
    if (end < runes.size() && runes[end] == U'%') {
        ++end;
    }
    return end;
}

} // namespace neo_cppjieba::detail

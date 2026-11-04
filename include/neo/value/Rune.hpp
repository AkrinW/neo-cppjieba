#pragma once

#include "neo/Config.hpp"
#include "neo/Fwd.hpp"
#include "neo/util/Logging.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace neo_cppjieba {

// Rune is a Unicode code point, every single Unicode character is represented by a Rune. using char32_t to fixed width
// of Rune to 4 bytes, which can represent all Unicode code points.
using Rune = char32_t;

namespace detail {
// Unicode is a sequence of Runes. an input string will be decoding to Unicode.
using Unicode = std::vector<Rune>;
} // namespace detail

// UnicodeWithOffset stores decoded Unicode along with source string offset information.
// When the target encoding matches the source encoding, re-encoding can be done by
// simply copying bytes from the original source, avoiding the per-codepoint encoding cost.
struct UnicodeWithOffset {
    std::vector<Rune> runes;
    // offsets[i] is the code-unit offset of runes[i] in source.
    // offsets[runes.size()] == source.size()  (sentinel for easy range computation).
    // Total size: runes.size() + 1.
    std::vector<SourceOffset> offsets;

    [[nodiscard]]
    auto get_runes() const & noexcept -> const std::vector<Rune> & {
        return runes;
    }
    [[nodiscard]]
    auto get_runes() const && -> const std::vector<Rune> & = delete;

    [[nodiscard]]
    auto get_offsets() const & noexcept -> const std::vector<SourceOffset> & {
        return offsets;
    }
    [[nodiscard]]
    auto get_offsets() const && -> const std::vector<SourceOffset> & = delete;
};

/// A half-open range [begin, end) of rune positions within the input.
/// Value types carry no encoding: encode the selected runes with Utf::encode<CharT>(range.slice(runes)).
struct WordRange {
    RuneIndex begin;
    RuneIndex end;

    [[nodiscard]]
    constexpr auto size() const noexcept -> RuneIndex {
        assert_check([this] { return begin <= end; }, "Reversed internal WordRange [{}, {})", begin, end);
        return end - begin;
    }

    /// Extract the corresponding sub-span from the original runes.
    [[nodiscard]]
    auto slice(std::span<const Rune> runes) const noexcept -> std::span<const Rune> {
        assert_check([&] { return end <= runes.size(); }, "Internal WordRange end {} exceeds {} runes", end,
                     runes.size());
        return runes.subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(size()));
    }

    constexpr auto operator==(const WordRange &) const noexcept -> bool = default;
};

} // namespace neo_cppjieba

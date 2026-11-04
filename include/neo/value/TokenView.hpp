#pragma once

#include "neo/Config.hpp"
#include "neo/Fwd.hpp"
#include "neo/concept/Text.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"

#include <cstddef>
#include <string_view>

namespace neo_cppjieba {

// A half-open range of original code units: bytes for UTF-8, native units for other encodings.
struct SourceRange {
    SourceOffset begin;
    SourceOffset end;

    [[nodiscard]]
    constexpr auto size() const noexcept -> SourceOffset {
        assert_check([&] { return begin <= end; }, "Reversed internal SourceRange [{}, {})", begin, end);
        return end - begin;
    }

    template <CharType CharT>
    [[nodiscard]]
    constexpr auto slice(std::basic_string_view<CharT> source) const noexcept -> std::basic_string_view<CharT> {
        assert_check([&] { return begin <= end && end <= source.size(); }, "SourceRange exceeds its input");
        return source.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
    }

    constexpr auto operator==(const SourceRange &) const noexcept -> bool = default;
};

// Compact token metadata keeps rune and source-code-unit coordinates distinct.
struct TokenPosition {
    WordRange runes;
    SourceRange source;

    constexpr auto operator==(const TokenPosition &) const noexcept -> bool = default;
};

// The word borrows the original text; position metadata is copied into the view.
template <CharType CharT>
struct TokenView {
    std::basic_string_view<CharT> word;
    TokenPosition position;
};

} // namespace neo_cppjieba

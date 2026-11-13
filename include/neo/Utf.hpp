#pragma once

#include "neo/Fwd.hpp"          // IWYU pragma: export
#include "neo/concept/Text.hpp" // IWYU pragma: export
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp" // IWYU pragma: export

#include <span>
#include <string>
#include <vector>

namespace neo_cppjieba {

// The public Unicode entry point: validation and conversion between encoded text and runes for cut_runes.
// Input encodings follow the code-unit type: UTF-8 bytes, UTF-16 or UTF-32; malformed input throws.
class Utf final {
public:
    Utf() = delete;

    // Check well-formedness without allocating; bytes valid in several encodings are not told apart.
    template <StringLike T>
    [[nodiscard]] static constexpr
    auto is_valid(const T &input) -> bool {
        return detail::is_valid_utf(input);
    }

    template <StringLike T>
    [[nodiscard]] static
    auto decode(const T &input) -> std::vector<Rune> {
        return detail::decode(input);
    }

    template <CharType CharT>
    [[nodiscard]] static
    auto encode(std::span<const Rune> runes) -> std::basic_string<CharT> {
        return detail::encode<CharT>(runes);
    }
};

} // namespace neo_cppjieba

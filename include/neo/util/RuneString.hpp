#pragma once

#include "neo/third_party/gtl.hpp"
#include "neo/value/Rune.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace neo_cppjieba::detail {

// u32string keys accept u32string_view lookups, so candidate words are never copied for a query.
struct RuneStringHash {
    using is_transparent = void;

    [[nodiscard]]
    auto operator()(std::u32string_view word) const noexcept -> size_t {
        return std::hash<std::u32string_view>{}(word);
    }
};

using RuneStringSet = gtl::flat_hash_set<std::u32string, RuneStringHash, std::equal_to<>>;

// View code points as the u32string_view key of rune-string sets and maps; no characters are copied.
[[nodiscard]]
inline auto rune_view(std::span<const Rune> runes) noexcept -> std::u32string_view {
    return {runes.data(), runes.size()};
}

} // namespace neo_cppjieba::detail

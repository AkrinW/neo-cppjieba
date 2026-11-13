#pragma once

#include "neo/Config.hpp"
#include "neo/value/PosTag.hpp"

#include <limits>

namespace neo_cppjieba::detail {

// Missing entries use a value outside finite dictionary weights, so log(1) == 0 remains valid.
inline constexpr auto kMissingWordWeight = -std::numeric_limits<detail::WordWeight>::infinity();

/// Compact dictionary entry stored by value in trie transitions, with no pointer indirection.
/// The original CppJieba DictUnit also stored the word and a std::string tag (~80 bytes).
struct DictUnit {
    detail::WordWeight weight{kMissingWordWeight};
    PosTag tag{};

    /// Default construction means "no value"; every finite weight, including zero, is a real entry.
    [[nodiscard]] constexpr
    auto has_value() const noexcept -> bool {
        return weight != kMissingWordWeight;
    }
};

static_assert(sizeof(DictUnit) == 2 * sizeof(detail::WordWeight));
static_assert(alignof(DictUnit) == alignof(detail::WordWeight));

} // namespace neo_cppjieba::detail

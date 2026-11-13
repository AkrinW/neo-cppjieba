#pragma once

#include "neo/Fwd.hpp"
#include "neo/concept/Text.hpp"
#include "neo/options/Tagging.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/TokenView.hpp"

#include <string>
#include <string_view>
#include <type_traits>

namespace neo_cppjieba {

// Token metadata with the word's part-of-speech tag; contains no pointers into the input.
struct TaggedPosition {
    TokenPosition position{};
    PosTag tag;

    constexpr
    auto operator==(const TaggedPosition &) const noexcept -> bool = default;
};

// An independent word and its tag, matching CppJieba's Tag pairs and Python jieba's posseg pairs.
template <CharType CharT>
struct TaggedWord {
    std::basic_string<CharT> word;
    PosTag tag;

    auto operator==(const TaggedWord &) const -> bool = default;
};

// The word borrows the original text; position metadata and the tag are copied into the view.
template <CharType CharT>
struct TaggedTokenView {
    std::basic_string_view<CharT> word;
    TokenPosition position{};
    PosTag tag;
};

namespace detail {

// Result element types selected by Tagging; Tagging::None keeps the untagged types unchanged.
template <Tagging tagging>
using token_record_t = std::conditional_t<tagging == Tagging::Pos, TaggedPosition, TokenPosition>;

template <Tagging tagging, CharType CharT>
using token_view_t = std::conditional_t<tagging == Tagging::Pos, TaggedTokenView<CharT>, TokenView<CharT>>;

template <Tagging tagging, CharType CharT>
using word_t = std::conditional_t<tagging == Tagging::Pos, TaggedWord<CharT>, std::basic_string<CharT>>;

} // namespace detail

} // namespace neo_cppjieba

#pragma once

#include "neo/Config.hpp"
#include "neo/algorithm/Output.hpp"
#include "neo/concept/Output.hpp"
#include "neo/concept/Text.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/TaggedToken.hpp"

#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace neo_cppjieba::detail {

// CppJieba's rule for words without a dictionary tag. ASCII runes are counted only until they
// reach half the word length, so a one-rune word is always "x" and an ASCII prefix decides the rest:
// no ASCII is "x", ASCII digits only is "m", and any other ASCII is "eng".
[[nodiscard]] constexpr
auto untagged_word_tag(std::span<const Rune> word) noexcept -> PosTag {
    auto ascii = size_t{0};
    auto digits = size_t{0};
    for (auto i = size_t{0}; i < word.size() && ascii < word.size() / 2; ++i) {
        if (word[i] < 0x80) {
            ++ascii;
            if (U'0' <= word[i] && word[i] <= U'9') {
                ++digits;
            }
        }
    }
    if (ascii == 0) {
        return pos::x;
    }
    return digits == ascii ? pos::m : pos::eng;
}

// User dictionary words without a tag fall back to the untagged rule, as in CppJieba.
[[nodiscard]] inline
auto word_tag(const DictTrie &dict, std::span<const Rune> word) noexcept -> PosTag {
    const auto unit = dict.find(word);
    return unit.has_value() && !unit.tag.empty() ? unit.tag : untagged_word_tag(word);
}

// Tags are looked up after segmentation, so untagged entry points never carry tag state.
template <CharType CharT, TaggedOutputValue<CharT> Output>
inline
auto fill_tagged_output(const DictTrie &dict, std::basic_string_view<CharT> source, std::span<const Rune> runes,
                        std::span<const WordRange> ranges, std::span<const SourceOffset> offsets,
                        std::vector<Output> &out) -> void {
    out.reserve(ranges.size());
    for (const auto &range : ranges) {
        const auto source_range = to_source_range(range, offsets);
        const auto tag = word_tag(dict, range.slice(runes));
        if constexpr (std::same_as<Output, TaggedPosition>) {
            out.push_back({{range, source_range}, tag});
        } else {
            out.push_back({std::basic_string<CharT>{source_range.slice(source)}, tag});
        }
    }
}

// Deliver borrowed tagged views in segmentation order; visitor exceptions propagate to the caller.
template <CharType CharT, TaggedTokenVisitor<CharT> Emit>
inline
auto emit_tagged_tokens(const DictTrie &dict, std::basic_string_view<CharT> source, std::span<const Rune> runes,
                        std::span<const WordRange> ranges, std::span<const SourceOffset> offsets,
                        Emit &emit) noexcept(std::is_nothrow_invocable_v<Emit &, TaggedTokenView<CharT>>) -> void {
    for (const auto &range : ranges) {
        const auto source_range = to_source_range(range, offsets);
        std::invoke(emit, TaggedTokenView<CharT>{
                              source_range.slice(source), {range, source_range}, word_tag(dict, range.slice(runes))});
    }
}

} // namespace neo_cppjieba::detail

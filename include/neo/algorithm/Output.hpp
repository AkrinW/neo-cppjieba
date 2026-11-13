#pragma once

#include "neo/Config.hpp"
#include "neo/concept/Output.hpp"
#include "neo/concept/Text.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/Rune.hpp"
#include "neo/value/TokenView.hpp"

#include <concepts>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace neo_cppjieba::detail {

// The offset sentinel maps the exclusive rune end to the corresponding source boundary.
[[nodiscard]] inline
auto to_source_range(WordRange range, std::span<const SourceOffset> offsets) noexcept -> SourceRange {
    assert_check([&] { return range.begin <= range.end && range.end < offsets.size(); },
                 "Jieba rune range exceeds its source offset table");
    return {offsets[to_index(range.begin)], offsets[to_index(range.end)]};
}

// Fill the caller's cleared output from validated ranges, retaining the output capacity.
template <CharType CharT, OutputValue<CharT> Output>
inline
auto fill_output(std::basic_string_view<CharT> source, std::span<const WordRange> ranges,
                 std::span<const SourceOffset> offsets, std::vector<Output> &out) -> void {
    out.reserve(ranges.size());
    for (const auto &range : ranges) {
        const auto source_range = to_source_range(range, offsets);
        if constexpr (std::same_as<Output, SourceRange>) {
            out.push_back(source_range);
        } else if constexpr (std::same_as<Output, TokenPosition>) {
            out.push_back({range, source_range});
        } else {
            out.emplace_back(source_range.slice(source));
        }
    }
}

// Deliver borrowed views in segmentation order; visitor exceptions propagate to the caller.
template <CharType CharT, TokenVisitor<CharT> Emit>
inline
auto emit_tokens(std::basic_string_view<CharT> source, std::span<const WordRange> ranges,
                 std::span<const SourceOffset> offsets,
                 Emit &emit) noexcept(std::is_nothrow_invocable_v<Emit &, TokenView<CharT>>) -> void {
    for (const auto &range : ranges) {
        const auto source_range = to_source_range(range, offsets);
        std::invoke(emit, TokenView<CharT>{source_range.slice(source), {range, source_range}});
    }
}

} // namespace neo_cppjieba::detail

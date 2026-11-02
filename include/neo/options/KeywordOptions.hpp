#pragma once

#include "neo/Fwd.hpp"
#include "neo/util/Logging.hpp"
#include "neo/value/PosTag.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace neo_cppjieba {

// Python jieba's default allowPOS for textrank: place names, nouns, verbal nouns and verbs.
inline constexpr auto python_textrank_tags = std::array{pos::ns, pos::n, pos::vn, pos::v};

// An empty allowed_tags keeps every word, as CppJieba does. Otherwise only words whose tag is listed
// become candidates, like Python jieba's allowPOS; tags come from Jieba::lookup_tag.
struct TfIdfOptions {
    std::vector<PosTag> allowed_tags{};
};

// CppJieba's TextRank defaults. Each candidate word links to the next window - 1 candidate words,
// so words removed by allowed_tags, like stop words, do not take up window slots.
struct TextRankOptions {
    size_t window{5};
    size_t iterations{10};
    double damping{0.85};
    std::vector<PosTag> allowed_tags{};
};

namespace detail {

// Option values supplied by callers are configuration errors, not internal invariants.
inline auto check_text_rank_options(const TextRankOptions &options) -> void {
    check(options.damping >= 0.0 && options.damping <= 1.0, "TextRankExtractor: damping {} is outside [0, 1]",
          options.damping);
}

} // namespace detail

} // namespace neo_cppjieba

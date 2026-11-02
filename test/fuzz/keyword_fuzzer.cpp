#include "neo/Config.hpp"
#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"
#include "neo/Utf.hpp"
#include "neo/options/KeywordOptions.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/Keyword.hpp"

#include "FuzzUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace neo_cppjieba;

namespace {

// The small sources keep segmentation cheap; extraction under test reads the fuzzed resources.
auto small_jieba() -> const Jieba & {
    static const auto jieba =
        Jieba{DictSource::memory(small_dict), DictSource::memory(small_model), std::span<const DictSource>{}};
    return jieba;
}

// Both extractors over one set of fuzzed resources; null when a resource is rejected.
struct Extractors {
    std::unique_ptr<TfIdfExtractor> tfidf;
    std::unique_ptr<TextRankExtractor> textrank;
};

// Build both extractors from the fuzzed IDF table and stop words.
auto load_extractors(std::string_view idf, std::string_view stop_words) -> Extractors {
    try {
        return {std::make_unique<TfIdfExtractor>(DictSource::memory(idf), DictSource::memory(stop_words)),
                std::make_unique<TextRankExtractor>(DictSource::memory(stop_words))};
    } catch (const LogConfig::Exception &) {
        return {};
    }
}

// At most top_n distinct non-stop words, highest weight first; every position spells its word in input order.
auto check_keywords(const std::vector<Keyword<char>> &keywords, std::string_view input, std::size_t top_n,
                    std::string_view stop_words) -> void {
    require(keywords.size() <= top_n, "more keywords than top_n");
    auto stops = std::vector<std::string_view>{};
    for (const auto line : detail::get_line_view(stop_words)) {
        stops.push_back(line);
    }
    auto words = std::vector<std::string_view>{};
    for (const auto &keyword : keywords) {
        require(!std::isnan(keyword.weight), "a keyword weight is NaN");
        require(std::ranges::find(stops, keyword.word) == stops.end(), "a stop word became a keyword");
        require(keyword.tag == small_jieba().lookup_tag(keyword.word), "a keyword tag differs from lookup_tag");
        require(!keyword.positions.empty(), "a keyword has no position");
        for (std::size_t index = 0; index < keyword.positions.size(); ++index) {
            const auto &source = keyword.positions[index].source;
            require(source.end <= input.size() && source.slice(input) == keyword.word,
                    "a position spells another word");
            require(index == 0 || keyword.positions[index - 1].source.begin < source.begin,
                    "positions are out of order");
        }
        words.push_back(keyword.word);
    }
    require(std::ranges::is_sorted(keywords, std::ranges::greater{}, &Keyword<char>::weight),
            "keywords are out of weight order");
    std::ranges::sort(words);
    require(std::ranges::adjacent_find(words) == words.end(), "a word appears as two keywords");
}

// Extraction from invalid UTF-8 must throw the checked exception.
template <typename Extract>
auto check_rejected(const Extract &extract) -> void {
    try {
        static_cast<void>(extract());
    } catch (const LogConfig::Exception &) {
        return;
    }
    require(false, "invalid UTF-8 input was not rejected");
}

} // namespace

// The first byte is top_n; the rest holds the IDF table, the stop words and the input, split at 0xFF bytes.
// Malformed resources and input must be rejected through the checked exception; internal assertions stay fatal.
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    const auto top_n = static_cast<std::size_t>(reader.byte() % 32);
    const auto parts = split_parts(reader.rest(), 3);
    if (parts.size() < 3) {
        return 0;
    }
    const auto idf = parts[0];
    const auto stop_words = parts[1];
    const auto input = parts[2];
    const auto extractors = load_extractors(idf, stop_words);
    if (extractors.tfidf == nullptr) {
        return 0;
    }
    const auto tfidf = [&] {
        return extractors.tfidf->extract(small_jieba(), input, top_n, TfIdfOptions{});
    };
    const auto textrank = [&] {
        return extractors.textrank->extract(small_jieba(), input, top_n, TextRankOptions{});
    };
    if (!Utf::is_valid(input)) {
        check_rejected(tfidf);
        check_rejected(textrank);
        return 0;
    }
    check_keywords(tfidf(), input, top_n, stop_words);
    check_keywords(textrank(), input, top_n, stop_words);
    return 0;
}

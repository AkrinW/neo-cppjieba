#pragma once

#include "neo/third_party/gtl.hpp"
#include "neo/util/Logging.hpp"
#include "neo/util/RuneString.hpp"
#include "neo/util/StringUtil.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/DictSource.hpp"

#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace neo_cppjieba::detail {

// Words excluded from keyword candidates. One word per line, compared exactly as written; empty lines are ignored.
class StopWords {
public:
    explicit StopWords(const DictSource &source) {
        with_source_text(source, [this](std::string_view text, std::string_view) { load(text); });
    }

    StopWords(StopWords &&) noexcept = default;
    auto operator=(StopWords &&) noexcept -> StopWords & = default;
    auto operator=(const StopWords &) -> StopWords & = delete;

    [[nodiscard]]
    auto clone() const -> StopWords {
        return StopWords{*this};
    }

    [[nodiscard]]
    auto contains(std::u32string_view word) const -> bool {
        return words_.contains(word);
    }

private:
    StopWords(const StopWords &) = default;

    auto load(std::string_view text) -> void {
        for (auto line : get_line_view(text)) {
            if (!line.empty()) {
                const auto runes = decode(line);
                words_.emplace(runes.begin(), runes.end());
            }
        }
    }

    RuneStringSet words_;
};

// Inverse document frequencies. Each nonempty line is "word idf"; the table is never empty.
// Words absent from the table score with the average IDF, as in CppJieba.
class IdfTable {
public:
    explicit IdfTable(const DictSource &source) {
        with_source_text(source, [this](std::string_view text, std::string_view name) { load(text, name); });
    }

    IdfTable(IdfTable &&) noexcept = default;
    auto operator=(IdfTable &&) noexcept -> IdfTable & = default;
    auto operator=(const IdfTable &) -> IdfTable & = delete;

    [[nodiscard]]
    auto clone() const -> IdfTable {
        return IdfTable{*this};
    }

    // The word's IDF, or the average for words missing from the table.
    [[nodiscard]]
    auto idf(std::u32string_view word) const -> double {
        const auto it = table_.find(word);
        return it != table_.end() ? it->second : average_;
    }

    [[nodiscard]]
    auto average() const noexcept -> double {
        return average_;
    }

private:
    IdfTable(const IdfTable &) = default;

    auto load(std::string_view text, std::string_view path) -> void {
        auto sum = 0.0;
        auto line_number = size_t{0};
        for (auto line : get_line_view(text)) {
            ++line_number;
            if (line.empty()) {
                continue;
            }
            auto fields = get_split_view(line, ' ');
            auto it = fields.begin();
            const auto word = *it;
            ++it;
            check(it != fields.end(), "KeywordExtractor: missing IDF at {}:{}", path, line_number);
            const auto idf = decode_value<double>(*it);
            ++it;
            check(it == fields.end(), "KeywordExtractor: expected exactly 2 fields at {}:{}", path, line_number);
            check(std::isfinite(idf), "KeywordExtractor: IDF must be finite at {}:{}", path, line_number);
            const auto runes = decode(word);
            table_.insert_or_assign(std::u32string{runes.begin(), runes.end()}, idf);
            sum += idf;
        }
        check(!table_.empty(), "KeywordExtractor: IDF table is empty: {}", path);
        average_ = sum / static_cast<double>(table_.size());
    }

    gtl::flat_hash_map<std::u32string, double, RuneStringHash, std::equal_to<>> table_;
    double average_{0.0};
};

} // namespace neo_cppjieba::detail

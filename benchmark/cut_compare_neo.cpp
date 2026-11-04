// Neo engine of cut_compare_benchmark, alone in its translation unit; see CutCompareEngines.hpp.
#include "neo/Jieba.hpp"

#include "CutCompareEngines.hpp"

#include <cstddef>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The Neo cut mode that matches each compared mode.
auto neo_mode(CompareMode mode) -> neo_cppjieba::CutMode {
    switch (mode) {
        case CompareMode::Mix:
            return neo_cppjieba::CutMode::MIX;
        case CompareMode::Mp:
            return neo_cppjieba::CutMode::MIX_NO_HMM;
        case CompareMode::Full:
            return neo_cppjieba::CutMode::FULL;
        case CompareMode::Search:
            return neo_cppjieba::CutMode::SEARCH;
        case CompareMode::Hmm:
            return neo_cppjieba::CutMode::HMM;
    }
    std::unreachable();
}

} // namespace

struct NeoEngine::Impl {
    Impl(const std::string &dict_path, const std::string &model_path) : jieba{dict_path, model_path, ""} {
    }

    neo_cppjieba::Jieba jieba;
    neo_cppjieba::Workspace workspace;
    std::vector<neo_cppjieba::TokenPosition> positions;
};

NeoEngine::NeoEngine(const std::string &dict_path, const std::string &model_path)
    : impl_{std::make_unique<Impl>(dict_path, model_path)} {
}

NeoEngine::~NeoEngine() = default;

auto NeoEngine::cut_strings(const std::string &line, CompareMode mode) const -> std::vector<std::string> {
    return impl_->jieba.cut_strings(line, neo_mode(mode));
}

// Verify every Neo output representation before timing its allocation strategy.
auto NeoEngine::verify_outputs(const std::string &line, CompareMode mode, const std::vector<std::string> &expected)
    -> void {
    auto &impl = *impl_;
    const auto cut_mode = neo_mode(mode);
    const auto borrowed = impl.jieba.cut(line, cut_mode);
    impl.jieba.cut_into(line, cut_mode, impl.positions, impl.workspace);
    if (borrowed.size() != expected.size() || impl.positions.size() != expected.size()) {
        throw std::runtime_error("Neo output representations have different token counts");
    }
    for (auto i = std::size_t{0}; i < expected.size(); ++i) {
        if (borrowed[i].word != expected[i] || borrowed[i].position != impl.positions[i]
            || impl.positions[i].source.slice(std::string_view{line}) != expected[i]) {
            throw std::runtime_error("Neo borrowed, reused and string outputs disagree");
        }
    }
}

auto NeoEngine::bench(const std::vector<std::string> &lines, CompareMode mode, NeoOutput output, std::size_t rounds)
    -> CutMeasurement {
    auto &impl = *impl_;
    const auto cut_mode = neo_mode(mode);
    switch (output) {
        case NeoOutput::Strings:
            return bench_cut([&](const std::string &line) { return impl.jieba.cut_strings(line, cut_mode); }, lines,
                             rounds);
        case NeoOutput::Borrowed:
            return bench_cut([&](const std::string &line) { return impl.jieba.cut(line, cut_mode); }, lines, rounds);
        case NeoOutput::Reused:
            return bench_cut(
                [&](const std::string &line) -> const std::vector<neo_cppjieba::TokenPosition> & {
                    impl.jieba.cut_into(line, cut_mode, impl.positions, impl.workspace);
                    return impl.positions;
                },
                lines, rounds);
    }
    std::unreachable();
}

// Export UTF-8 byte ranges outside timing so other languages can compare exact token sequences.
auto NeoEngine::write_ranges(std::ostream &out, CompareMode mode, const std::vector<std::string> &lines) -> void {
    auto &impl = *impl_;
    const auto cut_mode = neo_mode(mode);
    out << '[';
    for (auto line = std::size_t{0}; line < lines.size(); ++line) {
        impl.jieba.cut_into(lines[line], cut_mode, impl.positions, impl.workspace);
        out << (line == 0 ? "[" : ",[");
        for (auto token = std::size_t{0}; token < impl.positions.size(); ++token) {
            const auto &range = impl.positions[token].source;
            out << (token == 0 ? "[" : ",[") << static_cast<std::size_t>(range.begin) << ','
                << static_cast<std::size_t>(range.end) << ']';
        }
        out << ']';
    }
    out << ']';
}

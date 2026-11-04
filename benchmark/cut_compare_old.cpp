// Old CppJieba engine of cut_compare_benchmark, alone in its translation unit; see CutCompareEngines.hpp.
#include "cppjieba/Jieba.hpp"

#include "CutCompareEngines.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct OldEngine::Impl {
    // cppjieba::Jieba declares a destructor and cannot move, so it is built in place.
    Impl(const std::string &dict_path, const std::string &model_path, const std::string &user_dict_path)
        : jieba{dict_path, model_path, user_dict_path} {
    }

    cppjieba::Jieba jieba;

    // Dispatch one line to the legacy call matching mode.
    [[nodiscard]]
    auto cut(const std::string &line, CompareMode mode) const -> std::vector<std::string> {
        auto words = std::vector<std::string>{};
        switch (mode) {
            case CompareMode::Mix:
                jieba.Cut(line, words, true);
                break;
            case CompareMode::Mp:
                jieba.Cut(line, words, false);
                break;
            case CompareMode::Full:
                jieba.CutAll(line, words);
                break;
            case CompareMode::Search:
                jieba.CutForSearch(line, words, true);
                break;
            case CompareMode::Hmm:
                jieba.CutHMM(line, words);
                break;
        }
        return words;
    }
};

OldEngine::OldEngine(const std::string &dict_path, const std::string &model_path, const std::string &user_dict_path)
    : impl_{std::make_unique<Impl>(dict_path, model_path, user_dict_path)} {
}

OldEngine::~OldEngine() = default;

auto OldEngine::cut(const std::string &line, CompareMode mode) const -> std::vector<std::string> {
    return impl_->cut(line, mode);
}

auto OldEngine::bench(const std::vector<std::string> &lines, CompareMode mode, std::size_t rounds) const
    -> CutMeasurement {
    const auto &impl = *impl_;
    return bench_cut([&](const std::string &line) { return impl.cut(line, mode); }, lines, rounds);
}

/// Workload: one deterministic phase per run, for counting instructions under callgrind
///
///   instruction_workload <phase> <dict> <model> <idf> <stop-words> <corpus>
///
/// Each phase runs in its own measure_<phase> function, reached through a table chosen at run time so the compiler
/// cannot inline it; `valgrind --tool=callgrind --toggle-collect=*measure_<phase>*` counts only that phase, not
/// the setup before it. tools/instruction-counts.sh builds this file against two revisions and compares the counts.
/// It uses only public headers, so it also builds against earlier releases that have the same API.
#include "neo/Jieba.hpp"
#include "neo/KeywordExtractor.hpp"
#include "neo/options/CutMode.hpp"
#include "neo/options/KeywordOptions.hpp"
#include "neo/value/DictSource.hpp"
#include "neo/value/PosTag.hpp"
#include "neo/value/SnapshotSource.hpp"
#include "neo/value/TaggedToken.hpp"
#include "neo/value/TokenView.hpp"
#include "neo/workspace/Workspace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace neo_cppjieba;

namespace {

constexpr auto modes = std::array{CutMode::MIX,           CutMode::MIX_NO_HMM, CutMode::FULL, CutMode::SEARCH,
                                  CutMode::SEARCH_NO_HMM, CutMode::HMM,        CutMode::MP};
constexpr auto keywords_per_line = std::size_t{20};

/// Files every phase reads; the corpus holds one text per nonempty line.
struct Inputs {
    std::string dict;
    std::string model;
    std::string idf;
    std::string stop_words;
    std::vector<std::string> lines;
};

// Read the corpus's nonempty lines without their line endings.
auto read_lines(const std::string &path) -> std::vector<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    auto lines = std::vector<std::string>{};
    for (auto line = std::string{}; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

// Fold a cut's coordinates and tags into the checksum the program prints, so no phase is optimized away.
auto fold(std::uint64_t checksum, std::uint64_t value) -> std::uint64_t {
    return (checksum ^ value) * 0x100000001B3ULL;
}

/// What a measured phase reads; the engine and snapshot are prepared before collection starts.
struct Context {
    const Inputs &inputs;
    const Jieba *jieba;
    std::span<const std::byte> snapshot;
};

// Build an engine from the text dictionaries.
auto measure_load(const Context &context) -> std::uint64_t {
    const auto jieba = Jieba{context.inputs.dict, context.inputs.model, ""};
    return jieba.lookup_tag(std::string_view{"北京"}).raw();
}

// Build an engine from snapshot bytes.
auto measure_snapshot_load(const Context &context) -> std::uint64_t {
    const auto jieba = Jieba::from_snapshot(SnapshotSource::memory(context.snapshot, "<workload>"),
                                            DictSource::file(context.inputs.model));
    return jieba.lookup_tag(std::string_view{"北京"}).raw();
}

// Cut every line in every mode, reusing one workspace and output vector.
auto measure_cut(const Context &context) -> std::uint64_t {
    auto checksum = std::uint64_t{0};
    auto workspace = Workspace{};
    auto positions = std::vector<TokenPosition>{};
    for (const auto mode : modes) {
        for (const auto &line : context.inputs.lines) {
            positions.clear();
            context.jieba->cut_into(std::string_view{line}, mode, positions, workspace);
            for (const auto &position : positions) {
                checksum = fold(checksum, position.source.end);
            }
        }
    }
    return checksum;
}

// Cut and tag every line in MIX mode.
auto measure_tag(const Context &context) -> std::uint64_t {
    auto checksum = std::uint64_t{0};
    auto workspace = Workspace{};
    auto positions = std::vector<TaggedPosition>{};
    for (const auto &line : context.inputs.lines) {
        positions.clear();
        context.jieba->cut_into(std::string_view{line}, CutMode::MIX, positions, workspace);
        for (const auto &position : positions) {
            checksum = fold(checksum, position.tag.raw());
        }
    }
    return checksum;
}

// Load both keyword extractors and extract keywords from every line with the default options.
auto measure_keywords(const Context &context) -> std::uint64_t {
    const auto tfidf = TfIdfExtractor{context.inputs.idf, context.inputs.stop_words};
    const auto textrank = TextRankExtractor{context.inputs.stop_words};
    auto checksum = std::uint64_t{0};
    for (const auto &line : context.inputs.lines) {
        const auto text = std::string_view{line};
        checksum = fold(checksum, tfidf.extract(*context.jieba, text, keywords_per_line, TfIdfOptions{}).size());
        checksum = fold(checksum, textrank.extract(*context.jieba, text, keywords_per_line, TextRankOptions{}).size());
    }
    return checksum;
}

// What a phase needs prepared before its measured function runs.
enum class Setup : std::uint8_t { None, Engine, Snapshot };

/// One phase: its name on the command line, its setup, and the function that measures it.
struct Phase {
    std::string_view name;
    Setup setup;
    std::uint64_t (*measure)(const Context &);
};

// Called through this table only, so no measured function is inlined into main.
constexpr auto phases = std::array{
    Phase{"load", Setup::None, measure_load},           Phase{"snapshot_load", Setup::Snapshot, measure_snapshot_load},
    Phase{"cut", Setup::Engine, measure_cut},           Phase{"tag", Setup::Engine, measure_tag},
    Phase{"keywords", Setup::Engine, measure_keywords},
};

// Prepare the phase's engine or snapshot, then run its measured function.
auto run_phase(const Phase &phase, const Inputs &inputs) -> std::uint64_t {
    switch (phase.setup) {
        case Setup::Engine: {
            const auto jieba = Jieba{inputs.dict, inputs.model, ""};
            return phase.measure(Context{inputs, &jieba, {}});
        }
        case Setup::Snapshot: {
            const auto snapshot =
                Jieba::encode_dict_snapshot(DictSource::file(inputs.dict), std::span<const DictSource>{});
            return phase.measure(Context{inputs, nullptr, snapshot});
        }
        case Setup::None: {
            return phase.measure(Context{inputs, nullptr, {}});
        }
    }
    return 0;
}

// The phase with this name, or null. std::array iterators are pointers in libstdc++, where clang-tidy wants
// `auto *`, but classes in MSVC; returning a pointer works with both.
auto find_phase(std::string_view name) -> const Phase * {
    for (const auto &phase : phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

} // namespace

auto main(int argc, char **argv) -> int {
    const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
    const auto *const phase = arguments.size() == 7 ? find_phase(arguments[1]) : nullptr;
    if (phase == nullptr) {
        std::fputs("usage: instruction_workload <load|snapshot_load|cut|tag|keywords> <dict> <model> <idf> "
                   "<stop-words> <corpus>\n",
                   stderr);
        return 2;
    }
    const auto inputs = Inputs{arguments[2], arguments[3], arguments[4], arguments[5], read_lines(arguments[6])};
    std::printf("%s checksum %llu\n", arguments[1], static_cast<unsigned long long>(run_phase(*phase, inputs)));
    return 0;
}

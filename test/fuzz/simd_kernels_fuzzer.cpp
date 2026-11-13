#include "neo/Config.hpp"
#include "neo/algorithm/Separator.hpp"
#include "neo/resource/HMMState.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/simd/RuneMatch.hpp"
#include "neo/simd/SimdLevel.hpp"
#include "neo/simd/Viterbi.hpp"

#include "FuzzUtils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;
using namespace neo_cppjieba::detail::simd;

namespace {

// The kernel one input exercises, chosen by its first byte.
enum class Kernel : std::uint8_t { Separators, Viterbi, LegalViterbi };
constexpr auto kernel_count = std::uint8_t{3};

// Runes beside the separators in value or in the low bitmask, values a signed lane compare would misread, and an
// ordinary rune; a choice byte from raw_rune_choice on reads a raw 32-bit rune instead.
constexpr auto near_separator_runes =
    std::array<char32_t, 17>{8,      11,     31,     33,      0x49,    0x4A,       0x60,       0x2002, 0x3001,
                             0x3003, 0xFF0B, 0xFF0D, 0x1FF0C, 0x13002, 0x80000020, 0xFFFFFFFF, U'中'};
constexpr auto raw_rune_choice = std::uint8_t{0xC0};

// Scores that tie, cancel to signed zeros or sit at the CppJieba floor; a choice byte from raw_score_choice on reads
// a raw double, kept only when finite and at most max_raw_score in magnitude so 512 columns cannot overflow.
constexpr auto score_pool = std::array<double, 8>{-1.5, -2.25, -0.75, 0.0, -0.0, -1.0e-300, 1.5, MIN_DOUBLE};
constexpr auto raw_score_choice = std::uint8_t{0xF0};
constexpr auto max_raw_score = 1.0e100;

constexpr auto max_runes = std::size_t{4096};
constexpr auto max_columns = std::size_t{512};

using PathRow = std::array<HMMState, 4>;

// Read sizeof(T) bytes as a T; bytes past the end of the input read as zero.
template <typename T>
auto read_raw(ByteReader &reader) -> T {
    auto bytes = std::array<char, sizeof(T)>{};
    std::ranges::copy(reader.text(sizeof(T)), bytes.begin());
    return std::bit_cast<T>(bytes);
}

// Read one rune: a separator, a pooled near miss, or a raw 32-bit value.
auto read_rune(ByteReader &reader) -> char32_t {
    const auto choice = reader.byte();
    if (choice >= raw_rune_choice) {
        return read_raw<char32_t>(reader);
    }
    const auto index = static_cast<std::size_t>(choice) % (DEFAULT_SEPARATORS.size() + near_separator_runes.size());
    return index < DEFAULT_SEPARATORS.size() ? DEFAULT_SEPARATORS[index]
                                             : near_separator_runes[index - DEFAULT_SEPARATORS.size()];
}

// Read one score: a pooled value, or a bounded raw double.
auto read_score(ByteReader &reader) -> double {
    const auto choice = reader.byte();
    if (choice < raw_score_choice) {
        return score_pool[choice % score_pool.size()];
    }
    const auto raw = read_raw<double>(reader);
    return std::isfinite(raw) && std::abs(raw) <= max_raw_score ? raw : 0.0;
}

// Read the scores of the four states.
auto read_scores(ByteReader &reader) -> StateScores {
    auto scores = StateScores{};
    std::ranges::generate(scores, [&] { return read_score(reader); });
    return scores;
}

// The default-separator matches of a rune sequence from every compiled backend must equal the scalar reference.
auto check_separators(ByteReader &reader) -> void {
    auto runes = std::vector<char32_t>{};
    while (!reader.empty() && runes.size() < max_runes) {
        runes.push_back(read_rune(reader));
    }
    const auto span = std::span<const char32_t>{runes};
    auto reference = std::vector<RuneIndex>{};
    scalar::append_matches(span, is_default_separator_unlikely, reference);
    for_each_compiled_backend([&]<SimdLevel Backend>() {
        auto matches = std::vector<RuneIndex>{};
        append_matches<Backend>(span, DEFAULT_SEPARATORS, is_default_separator_unlikely, matches);
        require(matches == reference, "SIMD separator matches differ from the scalar reference");
    });
}

// The column-0 weights, transition rows and per-column emissions of one forward pass.
struct ViterbiInput {
    StateScores start;
    std::array<StateScores, 4> transitions;
    std::vector<StateScores> emissions;
};

// Read a run of 1 to max_columns columns; column 0's emissions are unused, as in hmm_internal_cut.
auto read_viterbi_input(ByteReader &reader) -> ViterbiInput {
    auto input = ViterbiInput{
        .start = read_scores(reader),
        .transitions = std::array{read_scores(reader), read_scores(reader), read_scores(reader), read_scores(reader)},
        .emissions = std::vector<StateScores>(1)};
    while (!reader.empty() && input.emissions.size() < max_columns) {
        input.emissions.push_back(read_scores(reader));
    }
    return input;
}

// A forward pass's predecessor rows and final weights, with weights kept as bit patterns so signed zeros differ.
struct ViterbiResult {
    std::vector<PathRow> path;
    std::array<std::uint64_t, 4> weight_bits;

    auto operator==(const ViterbiResult &) const -> bool = default;
};

// Run one backend over the input; every row starts as S so a row the pass skips stays visible.
template <typename Forward>
auto run_viterbi(const Forward &forward, const ViterbiInput &input) -> ViterbiResult {
    auto path =
        std::vector<PathRow>(input.emissions.size(), PathRow{HMMState::S, HMMState::S, HMMState::S, HMMState::S});
    const auto emit_at = [&](std::size_t x) {
        return input.emissions[x];
    };
    const auto weights = forward(input.start, std::span{input.transitions}, emit_at, std::span{path});
    auto result = ViterbiResult{.path = std::move(path), .weight_bits = {}};
    std::ranges::transform(weights, result.weight_bits.begin(),
                           [](double weight) { return std::bit_cast<std::uint64_t>(weight); });
    return result;
}

// The Python/Rust-rule scalar reference.
constexpr auto scalar_legal_forward = [](const auto &start, auto transitions, const auto &emit_at, auto path) {
    return scalar::viterbi_forward_legal(start, transitions, emit_at, path);
};

// The CppJieba-rule forward pass from every compiled backend must equal the scalar reference for any floor.
auto check_viterbi(ByteReader &reader) -> void {
    const auto floor_choice = reader.byte();
    const auto floor = ViterbiFloor<HMMState>{.score = score_pool[(floor_choice >> 2) % score_pool.size()],
                                              .state = static_cast<HMMState>(floor_choice & 3)};
    const auto input = read_viterbi_input(reader);
    const auto scalar_forward = [&](const auto &start, auto transitions, const auto &emit_at, auto path) {
        return scalar::viterbi_forward(start, transitions, floor, emit_at, path);
    };
    const auto reference = run_viterbi(scalar_forward, input);
    for_each_compiled_backend([&]<SimdLevel Backend>() {
        const auto backend_forward = [&](const auto &start, auto transitions, const auto &emit_at, auto path) {
            return viterbi_forward<Backend>(start, transitions, floor, emit_at, path);
        };
        require(run_viterbi(backend_forward, input) == reference,
                "SIMD CppJieba Viterbi differs from the scalar reference");
    });
}

// The Python/Rust-rule forward pass from every compiled backend must equal the scalar reference.
auto check_legal_viterbi(ByteReader &reader) -> void {
    const auto input = read_viterbi_input(reader);
    const auto reference = run_viterbi(scalar_legal_forward, input);
    for_each_compiled_backend([&]<SimdLevel Backend>() {
        const auto backend_forward = [](const auto &start, auto transitions, const auto &emit_at, auto path) {
            return viterbi_forward_legal<Backend>(start, transitions, emit_at, path);
        };
        require(run_viterbi(backend_forward, input) == reference,
                "SIMD Python/Rust Viterbi differs from the scalar reference");
    });
}

} // namespace

// Each SIMD kernel the build compiles must match its scalar reference bit for bit; OFF builds compare the scalar
// dispatch with itself.
extern
"C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
    auto reader = ByteReader{data, size};
    switch (static_cast<Kernel>(reader.byte() % kernel_count)) {
        case Kernel::Separators:
            check_separators(reader);
            break;
        case Kernel::Viterbi:
            check_viterbi(reader);
            break;
        case Kernel::LegalViterbi:
            check_legal_viterbi(reader);
            break;
    }
    return 0;
}

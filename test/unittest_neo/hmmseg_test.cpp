#include "../SegmentTestUtils.hpp"
#include "../TestUtils.hpp"
#include "gtest/gtest.h"
#include "neo/Config.hpp"
#include "neo/algorithm/HMMSegment.hpp"
#include "neo/algorithm/MixSegment.hpp"
#include "neo/algorithm/QuerySegment.hpp"
#include "neo/algorithm/SegmentCommon.hpp"
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMMState.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/Rune.hpp"
#include "neo/workspace/SegmentScratch.hpp"

#include "test_paths.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

using namespace neo_cppjieba;
using namespace neo_cppjieba::detail;

inline constexpr auto HMM_MODEL_FILE = std::string_view{DICT_DIR "/hmm_model.utf8"};

namespace {

// Model parsing tests own their input files in an isolated temporary directory.
class HMModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = create_temp_directory("neo-hmm-model-");
        write_model(model_lines_);
    }

    void TearDown() override {
        if (!directory_.empty()) {
            auto error = std::error_code{};
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    [[nodiscard]] auto model_path() const -> std::string {
        return path_to_utf8(directory_ / "model.utf8");
    }

    void write_model(std::span<const std::string> lines) const {
        auto output = std::ofstream{directory_ / "model.utf8", std::ios::binary};
        ASSERT_TRUE(output.is_open());
        for (const auto &line : lines) {
            output << line << '\n';
        }
        output.close();
        ASSERT_TRUE(output.good());
    }

    const std::vector<std::string> model_lines_{
        "-1 -2 -3 -4", "-1 -2 -3 -4", "-5 -6 -7 -8", "-9 -10 -11 -12", "-13 -14 -15 -16",
        "甲:-1,𠮷:0",  "甲:-2",       "甲:-3",       "甲:-4",
    };
    std::filesystem::path directory_;
};

} // namespace

TEST(HMMStateTest, LabelsRemainAvailableAtCompileTime) {
    constexpr auto labels = std::array{get_hmm_state_label(HMMState::B), get_hmm_state_label(HMMState::E),
                                       get_hmm_state_label(HMMState::M), get_hmm_state_label(HMMState::S)};
    EXPECT_EQ(labels, (std::array{'B', 'E', 'M', 'S'}));
}

TEST_F(HMModelTest, LoadsProbabilitiesForEveryState) {
    const auto model = HMModel{model_path()};
    constexpr auto states = std::array{HMMState::B, HMMState::E, HMMState::M, HMMState::S};
    for (auto i = size_t{0}; i < states.size(); ++i) {
        EXPECT_DOUBLE_EQ(model.get_start_prob(states[i]), -static_cast<double>(i + 1));
        EXPECT_DOUBLE_EQ(model.get_emit_prob(states[i], U'甲'), -static_cast<double>(i + 1));
        for (auto j = size_t{0}; j < states.size(); ++j) {
            EXPECT_DOUBLE_EQ(model.get_trans_prob(states[i], states[j]),
                             -static_cast<double>(i * states.size() + j + 1));
        }
    }
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'𠮷'), 0.0);
}

TEST_F(HMModelTest, FetchesAllEmissionStatesForARune) {
    const auto model = HMModel{model_path()};
    EXPECT_EQ(model.get_emit_probs(U'甲'), (EmitProbabilities{-1.0, -2.0, -3.0, -4.0}));
    EXPECT_EQ(model.get_emit_probs(U'𠮷'), (EmitProbabilities{0.0, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}));
}

TEST_F(HMModelTest, CloneKeepsEveryProbability) {
    static_assert(!std::is_copy_constructible_v<HMModel> && std::is_nothrow_move_constructible_v<HMModel>);
    const auto model = HMModel{model_path()};
    const auto copy = model.clone();
    constexpr auto states = std::array{HMMState::B, HMMState::E, HMMState::M, HMMState::S};
    for (const auto from : states) {
        EXPECT_EQ(copy.get_start_prob(from), model.get_start_prob(from));
        for (const auto to : states) {
            EXPECT_EQ(copy.get_trans_prob(from, to), model.get_trans_prob(from, to));
        }
    }
    EXPECT_EQ(copy.get_emit_probs(U'甲'), model.get_emit_probs(U'甲'));
    EXPECT_EQ(copy.get_emit_probs(U'𠮷'), model.get_emit_probs(U'𠮷'));
}

TEST_F(HMModelTest, MissingEmissionsRemainIndependentForEachState) {
    auto lines = model_lines_;
    lines[5] = "甲:0";
    lines[6] = "乙:-2";
    lines[7] = "𠮷:-3";
    lines[8] = "丙:-4";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_EQ(model.get_emit_probs(U'甲'), (EmitProbabilities{0.0, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}));
    EXPECT_EQ(model.get_emit_probs(U'乙'), (EmitProbabilities{MIN_DOUBLE, -2.0, MIN_DOUBLE, MIN_DOUBLE}));
    EXPECT_EQ(model.get_emit_probs(U'𠮷'), (EmitProbabilities{MIN_DOUBLE, MIN_DOUBLE, -3.0, MIN_DOUBLE}));
    EXPECT_EQ(model.get_emit_probs(U'丙'), (EmitProbabilities{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, -4.0}));
    for (const auto rune : {U'\0', U'A', U'外', U'\uffff', U'😀'}) {
        EXPECT_EQ(model.get_emit_probs(rune), (EmitProbabilities{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}));
    }
}

TEST_F(HMModelTest, DistinguishesBmpAndSupplementaryEmissionKeys) {
    auto lines = model_lines_;
    lines[5] = "甲:-1,\uffff:0,\U00010000:-2,\U0010ffff:-3";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'\uffff'), 0.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'\U00010000'), -2.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'\U0010ffff'), -3.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'\U00010001'), MIN_DOUBLE);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::S, U'\uffff'), MIN_DOUBLE);
}

TEST_F(HMModelTest, AcceptsNullRuneEmissionKey) {
    auto lines = model_lines_;
    lines[5] = "甲:-1,";
    lines[5].push_back('\0');
    lines[5] += ":0";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_EQ(model.get_emit_probs(U'\0'), (EmitProbabilities{0.0, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}));
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'甲'), -1.0);
}

TEST_F(HMModelTest, LoadsModelsWithOnlySupplementaryEmissions) {
    auto lines = model_lines_;
    lines[5] = "𠮷:0";
    lines[6] = "😀:-1";
    lines[7] = "𠮷:-2";
    lines[8] = "😀:-3";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_EQ(model.get_emit_probs(U'𠮷'), (EmitProbabilities{0.0, MIN_DOUBLE, -2.0, MIN_DOUBLE}));
    EXPECT_EQ(model.get_emit_probs(U'😀'), (EmitProbabilities{MIN_DOUBLE, -1.0, MIN_DOUBLE, -3.0}));
    EXPECT_EQ(model.get_emit_probs(U'甲'), (EmitProbabilities{MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE, MIN_DOUBLE}));
}

TEST_F(HMModelTest, AcceptsSupportedProbabilityBounds) {
    auto lines = model_lines_;
    lines[0] = "0 -3.14e100 -1 -2";
    lines[1] = "0 -3.14e100 -1 -2";
    lines[5] = "甲:0,𠮷:-3.14e100";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_DOUBLE_EQ(model.get_start_prob(HMMState::B), 0.0);
    EXPECT_DOUBLE_EQ(model.get_start_prob(HMMState::E), MIN_DOUBLE);
    EXPECT_DOUBLE_EQ(model.get_trans_prob(HMMState::B, HMMState::B), 0.0);
    EXPECT_DOUBLE_EQ(model.get_trans_prob(HMMState::B, HMMState::E), MIN_DOUBLE);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'甲'), 0.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'𠮷'), MIN_DOUBLE);
}

TEST_F(HMModelTest, RejectsInvalidLogProbabilitiesInEveryParameterSet) {
    for (const auto *const token : {"nan", "inf", "-inf", "0.1", "-3.15e100"}) {
        for (auto row = size_t{0}; row < model_lines_.size(); ++row) {
            auto lines = model_lines_;
            lines[row] = row < 5 ? std::string{token} + " -1 -1 -1" : "甲:" + std::string{token};
            ASSERT_NO_FATAL_FAILURE(write_model(lines));
            EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception)
                << "data line " << row + 1 << ", value " << token;
        }
    }
}

TEST_F(HMModelTest, RejectsMalformedProbabilityTokens) {
    for (const auto *const token : {"", "-1oops", "1e400"}) {
        for (const auto row : {size_t{0}, size_t{1}, size_t{5}}) {
            auto lines = model_lines_;
            lines[row] = row < 5 ? std::string{token} + " -1 -1 -1" : "甲:" + std::string{token};
            ASSERT_NO_FATAL_FAILURE(write_model(lines));
            EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception)
                << "data line " << row + 1 << ", value " << token;
        }
    }
}

TEST_F(HMModelTest, RejectsWrongProbabilityColumnCounts) {
    for (const auto *const probability_line : {"0 0 0", "0 0 0 0 0"}) {
        for (auto row = size_t{0}; row < 5; ++row) {
            auto lines = model_lines_;
            lines[row] = probability_line;
            ASSERT_NO_FATAL_FAILURE(write_model(lines));
            EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception) << "data line " << row + 1;
        }
    }
}

TEST_F(HMModelTest, RejectsMissingModelData) {
    for (const auto count : {size_t{0}, model_lines_.size() - 1}) {
        ASSERT_NO_FATAL_FAILURE(write_model(std::span<const std::string>{model_lines_}.first(count)));
        EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception);
    }
}

TEST_F(HMModelTest, RejectsAdditionalModelData) {
    auto lines = model_lines_;
    lines.emplace_back("甲:-1");
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception);
}

TEST_F(HMModelTest, LoadsCrLfModelWithCommentsAndBlankLines) {
    auto lines = model_lines_;
    lines.insert(lines.begin(), {"# header", "", " \t# indented comment", " \t"});
    lines.emplace_back("# trailing comment");
    for (auto &line : lines) {
        line += '\r';
    }
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_DOUBLE_EQ(model.get_start_prob(HMMState::B), -1.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::S, U'甲'), -4.0);
}

TEST_F(HMModelTest, RejectsEmptyEmissionTables) {
    for (auto row = size_t{5}; row < model_lines_.size(); ++row) {
        auto lines = model_lines_;
        lines[row] = ",,,";
        ASSERT_NO_FATAL_FAILURE(write_model(lines));
        EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception) << "data line " << row + 1;
    }
}

TEST_F(HMModelTest, RejectsDuplicateEmissionKeys) {
    for (const auto *const emission_line : {"甲:-1,甲:-2", "甲:-1,甲:-1", "甲:-3.14e100,甲:0", "𠮷:-3.14e100,𠮷:0"}) {
        for (auto row = size_t{5}; row < model_lines_.size(); ++row) {
            auto lines = model_lines_;
            lines[row] = emission_line;
            ASSERT_NO_FATAL_FAILURE(write_model(lines));
            EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception) << "data line " << row + 1;
        }
    }
}

TEST_F(HMModelTest, RejectsEmissionEntriesWithoutAColon) {
    auto lines = model_lines_;
    lines[5] = "甲-1";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception);
}

TEST_F(HMModelTest, RejectsEmissionKeysThatAreNotSingleUnicodeScalars) {
    for (const auto *const emission_line : {":-1", "甲乙:-1", "\xFF:-1"}) {
        auto lines = model_lines_;
        lines[5] = emission_line;
        ASSERT_NO_FATAL_FAILURE(write_model(lines));
        EXPECT_THROW((HMModel{model_path()}), LogConfig::Exception);
    }
}

TEST_F(HMModelTest, AcceptsEmptyEmissionFieldsAroundValidEntries) {
    auto lines = model_lines_;
    lines[5] = ",甲:-1,,𠮷:0,";
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'甲'), -1.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'𠮷'), 0.0);
    EXPECT_DOUBLE_EQ(model.get_emit_prob(HMMState::B, U'外'), MIN_DOUBLE);
}

TEST_F(HMModelTest, MissingRunesUseTheExistingFallback) {
    const auto model = HMModel{model_path()};
    for (const auto state : {HMMState::B, HMMState::E, HMMState::M, HMMState::S}) {
        EXPECT_DOUBLE_EQ(model.get_emit_prob(state, U'外'), MIN_DOUBLE);
    }
}

TEST_F(HMModelTest, UnknownRunesStillProduceContiguousRanges) {
    const auto model = HMModel{model_path()};
    const auto runes = decode("外𠀀");
    const auto ranges = cut<HMMSegment>(unused_dict(), model, runes);
    EXPECT_EQ(to_strings(runes, ranges), (std::vector<std::string>{"外", "𠀀"}));
}

TEST_F(HMModelTest, ViterbiSelectsTheHighestScoringCompletePath) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 -1", "-10 0 -10 -10",  "-10 -10 -10 -10", "-10 -10 -10 -10", "-10 -10 -10 0",
        "甲:0,乙:0,丙:0", "甲:0,乙:0,丙:0", "甲:0,乙:0,丙:0",  "甲:0,乙:0,丙:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙丙");
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, std::span<const Rune>{runes}.first(1)),
              (std::vector<WordRange>{{0, 1}}));
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, std::span<const Rune>{runes}.first(2)),
              (std::vector<WordRange>{{0, 2}}));
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), (std::vector<WordRange>{{0, 1}, {1, 2}, {2, 3}}));
}

TEST_F(HMModelTest, ViterbiKeepsTheFirstPredecessorWhenScoresTie) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 0", "-100 0 -100 -100", "-100 -100 -100 -100", "-100 -100 -100 -100", "-100 0 -100 -100",
        "甲:0,乙:0",     "甲:0,乙:0",        "甲:0,乙:0",           "甲:0,乙:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙");
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), (std::vector<WordRange>{{0, 2}}));
}

TEST_F(HMModelTest, ViterbiPrefersEndStateWhenFinalScoresTie) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 0", "-100 0 -100 -100", "-100 -100 -100 -100", "-100 -100 -100 -100", "-100 -100 -100 0",
        "甲:0,乙:0",     "甲:0,乙:0",        "甲:0,乙:0",           "甲:0,乙:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙");
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), (std::vector<WordRange>{{0, 2}}));
}

TEST_F(HMModelTest, ViterbiPreservesWordBoundariesAcrossLongRuns) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 0",        "-100 0 -100 -100",        "0 -100 -100 0",
        "-100 -100 -100 -100",  "0 -100 -100 0",           "甲:0,乙:-100,𠮷:-100",
        "甲:-100,乙:0,𠮷:-100", "甲:-100,乙:-100,𠮷:-100", "甲:-100,乙:-100,𠮷:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    constexpr auto pair_count = RuneIndex{1024};
    auto runes = Unicode{};
    auto expected = std::vector<WordRange>{};
    runes.reserve(2 * pair_count + 1);
    expected.reserve(pair_count + 1);
    for (auto i = RuneIndex{0}; i < pair_count; ++i) {
        runes.push_back(U'甲');
        runes.push_back(U'乙');
        expected.push_back({2 * i, 2 * i + 2});
    }
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), expected);
    runes.push_back(U'𠮷');
    expected.push_back({2 * pair_count, 2 * pair_count + 1});
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), expected);
}

TEST_F(HMModelTest, ViterbiReconstructsLongWordsAndSingletonsAcrossAllStates) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 0",
        "-100 -100 0 -100",
        "0 -100 -100 0",
        "-100 0 0 -100",
        "0 -100 -100 0",
        "甲:0,乙:-100,丙:-100,丁:-100,𠮷:-100",
        "甲:-100,乙:-100,丙:-100,丁:0,𠮷:-100",
        "甲:-100,乙:0,丙:0,丁:-100,𠮷:-100",
        "甲:-100,乙:-100,丙:-100,丁:-100,𠮷:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    constexpr auto block_count = RuneIndex{1024};
    auto runes = Unicode{};
    auto expected = std::vector<WordRange>{};
    runes.reserve(std::size_t{5} * block_count);
    expected.reserve(std::size_t{2} * block_count);
    for (auto i = RuneIndex{0}; i < block_count; ++i) {
        runes.insert(runes.end(), {U'甲', U'乙', U'丙', U'丁', U'𠮷'});
        expected.push_back({5 * i, 5 * i + 4});
        expected.push_back({5 * i + 4, 5 * i + 5});
    }
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), expected);
    runes.pop_back();
    expected.pop_back();
    EXPECT_EQ(cut<HMMSegment>(unused_dict(), model, runes), expected);
}

TEST_F(HMModelTest, ViterbiKeepsRuneOffsetsAcrossAsciiAndSeparatorBoundaries) {
    const auto model = HMModel{model_path()};
    const auto runes = decode("前AB12甲甲甲，3.14甲甲后");
    const auto input = std::span<const Rune>{runes}.subspan(1, runes.size() - 2);
    const auto result = cut<HMMSegment>(unused_dict(), model, input);

    EXPECT_EQ(to_strings(input, result), (std::vector<std::string>{"AB12", "甲甲甲", "，", "3.14", "甲甲"}));
    EXPECT_EQ(result, (std::vector<WordRange>{{0, 4}, {4, 7}, {7, 8}, {8, 12}, {12, 14}}));
}

TEST_F(HMModelTest, ViterbiPreservesLegacyFallbackBelowMinimumInitialProbability) {
    const auto lines = std::vector<std::string>{
        "-2e100 -3e100 -3e100 -3e100",
        "-3e100 0 -3e100 -3e100",
        "-3e100 -3e100 -3e100 -3e100",
        "-3e100 -3e100 -3e100 -3e100",
        "-3e100 -3e100 -3e100 -3e100",
        "甲:-2e100,乙:-3e100",
        "甲:-3e100,乙:0",
        "甲:-3e100,乙:-3e100",
        "甲:-3e100,乙:-3e100",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<HMMSegment>(unused_dict(), model, runes)), (std::vector<std::string>{"甲", "乙"}));
}

TEST_F(HMModelTest, ViterbiPreservesLegacyFloorForAccumulatedProbabilities) {
    const auto lines = std::vector<std::string>{
        "0 -3.14e100 -3.14e100 -2e100",
        "-3.14e100 0 0 -3.14e100",
        "0 -3.14e100 -3.14e100 0",
        "-3.14e100 -0.25e100 0 -3.14e100",
        "0 -3.14e100 -3.14e100 0",
        "甲:-1e100,乙:-1e100,丙:-1e100,丁:-1e100",
        "甲:-1e100,乙:-1e100,丙:-1e100,丁:-1e100",
        "甲:-1e100,乙:-1e100,丙:-1e100,丁:-1e100",
        "甲:-1e100,乙:-1e100,丙:-1e100,丁:-1e100",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙丙丁");
    EXPECT_EQ(to_strings(runes, cut<HMMSegment>(unused_dict(), model, runes)),
              (std::vector<std::string>{"甲乙", "丙", "丁"}));
}

TEST_F(HMModelTest, StoredMinimumEmissionPreservesLegacyFallback) {
    const auto lines = std::vector<std::string>{
        "0 -3.14e100 -3.14e100 -3.14e100",
        "-3.14e100 0 -3.14e100 -3.14e100",
        "-3.14e100 -3.14e100 -3.14e100 -3.14e100",
        "-3.14e100 -3.14e100 -3.14e100 -3.14e100",
        "-3.14e100 -3.14e100 -3.14e100 -3.14e100",
        "甲:-3.14e100,乙:-3.14e100",
        "甲:-3.14e100,乙:0",
        "甲:-3.14e100,乙:-3.14e100",
        "甲:-3.14e100,乙:-3.14e100",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙");
    EXPECT_EQ(to_strings(runes, cut<HMMSegment>(unused_dict(), model, runes)), (std::vector<std::string>{"甲", "乙"}));
}

TEST_F(HMModelTest, UnknownRunesPreserveLegacyStateInHmmMixAndSearch) {
    const auto lines = std::vector<std::string>{
        "0 -100 -100 -100", "-100 0 -100 -100", "0 -100 -100 -100", "-100 -100 -100 -100", "0 -100 -100 -100",
        "甲:0,乙:0",        "甲:0,乙:0",        "甲:0,乙:0",        "甲:0,乙:0",
    };
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto dict_path = directory_ / "main.dict";
    {
        auto output = std::ofstream{dict_path};
        ASSERT_TRUE(output.is_open());
        output << "主词 1 n\n";
        output.close();
        ASSERT_TRUE(output.good());
    }
    const auto dict = DictTrie{dict_path.string(), "", UserWordWeightOption::WordWeightMedian};
    const auto model = HMModel{model_path()};
    const auto runes = decode("甲乙𠀀甲乙，外甲乙A12");
    const auto words = cut<HMMSegment>(unused_dict(), model, runes);
    const auto expected =
        std::vector<WordRange>{{0, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 12}};
    EXPECT_EQ(to_strings(runes, words),
              (std::vector<std::string>{"甲乙", "𠀀", "甲", "乙", "，", "外", "甲", "乙", "A12"}));
    EXPECT_EQ(words, expected);
    EXPECT_EQ(cut<MixSegment<true>>(dict, model, runes), expected);
    EXPECT_EQ(cut<QuerySegment<true>>(dict, model, runes), expected);
}

TEST_F(HMModelTest, HmmSegmentationPreservesOffsetsAtWordRangeLimit) {
    const auto model = HMModel{model_path()};
    const auto runes = decode("AB，𠀀");
    constexpr auto limit = std::numeric_limits<RuneIndex>::max();
    auto words = std::vector<WordRange>{};
    auto scratch = detail::SegmentScratch{};
    detail::hmm_cut_append(model, runes, limit - 4, words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{limit - 4, limit - 2}, {limit - 2, limit - 1}, {limit - 1, limit}}));
}

TEST_F(HMModelTest, ReusedViterbiRowsKeepCurrentBoundariesAfterGrowth) {
    auto lines = model_lines_;
    for (auto i = size_t{0}; i < lines.size(); ++i) {
        lines[i] = i < 5 ? "0 0 0 0" : "甲:0,𠮷:0";
    }
    ASSERT_NO_FATAL_FAILURE(write_model(lines));
    const auto model = HMModel{model_path()};
    auto scratch = detail::SegmentScratch{};
    auto words = std::vector<WordRange>{};
    const auto long_run = std::u32string(64, U'甲');
    const auto longer_run = std::u32string(256, U'𠮷');
    const auto inputs = std::array<std::u32string_view, 6>{long_run, U"甲𠮷", U"", longer_run, U"𠮷", long_run};

    // Equal scores select B predecessors and a final E, forming one word over the active input.
    for (const auto input : inputs) {
        HMMSegment::cut_into(unused_dict(), model, std::span<const Rune>{input.data(), input.size()}, words, scratch);
        if (input.empty()) {
            EXPECT_TRUE(words.empty());
        } else {
            EXPECT_EQ(words, (std::vector<WordRange>{{0, static_cast<RuneIndex>(input.size())}}));
        }
    }

    // Missing emissions must overwrite those predecessors with the legacy E fallback.
    constexpr auto missing = std::u32string_view{U"乙😀"};
    HMMSegment::cut_into(unused_dict(), model, std::span<const Rune>{missing.data(), missing.size()}, words, scratch);
    EXPECT_EQ(words, (std::vector<WordRange>{{0, 1}, {1, 2}}));
}

TEST(HMMSegmentTest, EmptyInput) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = Unicode{};
    auto result = cut<HMMSegment>(unused_dict(), model, std::span<const Rune>{runes});
    EXPECT_TRUE(result.empty());
}

TEST(HMMSegmentTest, ChineseWithPunctuation) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string{"我来自北京邮电大学。。。学号123456"};
    auto runes = decode(sentence);
    auto result = cut<HMMSegment>(unused_dict(), model, runes);
    auto words = to_strings(runes, result);
    auto expected = std::vector<std::string>{"我来", "自北京", "邮电大学", "。", "。", "。", "学号", "123456"};
    EXPECT_EQ(words, expected) << "got: " << join(words);
}

TEST(HMMSegmentTest, ASCIIWithCommas) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string{"IBM,1.2,123"};
    auto runes = decode(sentence);
    auto result = cut<HMMSegment>(unused_dict(), model, runes);
    auto words = to_strings(runes, result);
    auto expected = std::vector<std::string>{"IBM", ",", "1.2", ",", "123"};
    EXPECT_EQ(words, expected) << "got: " << join(words);
}

TEST(HMMSegmentTest, PureChinese) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string{"我来自北京邮电大学"};
    auto runes = decode(sentence);
    auto ranges = cut<HMMSegment>(unused_dict(), model, runes);
    auto words = to_strings(std::span<const Rune>{runes}, ranges);
    EXPECT_FALSE(words.empty());
    auto reconstructed = std::string{};
    for (auto &&w : words) {
        reconstructed += w;
    }
    EXPECT_EQ(reconstructed, sentence);
}

TEST(HMMSegmentTest, SingleCharacter) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto runes = decode(std::string_view{"我"});
    auto result = cut<HMMSegment>(unused_dict(), model, runes);
    auto words = to_strings(runes, result);
    ASSERT_EQ(words.size(), 1U);
    EXPECT_EQ(words[0], "我");
}

TEST(HMMSegmentTest, PureASCII) {
    auto model = HMModel{HMM_MODEL_FILE};
    auto sentence = std::string{"Hello123World"};
    auto runes = decode(sentence);
    auto result = cut<HMMSegment>(unused_dict(), model, runes);
    auto words = to_strings(runes, result);
    EXPECT_FALSE(words.empty());
    auto reconstructed = std::string{};
    for (auto &&w : words) {
        reconstructed += w;
    }
    EXPECT_EQ(reconstructed, sentence);
}

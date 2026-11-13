#pragma once

#include "neo/util/Logging.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace neo_cppjieba::detail {

// HMMState represents the hidden states in the HMM model for Chinese word segmentation.
enum class HMMState : uint8_t {
    B = 0, // Begin
    E = 1, // End
    M = 2, // Middle
    S = 3  // Single
};
inline constexpr auto kHMMStatesNum = size_t{4};
/// Emission probabilities for one rune in B/E/M/S order.
using EmitProbabilities = std::array<double, kHMMStatesNum>;
inline constexpr auto kHMMStateLables = std::array<char, kHMMStatesNum>{'B', 'E', 'M', 'S'};

constexpr
auto get_hmm_state_label(HMMState state) noexcept -> char {
    assert_check([=] { return static_cast<size_t>(state) < kHMMStatesNum; }, "HMModel: invalid label state");
    return kHMMStateLables[static_cast<size_t>(state)];
}

} // namespace neo_cppjieba::detail

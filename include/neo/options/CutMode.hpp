#pragma once

#include "neo/Fwd.hpp"
#include "neo/util/Logging.hpp"

#include <cstdint>
#include <utility>

namespace neo_cppjieba {

// Each mode names a complete behavior, including the modes that disable HMM.
enum class CutMode : uint8_t { MIX, MIX_NO_HMM, FULL, SEARCH, SEARCH_NO_HMM, HMM, MP };

namespace detail {

// Public entry points accept only named segmentation modes.
constexpr
auto valid_cut_mode(CutMode mode) noexcept -> bool {
    switch (mode) {
        case CutMode::MIX:
        case CutMode::MIX_NO_HMM:
        case CutMode::FULL:
        case CutMode::SEARCH:
        case CutMode::SEARCH_NO_HMM:
        case CutMode::HMM:
        case CutMode::MP:
            return true;
    }
    return false;
}

// Mode values supplied by callers are configuration errors, not internal invariants.
inline
auto check_cut_mode(CutMode mode) -> void {
    check(valid_cut_mode(mode), "Unknown Jieba cut mode {}", std::to_underlying(mode));
}

} // namespace detail

} // namespace neo_cppjieba

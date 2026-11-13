#pragma once

#include "neo/util/Logging.hpp"

#include <cstdint>

namespace neo_cppjieba::detail {

// Selects the weight of user dictionary and runtime words that give no frequency: the main dictionary's
// minimum, median or maximum word weight. DictTrie bakes the choice into its data at construction.
enum class UserWordWeightOption : uint8_t { WordWeightMin, WordWeightMedian, WordWeightMax };

// Option values supplied by callers are configuration errors, not internal invariants.
inline
auto check_user_word_weight_option(UserWordWeightOption option) -> void {
    check(option == UserWordWeightOption::WordWeightMin || option == UserWordWeightOption::WordWeightMedian
              || option == UserWordWeightOption::WordWeightMax,
          "DictTrie: invalid user word weight option {}", static_cast<uint8_t>(option));
}

} // namespace neo_cppjieba::detail

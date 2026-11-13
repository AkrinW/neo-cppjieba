#pragma once

/// Shared resources for segmentation policy tests. Every policy takes a dictionary and a model;
/// tests of a policy that ignores one of them borrow these instances, loaded once per process.

#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"

#include "test_paths.h"

// The bundled HMM model, for policies that only read the dictionary.
inline
auto unused_model() -> const neo_cppjieba::detail::HMModel & {
    static const auto model = neo_cppjieba::detail::HMModel{DICT_DIR "/hmm_model.utf8"};
    return model;
}

// A one-word dictionary, for policies that only read the HMM model.
inline
auto unused_dict() -> const neo_cppjieba::detail::DictTrie & {
    static const auto dict =
        neo_cppjieba::detail::DictTrie{neo_cppjieba::DictSource::memory("的 1 uj", "unused"),
                                       {},
                                       neo_cppjieba::detail::UserWordWeightOption::WordWeightMedian};
    return dict;
}

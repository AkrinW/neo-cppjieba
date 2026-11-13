#include "cppjieba/HMMSegment.hpp"
#include "cppjieba/KeywordExtractor.hpp"
#include "cppjieba/MPSegment.hpp"
#include "cppjieba/MixSegment.hpp"
#include "limonp/Colors.hpp"

#include "test_paths.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace cppjieba;
namespace {
static
void Cut(size_t times = 50) {
    MixSegment seg(DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8");
    vector<string> res;
    string doc;
    ifstream ifs(TEST_DATA_DIR "/weicheng.utf8");
    assert(ifs);
    doc << ifs;
    long beginTime = clock();
    for (size_t i = 0; i < times; i++) {
        printf("process [%3.0lf %%]\r", 100.0 * (i + 1) / times);
        fflush(stdout);
        res.clear();
        seg.Cut(doc, res);
    }
    printf("\n");
    long endTime = clock();
    limonp::ColorPrintln(limonp::GREEN, "Cut: [%.3lf seconds]time consumed.",
                         static_cast<double>(endTime - beginTime) / CLOCKS_PER_SEC);
}

static
void Extract(size_t times = 400) {
    KeywordExtractor Extractor(DICT_DIR "/jieba.dict.utf8", DICT_DIR "/hmm_model.utf8", DICT_DIR "/idf.utf8",
                               DICT_DIR "/stop_words.utf8");
    vector<string> words;
    string doc;
    ifstream ifs(TEST_DATA_DIR "/review.100");
    assert(ifs);
    doc << ifs;
    long beginTime = clock();
    for (size_t i = 0; i < times; i++) {
        printf("process [%3.0lf %%]\r", 100.0 * (i + 1) / times);
        fflush(stdout);
        words.clear();
        Extractor.Extract(doc, words, 5);
    }
    printf("\n");
    long endTime = clock();
    limonp::ColorPrintln(limonp::GREEN, "Extract: [%.3lf seconds]time consumed.",
                         static_cast<double>(endTime - beginTime) / CLOCKS_PER_SEC);
}
} // namespace

auto main(int /*argc*/, char ** /*argv*/) -> int {
    Cut();
    Extract();
    return EXIT_SUCCESS;
}

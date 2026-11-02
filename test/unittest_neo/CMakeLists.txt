add_executable(test_neo.run
    ../gtest_main.cpp
    unicode_test.cpp
    char_class_test.cpp
    traits_test.cpp
    fwd_test.cpp
    concept_test.cpp
    logging_test.cpp
    fileio_test.cpp
    benchmark_utils_test.cpp
    postag_test.cpp
    trie_stats_test.cpp
    trie_update_test.cpp
    dicttrie_test.cpp
    segment_api_test.cpp
    hmmseg_test.cpp
    fullseg_test.cpp
    mpseg_test.cpp
    mixseg_test.cpp
    queryseg_test.cpp
    jieba_test.cpp
    tagging_test.cpp
    keyword_extractor_test.cpp
    dict_loading_test.cpp
    dict_snapshot_test.cpp
    concurrency_test.cpp
)

target_compile_definitions(test_neo.run PRIVATE LOGGING_LEVEL=LL_WARNING)
# Add include directories
find_package(Threads REQUIRED)
target_link_libraries(test_neo.run PRIVATE neo_cppjieba::neo_cppjieba cppjieba_test_support Threads::Threads)
if(CPPJIEBA_ENABLE_ICU)
    target_sources(test_neo.run PRIVATE encoded_text_test.cpp)
    target_link_libraries(test_neo.run PRIVATE neo_cppjieba::icu)
endif()
add_test(NAME neo_unit_tests COMMAND test_neo.run)

if(CPPJIEBA_BUILD_C_API)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CApiTests.cmake")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ConfigVariantTests.cmake")

# libFuzzer ships with Clang; AppleClang omits it.
if(CPPJIEBA_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "CPPJIEBA_BUILD_FUZZERS requires Clang with libFuzzer")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz")

# Compile the adapter into the fuzzer so it is instrumented without building the C ABI libraries.
add_library(cppjieba_fuzz_c_api INTERFACE)
target_sources(cppjieba_fuzz_c_api INTERFACE "${PROJECT_SOURCE_DIR}/api/src/neo_cppjieba.cpp")
target_include_directories(cppjieba_fuzz_c_api INTERFACE "${PROJECT_SOURCE_DIR}/api/include")
target_compile_definitions(cppjieba_fuzz_c_api INTERFACE NEO_CPPJIEBA_C_STATIC)
target_link_libraries(cppjieba_fuzz_c_api INTERFACE cppjieba_test_data)

# Build one harness as <name>_fuzzer for libFuzzer and, under BUILD_TESTING, as a test that replays
# corpus/<name> with any compiler, so sanitizer and coverage builds run the checked-in inputs too.
function(cppjieba_add_fuzzer name)
    cmake_parse_arguments(PARSE_ARGV 1 FUZZ "" "" "SOURCES;LIBRARIES")
    if(CPPJIEBA_BUILD_FUZZERS)
        # Every target runs with ASan and UBSan; build without NDEBUG to keep library assertions fatal.
        set(sanitizers -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
        add_executable(${name}_fuzzer ${FUZZ_SOURCES})
        target_compile_options(${name}_fuzzer PRIVATE ${sanitizers})
        target_link_options(${name}_fuzzer PRIVATE ${sanitizers})
        target_link_libraries(${name}_fuzzer PRIVATE neo_cppjieba::neo_cppjieba ${FUZZ_LIBRARIES})
    endif()
    if(BUILD_TESTING)
        add_executable(test_fuzz_${name}.run replay_main.cpp ${FUZZ_SOURCES})
        set_target_properties(test_fuzz_${name}.run PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test")
        target_link_libraries(test_fuzz_${name}.run PRIVATE neo_cppjieba::neo_cppjieba ${FUZZ_LIBRARIES})
        add_test(NAME neo_fuzz_replay_${name}
            COMMAND test_fuzz_${name}.run "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
    endif()
endfunction()

cppjieba_add_fuzzer(utf_decode SOURCES utf_decode_fuzzer.cpp)
cppjieba_add_fuzzer(main_dict SOURCES main_dict_fuzzer.cpp)
cppjieba_add_fuzzer(user_dict SOURCES user_dict_fuzzer.cpp)
cppjieba_add_fuzzer(hmm_model SOURCES hmm_model_fuzzer.cpp)
cppjieba_add_fuzzer(dict_snapshot SOURCES dict_snapshot_fuzzer.cpp)
cppjieba_add_fuzzer(snapshot_roundtrip SOURCES snapshot_roundtrip_fuzzer.cpp)
cppjieba_add_fuzzer(keyword SOURCES keyword_fuzzer.cpp)
cppjieba_add_fuzzer(utf_cut SOURCES utf_cut_fuzzer.cpp LIBRARIES cppjieba_test_data)
cppjieba_add_fuzzer(c_api_cut SOURCES c_api_cut_fuzzer.cpp LIBRARIES cppjieba_fuzz_c_api)
cppjieba_add_fuzzer(c_api_ranges SOURCES c_api_ranges_fuzzer.cpp LIBRARIES cppjieba_fuzz_c_api)
cppjieba_add_fuzzer(c_api_sources SOURCES c_api_sources_fuzzer.cpp LIBRARIES cppjieba_fuzz_c_api)
cppjieba_add_fuzzer(c_api_edit SOURCES c_api_edit_fuzzer.cpp LIBRARIES cppjieba_fuzz_c_api)
cppjieba_add_fuzzer(c_api_keyword SOURCES c_api_keyword_fuzzer.cpp LIBRARIES cppjieba_fuzz_c_api)

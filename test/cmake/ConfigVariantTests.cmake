# Included in unittest_neo's directory scope after test_neo.run is defined.

# Each capacity variant is a separate executable so inline definitions never mix configurations.
set(capacity_config_path "${PROJECT_SOURCE_DIR}/include/neo/Config.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${capacity_config_path}")
file(READ "${capacity_config_path}" capacity_config_source)

function(cppjieba_add_capacity_test name rune_type source_type dag_type node_type test_source)
    set(config "${capacity_config_source}")
    # Capacity and existing regression fixtures retain the CPP output contract.
    string(REGEX REPLACE "segmentation_style = SegmentationStyle::(CPP|RUST|PYTHON);"
        "segmentation_style = SegmentationStyle::CPP;" config "${config}")
    string(REGEX REPLACE "using RuneIndex = [^;]+;" "using RuneIndex = ${rune_type};" config "${config}")
    string(REGEX REPLACE "using SourceOffset = [^;]+;" "using SourceOffset = ${source_type};" config "${config}")
    string(REGEX REPLACE "using DagOffset = [^;]+;" "using DagOffset = ${dag_type};" config "${config}")
    string(REGEX REPLACE "using TrieNodeId = [^;]+;" "using TrieNodeId = ${node_type};" config "${config}")
    set(config_dir "${PROJECT_BINARY_DIR}/generated/capacity/${name}")
    # Substituting the complete header once preserves embedded variable syntax.
    # file(CONFIGURE) updates the timestamp only when the header content changes.
    file(CONFIGURE OUTPUT "${config_dir}/neo/Config.hpp" CONTENT "@config@" @ONLY)

    set(target "test_capacity_${name}.run")
    add_executable(${target} ../gtest_main.cpp ${test_source})
    target_include_directories(${target} BEFORE PRIVATE "${config_dir}")
    target_link_libraries(${target} PRIVATE neo_cppjieba::neo_cppjieba cppjieba_test_support)
    add_test(NAME neo_capacity_${name} COMMAND ${target})
endfunction()

# Each style has its own executable and generated header, avoiding mixed inline definitions.
foreach(style IN ITEMS CPP RUST PYTHON)
    string(TOLOWER "${style}" style_name)
    set(config "${capacity_config_source}")
    string(REGEX REPLACE "segmentation_style = SegmentationStyle::(CPP|RUST|PYTHON);"
        "segmentation_style = SegmentationStyle::${style};" config "${config}")
    set(config_dir "${PROJECT_BINARY_DIR}/generated/style/${style_name}")
    file(CONFIGURE OUTPUT "${config_dir}/neo/Config.hpp" CONTENT "@config@" @ONLY)
    set(target "test_style_${style_name}.run")
    add_executable(${target} ../gtest_main.cpp segmentation_style_test.cpp)
    target_include_directories(${target} BEFORE PRIVATE "${config_dir}")
    target_link_libraries(${target} PRIVATE neo_cppjieba::neo_cppjieba cppjieba_test_support)
    add_test(NAME neo_style_${style_name} COMMAND ${target})
endforeach()

foreach(bits IN ITEMS 8 16 32 64)
    cppjieba_add_capacity_test(${bits} "std::uint${bits}_t" "std::uint${bits}_t" "std::uint${bits}_t"
        "std::uint32_t" capacity_test.cpp)
    cppjieba_add_capacity_test(trie_${bits} "std::uint32_t" "std::uint32_t" "std::uint32_t"
        "std::uint${bits}_t" trie_capacity_test.cpp)
endforeach()
# Existing regression fixtures require more capacity than the narrow variants provide.
target_include_directories(test_neo.run BEFORE PRIVATE "${PROJECT_BINARY_DIR}/generated/capacity/32")
cppjieba_add_capacity_test(mixed "std::uint8_t" "std::uint16_t" "std::uint32_t" "std::uint64_t" capacity_test.cpp)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("int main() { unsigned __int128 value = 0; return static_cast<int>(value); }"
    NEO_TEST_HAS_UINT128)
if(NEO_TEST_HAS_UINT128)
    cppjieba_add_capacity_test(128 "uint128_t" "uint128_t" "uint128_t" "uint128_t" capacity_test.cpp)
    cppjieba_add_capacity_test(mixed128 "uint128_t" "std::uint8_t" "std::uint16_t" "std::uint64_t" capacity_test.cpp)
    cppjieba_add_capacity_test(trie_128 "std::uint32_t" "std::uint32_t" "std::uint32_t"
        "uint128_t" trie_capacity_test.cpp)
    # Clang reports every subscript with a 128-bit RuneIndex as a conversion to size_t; the index never exceeds
    # the size of the span it subscripts, so these variants would otherwise warn on each rune access.
    # detail::to_index makes those conversions explicit, so the variants keep every conversion warning.
endif()

# Generate the level a test executable's build passes to the headers, for SimdTest to compare against.
function(cppjieba_add_simd_expectation target level)
    set(expectation_dir "${PROJECT_BINARY_DIR}/generated/simd/${target}")
    string(CONCAT expectation
        "#pragma once\n\n#include \"neo/simd/SimdLevel.hpp\"\n\n"
        "inline constexpr auto expected_simd_level = neo_cppjieba::detail::simd::SimdLevel::${level};\n")
    file(CONFIGURE OUTPUT "${expectation_dir}/SimdExpectation.hpp" CONTENT "${expectation}" @ONLY)
    target_include_directories(${target} PRIVATE "${expectation_dir}")
endfunction()

cppjieba_add_simd_expectation(test_neo.run "${CPPJIEBA_SIMD}")

# Each level this host can execute gets its own executable, so the default OFF build still tests every level.
# Variants need an OFF core target, since the headers reject a second CPPJIEBA_SIMD_<LEVEL> definition.
if(CPPJIEBA_SIMD STREQUAL "OFF")
    foreach(level IN ITEMS SSE2 AVX2 NEON)
        cppjieba_simd_probe(${level} RUNS simd_runs)
        if(NOT simd_runs)
            continue()
        endif()
        string(TOLOWER "${level}" level_name)
        set(target "test_simd_${level_name}.run")
        add_executable(${target} ../gtest_main.cpp simd_test.cpp)
        cppjieba_simd_options(${level} simd_options)
        target_compile_definitions(${target} PRIVATE "CPPJIEBA_SIMD_${level}")
        target_compile_options(${target} PRIVATE ${simd_options})
        cppjieba_add_simd_expectation(${target} ${level})
        target_link_libraries(${target} PRIVATE neo_cppjieba::neo_cppjieba cppjieba_test_support)
        add_test(NAME neo_simd_${level_name} COMMAND ${target})
    endforeach()
endif()

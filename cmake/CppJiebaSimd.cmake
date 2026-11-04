# Turn CPPJIEBA_SIMD into the CPPJIEBA_SIMD_<LEVEL> definition and compile options that neo_cppjieba carries.
# The level is part of the package configuration: every translation unit of a program must use the same one.
include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)
include(CMakePushCheckState)

set(cppjieba_simd_levels OFF SSE2 AVX2 NEON)

# Set out_var to the compile options that enable level; SSE2 and NEON are baselines of x86-64 and AArch64.
function(cppjieba_simd_options level out_var)
    set(options)
    if(level STREQUAL "AVX2")
        if(MSVC)
            set(options /arch:AVX2)
        else()
            set(options -mavx2)
        endif()
    endif()
    set(${out_var} "${options}" PARENT_SCOPE)
endfunction()

# Set out_var to whether the active compiler and target build level (mode COMPILES) or also execute it (mode RUNS).
# The probe includes neo/simd/SimdLevel.hpp, so it fails exactly where the header would reject the level.
function(cppjieba_simd_probe level mode out_var)
    if(level STREQUAL "NEON")
        set(body [=[
#include <arm_neon.h>
int main() {
    volatile unsigned seed = 1;
    const uint32x4_t v = vdupq_n_u32(seed);
    return static_cast<int>(vmaxvq_u32(vceqq_u32(vaddq_u32(v, v), v)));
}
]=])
    elseif(level STREQUAL "AVX2")
        set(body [=[
#include <immintrin.h>
int main() {
    volatile int seed = 1;
    const __m256i v = _mm256_set1_epi32(seed);
    return _mm256_movemask_epi8(_mm256_cmpeq_epi32(_mm256_add_epi32(v, v), v));
}
]=])
    elseif(level STREQUAL "SSE2")
        set(body [=[
#include <emmintrin.h>
int main() {
    volatile int seed = 1;
    const __m128i v = _mm_set1_epi32(seed);
    return _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_add_epi32(v, v), v));
}
]=])
    else()
        message(FATAL_ERROR "cppjieba_simd_probe: no probe for SIMD level '${level}'")
    endif()

    cppjieba_simd_options(${level} options)
    cmake_push_check_state(RESET)
    list(JOIN options " " CMAKE_REQUIRED_FLAGS)
    set(CMAKE_REQUIRED_DEFINITIONS "-DCPPJIEBA_SIMD_${level}")
    set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}/include")
    set(source "#include \"neo/simd/SimdLevel.hpp\"\n${body}")
    if(mode STREQUAL "RUNS")
        check_cxx_source_runs("${source}" CPPJIEBA_SIMD_${level}_RUNS)
        set(result "${CPPJIEBA_SIMD_${level}_RUNS}")
    elseif(mode STREQUAL "COMPILES")
        check_cxx_source_compiles("${source}" CPPJIEBA_SIMD_${level}_COMPILES)
        set(result "${CPPJIEBA_SIMD_${level}_COMPILES}")
    else()
        message(FATAL_ERROR "cppjieba_simd_probe: mode must be COMPILES or RUNS, not '${mode}'")
    endif()
    cmake_pop_check_state()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

if(NOT CPPJIEBA_SIMD IN_LIST cppjieba_simd_levels)
    list(JOIN cppjieba_simd_levels ", " cppjieba_simd_names)
    message(FATAL_ERROR "CPPJIEBA_SIMD=${CPPJIEBA_SIMD} is not a SIMD level; use one of ${cppjieba_simd_names}")
endif()

if(NOT CPPJIEBA_SIMD STREQUAL "OFF")
    list(LENGTH CMAKE_OSX_ARCHITECTURES cppjieba_simd_arch_count)
    if(cppjieba_simd_arch_count GREATER 1)
        message(FATAL_ERROR "CPPJIEBA_SIMD=${CPPJIEBA_SIMD} selects one instruction set; "
            "build each of CMAKE_OSX_ARCHITECTURES (${CMAKE_OSX_ARCHITECTURES}) separately")
    endif()
    # Only compilation is checked, so cross builds work; the build machine's CPU is never consulted.
    cppjieba_simd_probe(${CPPJIEBA_SIMD} COMPILES cppjieba_simd_supported)
    if(NOT cppjieba_simd_supported)
        message(FATAL_ERROR "CPPJIEBA_SIMD=${CPPJIEBA_SIMD} is not supported by this compiler and target: "
            "SSE2 and AVX2 need x86-64, NEON needs AArch64 with GCC or Clang")
    endif()
    cppjieba_simd_options(${CPPJIEBA_SIMD} cppjieba_simd_compile_options)
    target_compile_definitions(neo_cppjieba INTERFACE "CPPJIEBA_SIMD_${CPPJIEBA_SIMD}")
    target_compile_options(neo_cppjieba INTERFACE ${cppjieba_simd_compile_options})
endif()
message(STATUS "SIMD level: ${CPPJIEBA_SIMD}")

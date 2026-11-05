#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

// The only header that maps the CPPJIEBA_SIMD_<LEVEL> definition, set by the CPPJIEBA_SIMD CMake option, to a level.
// Platform macros and intrinsics stay under neo/simd/; code outside it calls the kernels and never tests them.
// A level also compiles the lower levels of its instruction-set family and the scalar backend: callers pass `level`
// to use the highest, and tests compare every compiled backend with the scalar one. Backends no caller instantiates
// emit no code.

namespace neo_cppjieba::detail::simd {

/// Instruction-set level chosen at build time. x86 levels are ordered: AVX2 includes SSE2.
enum class SimdLevel : std::uint8_t { OFF, SSE2, AVX2, NEON };

#if defined(CPPJIEBA_SIMD_SSE2) + defined(CPPJIEBA_SIMD_AVX2) + defined(CPPJIEBA_SIMD_NEON) > 1
#error "Define at most one CPPJIEBA_SIMD_<LEVEL>; the CPPJIEBA_SIMD CMake option sets it"
#endif

#if defined(CPPJIEBA_SIMD_AVX2)
#if !(defined(__x86_64__) || (defined(_M_X64) && !defined(_M_ARM64EC))) || !defined(__AVX2__)
#error "CPPJIEBA_SIMD_AVX2 requires an x86-64 target compiled with -mavx2 or /arch:AVX2"
#endif
inline constexpr auto level = SimdLevel::AVX2;
inline constexpr auto compiled_backends = std::array{SimdLevel::OFF, SimdLevel::SSE2, SimdLevel::AVX2};
#elif defined(CPPJIEBA_SIMD_SSE2)
#if !(defined(__x86_64__) || (defined(_M_X64) && !defined(_M_ARM64EC)))
#error "CPPJIEBA_SIMD_SSE2 requires an x86-64 target"
#endif
inline constexpr auto level = SimdLevel::SSE2;
inline constexpr auto compiled_backends = std::array{SimdLevel::OFF, SimdLevel::SSE2};
#elif defined(CPPJIEBA_SIMD_NEON)
#if !((defined(__aarch64__) && defined(__ARM_NEON)) || (defined(_M_ARM64) && !defined(_M_ARM64EC)))
#error "CPPJIEBA_SIMD_NEON requires an AArch64 target with NEON"
#endif
inline constexpr auto level = SimdLevel::NEON;
inline constexpr auto compiled_backends = std::array{SimdLevel::OFF, SimdLevel::NEON};
#else
inline constexpr auto level = SimdLevel::OFF;
inline constexpr auto compiled_backends = std::array{SimdLevel::OFF};
#endif

static_assert(compiled_backends.front() == SimdLevel::OFF && compiled_backends.back() == level,
              "compiled_backends must run from the scalar backend up to the build level");

/// Whether this build compiles backend; the kernel entries reject any other.
[[nodiscard]]
constexpr auto is_compiled_backend(SimdLevel backend) noexcept -> bool {
    return std::ranges::contains(compiled_backends, backend);
}

/// The level's name as the CPPJIEBA_SIMD option spells it.
[[nodiscard]]
constexpr auto level_name(SimdLevel value) noexcept -> std::string_view {
    switch (value) {
        case SimdLevel::OFF:
            return "OFF";
        case SimdLevel::SSE2:
            return "SSE2";
        case SimdLevel::AVX2:
            return "AVX2";
        case SimdLevel::NEON:
            return "NEON";
    }
    std::unreachable();
}

/// Call visit.template operator()<backend>() for each of compiled_backends, scalar first.
template <typename Visit>
constexpr auto for_each_compiled_backend(const Visit &visit) -> void {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (visit.template operator()<compiled_backends[I]>(), ...);
    }(std::make_index_sequence<compiled_backends.size()>{});
}

} // namespace neo_cppjieba::detail::simd

#pragma once

#include <cstdint>

// The only header that maps the CPPJIEBA_SIMD_<LEVEL> definition, set by the CPPJIEBA_SIMD CMake option, to a level.
// Platform macros and intrinsics stay under neo/simd/; code outside it calls the kernels and never tests them.

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
#elif defined(CPPJIEBA_SIMD_SSE2)
#if !(defined(__x86_64__) || (defined(_M_X64) && !defined(_M_ARM64EC)))
#error "CPPJIEBA_SIMD_SSE2 requires an x86-64 target"
#endif
inline constexpr auto level = SimdLevel::SSE2;
#elif defined(CPPJIEBA_SIMD_NEON)
#if !((defined(__aarch64__) && defined(__ARM_NEON)) || (defined(_M_ARM64) && !defined(_M_ARM64EC)))
#error "CPPJIEBA_SIMD_NEON requires an AArch64 target with NEON"
#endif
inline constexpr auto level = SimdLevel::NEON;
#else
inline constexpr auto level = SimdLevel::OFF;
#endif

} // namespace neo_cppjieba::detail::simd

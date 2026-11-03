#pragma once

// NEO_FORCE_INLINE forces inlining of a measured per-rune helper, which GCC otherwise leaves out of line once a large
// translation unit spends its inline-unit-growth budget. It names an attribute for an attribute list, as in
// [[nodiscard, NEO_FORCE_INLINE]]; test/cmake/CheckInlining.cmake lists every helper that relies on it.
#if defined(__GNUC__) || defined(__clang__)
#define NEO_FORCE_INLINE gnu::always_inline
#elif defined(_MSC_VER)
#define NEO_FORCE_INLINE msvc::forceinline
#else
#define NEO_FORCE_INLINE
#endif

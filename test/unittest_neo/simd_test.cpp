#include "gtest/gtest.h"
#include "neo/simd/SimdLevel.hpp"

#include "SimdExpectation.hpp"

using namespace neo_cppjieba::detail::simd;

// The CPPJIEBA_SIMD option, or a variant executable's own definition, reaches the headers as the same level.
TEST(SimdTest, LevelMatchesBuildOption) {
    EXPECT_EQ(level, expected_simd_level);
}

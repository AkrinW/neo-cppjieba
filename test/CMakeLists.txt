include("${PROJECT_SOURCE_DIR}/deps/googletest.cmake")

# C++ test dependencies stay separate from the pure C ABI smoke targets.
add_library(cppjieba_test_support INTERFACE)
target_link_libraries(cppjieba_test_support INTERFACE cppjieba_test_data GTest::gtest)
if(MSVC)
    target_compile_options(cppjieba_test_support INTERFACE /utf-8)
endif()

# Set the output directory after GoogleTest so it applies only to our test targets.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test")

add_subdirectory(unittest_neo)
if(CPPJIEBA_BUILD_LEGACY_TESTS)
    add_subdirectory(unittest)
endif()

include(cmake/HeaderTests.cmake)
include(cmake/InlineTests.cmake)
include(cmake/PackageTests.cmake)

foreach(provider IN ITEMS parent installed source)
    add_test(NAME neo_googletest_${provider} COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gtest_discovery"
        -B "${PROJECT_BINARY_DIR}/googletest-discovery/${provider}"
        -G "${CMAKE_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCPPJIEBA_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DCPPJIEBA_TEST_GTEST_PROVIDER=${provider}")
endforeach()

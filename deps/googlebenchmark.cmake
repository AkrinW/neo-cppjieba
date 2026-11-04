if(TARGET benchmark::benchmark)
    return()
endif()

# 1.8 added the RegisterBenchmark, SkipWithError and AddCustomContext signatures the benchmarks use.
find_package(benchmark 1.8 QUIET)
if(benchmark_FOUND)
    return()
endif()

# Build only the library: its tests would fetch GoogleTest into benchmark builds, and its -Werror default turns
# warnings that newer compilers add into build failures.
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_WERROR OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_INSTALL_DOCS OFF)
set(BENCHMARK_ENABLE_LIBPFM OFF)

include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.10.0
)
FetchContent_MakeAvailable(googlebenchmark)

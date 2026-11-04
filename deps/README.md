# Dependencies

[Build options](../docs/building.md#构建选项) · [Benchmarks](../benchmark/README.md)

This directory configures test and comparison dependencies. GoogleTest belongs to
`BUILD_TESTING`; the remaining dependencies serve legacy tests and benchmarks.
The core C++ target uses the standard library and headers shipped under `include/`.

## GoogleTest

`googletest.cmake` resolves the test dependency in this order:

1. Reuse the parent project's `GTest::gtest` target.
2. Locate an installed package or libraries with `find_package(GTest QUIET)`.
3. Download and build upstream `v1.18.0` through `FetchContent`.

Set `GTest_DIR` to the directory containing `GTestConfig.cmake`, or add its
installation prefix to `CMAKE_PREFIX_PATH`. Match the active compiler, standard
library and runtime configuration when using prebuilt libraries.

For an offline source build:

```sh
cmake -S . -B build-tests -DBUILD_TESTING=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
```

To select source discovery over installed packages, also pass
`-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON`. An existing parent target takes precedence.
FetchContent manages this optional source dependency in the build tree; the
submodules below pin comparison implementations.

## Google Benchmark

`googlebenchmark.cmake` resolves the benchmark framework for `CPPJIEBA_BUILD_BENCHMARKS`
in the same order: a parent `benchmark::benchmark` target, an installed package of
version 1.8 or later through `find_package(benchmark 1.8 QUIET)`, then upstream
`v1.10.0` through `FetchContent`. The source build disables the framework's own tests,
so benchmark builds never fetch GoogleTest, and its `-Werror` default.

```sh
cmake -S . -B build-bench -DCPPJIEBA_BUILD_BENCHMARKS=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK=/path/to/benchmark
```

Set `benchmark_DIR` or `CMAKE_PREFIX_PATH` to use an installed package; pass
`-DCMAKE_DISABLE_FIND_PACKAGE_benchmark=ON` to select the source build.

## Pinned comparison sources

| Dependency | Revision | CMake target |
| --- | --- | --- |
| CppJieba | [8f171de](https://github.com/yanyiwu/cppjieba/tree/8f171de5018e8478ff22ca58caacf579cba809c8) | `cppjieba::cppjieba` |
| limonp | [4065c5f](https://github.com/yanyiwu/limonp/tree/4065c5f6d5a7a7248aacdd34d738fe0351b97c3f) | `limonp::limonp` |
| jieba-rs 0.11.0 | [3a0d75c](https://github.com/messense/jieba-rs/tree/3a0d75cf7455e2006330a5b4e94156da272cd7b9) | `jieba_rs::jieba_rs` |
| Python jieba 0.42.1 | [1e20c89](https://github.com/fxsjy/jieba/tree/1e20c89b66f56c9301b0feed211733ffaa1bd72a) | Python benchmark script |

The checkout's submodule gitlinks and `rust-jieba/Cargo.lock` determine the exact
versions. Initialize only the sources needed by the selected comparison.

## C++ comparisons

```sh
git submodule update --init deps/cppjieba            # legacy tests
git submodule update --init deps/cppjieba deps/limonp # C++ benchmarks
```

`cppjieba.cmake` supplies targets for the pinned upstream headers; the pinned
CppJieba headers do not include limonp. The core library, C ABI, unit tests and
legacy tests do not depend on limonp. It serves only the FileIO comparison and the
legacy load benchmark under `CPPJIEBA_BUILD_BENCHMARKS`, which link `limonp::limonp`
explicitly. Their build and installation are controlled by this repository.

Comparisons pass the shared dictionary, HMM model and user-dictionary paths
explicitly. The upstream facade also offers default paths inside its own checkout.
The earlier upstream guide remains in [cppjieba-upstream-history.md](cppjieba-upstream-history.md).

## Rust comparison

```sh
git submodule update --init deps/jieba-rs
```

`rust-jieba/` contains the benchmark C ABI adapter and C++ wrapper. Cargo uses
local paths for the pinned jieba-rs and jieba-macros crates, and downloads other
locked dependencies during the build. Output stays in the CMake build tree.
See [RUST.md](../benchmark/RUST.md) for output contracts and commands.

## Python comparison

```sh
git submodule update --init deps/python-jieba
```

The benchmark imports this checkout directly and loads the shared dictionary
and HMM model before timing. Its upstream MIT license remains in the submodule.
Python is needed when running this script; see [PYTHON.md](../benchmark/PYTHON.md).

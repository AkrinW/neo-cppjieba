# 构建与安装

[快速开始](../README.md#快速开始) · [C++ 使用指南](usage.md) · [C ABI](../api/README.md)

## 环境

使用 CMake 3.20+ 和支持 C++23 的编译器、标准库。核心库使用 `<format>`，应用示例还使用 `<print>`。
当前 CI 配置覆盖以下工具链及其 Debug / Release 构建：

| 平台 | CI 工具链 |
| --- | --- |
| Linux x64 / ARM64 | GCC 16 |
| macOS ARM64 | Homebrew LLVM 23 / libc++ |
| Windows x64 | Visual Studio 2026 |

编译器选择和平台运行库参数见 [Linux](../.github/workflows/cmake.yml)、
[macOS / Windows](../.github/workflows/platforms.yml) 和 [C API](../.github/workflows/c-api.yml) 的配置。
下列构建命令从仓库根目录执行；应用消费端命令从应用项目根目录执行。

## 源码接入

将仓库放入应用的 `external/neo-cppjieba`，在应用的 CMakeLists.txt 中链接接口目标：

```cmake
add_subdirectory(external/neo-cppjieba)
target_link_libraries(my_app PRIVATE neo_cppjieba::neo_cppjieba)
```

该目标提供头文件路径、C++23 标准和 MSVC 的 `/utf-8` 选项。
`BUILD_TESTING` 沿用父项目设置；仅接入库时，可在应用的配置命令中传入 `-DBUILD_TESTING=OFF`。

## 安装与消费

```sh
cmake -S . -B build-library -DBUILD_TESTING=OFF
cmake --install build-library --prefix install
```

在应用中使用安装后的包：

```cmake
find_package(neo_cppjieba 2.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE neo_cppjieba::neo_cppjieba)
```

配置应用时传入 `-DCMAKE_PREFIX_PATH=/absolute/path/to/install`。默认安装布局如下，
实际目录名遵循 `GNUInstallDirs`：

| 内容 | 安装前缀下的路径 |
| --- | --- |
| C++ 头文件 | `include/neo/` |
| 词典和模型 | `share/neo_cppjieba/dict/` |
| C++ CMake 包 | `lib/cmake/neo_cppjieba/` |
| C API CMake 包（启用时） | `lib/cmake/neo_cppjieba_c/` |

程序部署时需要带上词典和模型，并把实际路径传给分词器。`find_package` 之后，
`neo_cppjieba_DICT_DIR` 给出安装包中的词典目录，可用于测试或复制到部署目录。
手工集成时，把 `include/` 加入头文件搜索路径，并启用 C++23 和 MSVC 的 UTF-8 源码选项。

## 可选组件

配置源码项目时启用 `CPPJIEBA_ENABLE_ICU=ON`，并提供 ICU 的 `uc`、`data` 开发库，
即可链接 `neo_cppjieba::icu`。安装消费端通过 `COMPONENTS icu` 选择该组件：

```cmake
find_package(neo_cppjieba 2.0 CONFIG REQUIRED COMPONENTS icu)
target_link_libraries(my_app PRIVATE neo_cppjieba::icu)
```

核心目标只依赖随库头文件和标准库，选择 `icu` 组件时才查找 ICU。
转换接口及 `ICU_ROOT` 配置见 [编码指南](encoding.md)。

`CPPJIEBA_BUILD_C_API=ON` 增加共享库、静态库和纯 C 头文件。
构建、链接与运行库选择集中在 [C ABI 指南](../api/README.md)。

### SIMD

`CPPJIEBA_SIMD` 选择向量化内核使用的指令集，默认 `OFF`，只用标量代码：

| 取值 | 目标 | 追加的编译选项 |
| --- | --- | --- |
| `OFF` | 任意 | 无 |
| `SSE2` | x86-64 | 无，属于 x86-64 基线 |
| `AVX2` | x86-64 | GCC / Clang 为 `-mavx2`，MSVC 为 `/arch:AVX2` |
| `NEON` | AArch64，GCC 或 Clang | 无，属于 AArch64 基线 |

配置时 CMake 带上所选选项试编译一段使用该指令集的代码，编译器或目标架构不支持时直接报错；
取值不在表中、或 `CMAKE_OSX_ARCHITECTURES` 含多个架构时同样报错。只检查能否编译，不检查构建机的 CPU，交叉编译照常可用。
`CPPJIEBA_SIMD_<级别>` 宏和编译选项随 `neo_cppjieba::neo_cppjieba` 传给使用方，安装包保留构建时的级别。

- 级别属于整个程序的配置：同一程序的所有翻译单元须使用相同级别，否则同名内联函数会有不同定义。
- `AVX2` 的编译选项作用于使用方的整个目标，生成的程序只能在支持 AVX2 的 CPU 上运行，库不做运行时检测。
  C ABI 库同样随所选级别编译，分发给他人的二进制宜保持 `OFF` 或基线级别。
- 不经 CMake 集成时，自行定义 `CPPJIEBA_SIMD_SSE2`、`CPPJIEBA_SIMD_AVX2`、`CPPJIEBA_SIMD_NEON` 之一并加上对应编译选项；
  宏与目标架构或编译选项不符时，`neo/simd/SimdLevel.hpp` 在编译期报错。
- 按级别切换实现的内核有分隔符扫描和 HMM Viterbi（CPP 与 Python/Rust 两种规则各一个）：`SSE2` 和 `AVX2`
  各有向量版本，`NEON` 暂用标量；其余各项见 [ROADMAP](../ROADMAP.md#分词吞吐simd)。2026-11 在 i5-12500 上，
  分隔符扫描让各模式快约 3%–4%，但 `-mavx2` 本身让其余代码慢约 1.5%–2%；CPP 规则的 Viterbi 内核另让 MIX 和
  SEARCH 在 `AVX2` 下快约 3%–4%、`SSE2` 下快约 1%–2%，HMM 模式快约 15%–24%；Python/Rust 规则的内核让 HMM 模式
  快约 24%–28%，MIX 快约 1%–2%。合计起来，MIX 上两级相当，SEARCH 和 HMM 模式 `AVX2` 更快。

## 构建选项

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `BUILD_TESTING` | 顶层 ON；子项目未预设时 OFF | 构建单元测试 |
| `CPPJIEBA_BUILD_EXAMPLES` | OFF | 构建应用示例 |
| `CPPJIEBA_BUILD_LEGACY_TESTS` | OFF | 增加原版 CppJieba 回归测试，要求 `BUILD_TESTING=ON` |
| `CPPJIEBA_BUILD_BENCHMARKS` | OFF | 构建独立性能程序 |
| `CPPJIEBA_BUILD_RUST_BENCHMARKS` | OFF | 增加 Rust 比较，要求 benchmark 和 Cargo |
| `CPPJIEBA_ENABLE_ICU` | OFF | 增加 ICU 编码转换目标及测试 |
| `CPPJIEBA_SIMD` | OFF | 向量化内核的指令集：`OFF`、`SSE2`、`AVX2`、`NEON`，见 [SIMD](#simd) |
| `CPPJIEBA_BUILD_C_API` | OFF | 构建 C ABI 库 |
| `CPPJIEBA_C_API_BUILD_SHARED` | ON | 启用 C API 后构建共享库 |
| `CPPJIEBA_C_API_BUILD_STATIC` | ON | 启用 C API 后构建静态库 |
| `CPPJIEBA_BUILD_FUZZERS` | OFF | 构建 libFuzzer 目标，要求 Clang |
| `CPPJIEBA_INSTALL_LEGACY_DICT_DIR` | OFF | 过渡选项：词典同时安装到 1.2 及更早版本的 `share/cppjieba/dict/` |

启用 C API 时至少保留一种库形式。测试与比较依赖的版本、初始化和离线配置见 [依赖指南](../deps/README.md)。

## 测试

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-tests --config Debug --parallel
ctest --test-dir build-tests -C Debug --output-on-failure
```

GoogleTest 按父项目目标、本地安装、源码下载的顺序查找；使用已有安装时设置 `GTest_DIR` 或 `CMAKE_PREFIX_PATH`。
单元测试覆盖分词、编码、输出契约和容量变体，安装消费端测试检查包导出及可选组件。
`CPPJIEBA_SIMD=OFF` 时，本机能执行的每个 SIMD 级别另有一个 `neo_simd_<级别>` 测试。
GCC 构建另有 `neo_inline_hot_paths`，检查逐字符热点函数在内联预算耗尽时仍被内联。
启用 ICU 或 C API 后，CTest 增加相应的验证。性能测量命令见 [benchmark 指南](../benchmark/README.md)。

## 质量检查

CI 中每种工具链只用一个版本，Linux 任务自行安装：GCC 16 来自 Ubuntu 26.04 软件源（该源提供的是 16-20260322 开发快照），
LLVM 23 来自 apt.llvm.org，软件源由 `tools/add-llvm-apt-source.sh` 添加。Linux 上的 Clang 任务同时安装 libstdc++ 16，
与 GCC 任务使用同一标准库。本地格式化请使用 clang-format 23，与 CI 结果保持一致。
[Quality 工作流](../.github/workflows/quality.yml) 各项检查的本地命令如下：

| 检查 | 本地命令 |
| --- | --- |
| clang-format | `tools/clang-format-recursive.sh --check`；去掉 `--check` 即就地格式化 |
| clang-tidy | `BUILD_DIR=build tools/clang-tidy-recursive.sh --check`；去掉 `--check` 即循环应用修复 |
| clang 静态分析 | `CLANG_TIDY_CHECKS='-*,clang-analyzer-*,-clang-analyzer-cplusplus.Move,-clang-analyzer-optin.core.EnumCastOutOfRange' PER_FILE_TIMEOUT=900 BUILD_DIR=build tools/clang-tidy-recursive.sh --check` |
| 头文件自足 | `tools/check-headers.sh`，`CLANG_CXX` 指定 Clang；`BUILD_TESTING` 构建同时编译 `neo_header_check` |
| ASan + UBSan、TSan | 以 GCC 和工作流中的 `-fsanitize` 参数配置 Debug 构建，再运行 `ctest`；ASan + UBSan 另加 `-D_GLIBCXX_ASSERTIONS` |
| 警告即错误 | 以 GCC 或 Clang、`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wno-float-conversion`（Clang 另加 `-Wno-implicit-float-conversion`）和 `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` 配置 Debug 构建 |
| libc++ 加固 | 以 Clang、`-stdlib=libc++` 和 `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG` 配置 Debug 构建，`-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON` 让 GoogleTest 按同一标准库从源码构建，再运行 `ctest` |
| 隐式整数转换 | 以 Clang、`-fsanitize=implicit-conversion` 和 `-fsanitize-ignorelist=tools/implicit-conversion.ignorelist` 配置 Debug 构建，再运行 `ctest` |
| libFuzzer | 以 `-DCPPJIEBA_BUILD_FUZZERS=ON` 构建，再运行 `tools/fuzz.sh <构建目录> [每个目标的秒数] [目标...]`；`--list` 列出目标 |
| Fuzz 语料回放 | `BUILD_TESTING` 构建中运行 `ctest -R neo_fuzz_replay`，任意编译器均可 |
| 覆盖率 | 以 `-fprofile-instr-generate -fcoverage-mapping` 构建，再运行 `tools/llvm-coverage.sh <构建目录> <输出目录>` |
| 指令数 | `tools/instruction-counts.sh <工作目录> <基准修订>`，需要 Valgrind；见 [benchmark 指南](../benchmark/README.md#instruction-counts) |
| C ABI 兼容性 | `tools/check-c-abi.sh <工作目录> [基准修订]`，需要 libabigail 的 `abidiff`；默认与最新的 `v*` 标签比较 |

- clang-tidy 读取 `compile_commands.json`，构建目录需启用工作流中列出的全部可选组件。
  `.clang-tidy` 启用 `bugprone-*`，并排除标出有意写法的几项；路径敏感的 `clang-analyzer-*` 比其他检查慢数倍，
  由单独的 Quality 任务通过 `CLANG_TIDY_CHECKS` 运行。确属有意的个别写法用带理由的 `NOLINT` 注释标出。
  `CLANG_FORMAT`、`CLANG_TIDY` 指定工具路径；`GCC_TOOLCHAIN` 设为空值时使用编译器默认的 GCC。
- 警告即错误任务覆盖全部容量变体。窄容量类型的运算经整数提升变为 `int`，库代码在原本隐式缩回的位置写出显式转换，
  `-O3` 下生成的机器码与改写前逐函数一致。以 `RuneIndex` 作容器下标时经 `detail::to_index` 显式转换为 `size_t`，
  128 位变体因此同样开启转换警告；下标总在被访问容器的长度之内，转换保留原值。
- Sanitizer 与覆盖率构建同时启用共享和静态 C ABI。GCC 的 sanitizer 运行库以动态库链接，满足共享库的 `-z defs`；
  sanitizer 会替换分配器，因此 sanitizer 任务跳过检查私有运行库的 `neo_c_api_symbols` 和 `neo_c_api_c_host`，
  二者由 C API 工作流在常规构建中运行。
- C API 工作流用 `tools/check-c-abi.sh` 比较共享库与最新发布版：新增函数只列入报告，已发布符号或公开类型的变化使检查失败。
  `--headers-dir` 把比较限于公开头文件中的类型，不透明句柄的内部实现可以自由变化。
  `test/unittest_neo/c_api_abi_v1.c` 在各平台编译期冻结 v1 的类型、布局、数值和函数类型。
- Fuzz 目标在 Debug 下构建以保留库断言，导致崩溃的输入写入 `<构建目录>/fuzz-run/artifacts/`。
- `test/fuzz/corpus/<目标>/` 保存按边覆盖精简的语料，`tools/fuzz.sh` 只读取它，新输入写入 `<构建目录>/fuzz-run/corpus/`。
  运行 `<构建目录>/fuzz/<目标>_fuzzer -set_cover_merge=1 -use_counters=0 test/fuzz/corpus/<目标> <构建目录>/fuzz-run/corpus/<目标>`
  把带来新覆盖的输入并入语料；修复崩溃后把复现输入放入对应目录，作为回归测试。
- `tools/fuzz.sh` 按输入格式为每个目标选用 `test/fuzz/dict/` 中的 libFuzzer 字典。`FUZZ_FORK=<n>` 为每个目标运行 n 个
  fuzz 进程，`FUZZ_MINIMIZE=1` 在每个目标结束后把工作语料合并为带来覆盖的输入。
- Quality 工作流对每个目标 fuzz 30 秒，从缓存的语料出发；PR 只读取缓存，master 的推送精简后保存。
  [Nightly fuzz 工作流](../.github/workflows/fuzz-nightly.yml)每天为每个目标开一个任务，以 4 个进程 fuzz 10 分钟，
  再把各目标精简后的语料合并存入同一缓存；手动触发时可指定每个目标的秒数。
- 覆盖率脚本把 `summary.md` 写入输出目录，CI 将它发布为 job summary：总计、各目录和覆盖最少的文件按行和分支统计，
  任一测试程序或模板实例到达即算覆盖。llvm-cov 把返回语句之后不可达的块尾（如 `case` 末尾的 `}`）记为 0，
  因此只含右花括号的行在其前一可执行行被覆盖时计为覆盖。设置 `COVERAGE_BASE=<修订>` 时另列相对该修订改动行的覆盖，
  PR 以目标分支为基准；`summary.txt` 保留 llvm-cov 按函数的统计，同一函数的多个实例覆盖不同时，两者的数字会有差异。

## 仓库布局

| 目录 | 职责 |
| --- | --- |
| `include/neo/` | 一级目录是门面、`Config.hpp` 和 `Fwd.hpp`；按角色划分的二级目录（见 [设计原则·目录](design.md#目录)）及随库第三方头文件 |
| `api/` | C ABI 头文件、适配实现和平台规则 |
| `cmake/` | 项目选项、安装导出和共享测试数据 |
| `deps/` | 测试、比较依赖的配置及子模块 |
| `dict/`、`examples/` | 词典模型、可运行示例 |
| `test/` | 单元测试、fuzz 目标、消费端测试、测试数据和 CMake 验证脚本 |
| `benchmark/`、`docs/benchmarks/` | 测量程序、报告生成工具及已发布的数据快照；`benchmark/third_party/` 为随附的第三方头文件 |
| `tools/` | 格式化、静态分析、fuzz 和覆盖率脚本 |

`build*/`、`cmake-build-*/`、`install/` 和 `benchmark/results/` 用于生成输出及本地测量记录。

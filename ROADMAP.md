# Roadmap

本文记录 neo-cppjieba 在 1.2.0 之后的计划方向。版本划分表示优先级，不承诺发布日期；
各项完成后移入 [CHANGELOG](CHANGELOG.md)。1.3 与 1.4 的内容合并为 2.0.0 发布。

## 现状

1.2.0 已提供：

- MIX、MIX_NO_HMM、FULL、SEARCH、SEARCH_NO_HMM、HMM、MP 分词，以及编译期 CPP / RUST / PYTHON 规则。
- UTF-8、UTF-16、UTF-32 输入，借用、持有原文和独立字符串三种结果，可复用 `Workspace`。
- 可选 ICU 编码转换、C ABI v1、可安装的 CMake 包。
- Linux x64/ARM64、macOS ARM64、Windows x64 CI，以及与 CppJieba、jieba-rs、Python jieba 的性能对比。

主要差距：原版 CppJieba 的运行时加词尚未迁移；C ABI 只覆盖单次分词。

## 1.3 — 补齐 CppJieba 功能

从 CppJieba 迁移时最常见的阻碍，作为下一版本主线。

### 词性标注

`Jieba` 的分词入口已支持 `Tagging::Pos` 模板参数和 `lookup_tag`，见 [词性标注](docs/usage.md#词性标注)。

- [x] 增加带词性的输出入口，例如 `cut_tagged`，以及 `cut_into` 可接受的带词性输出类型。
- [x] 未登录词沿用上游规则：数字为 `m`，英文为 `eng`，其余为 `x`。
- [x] 将根目录的 [词性标记表](docs/pos_tags.md) 移入 `docs/`，并在使用指南中引用。

### 关键词提取

提取器见 [关键词提取](docs/usage.md#关键词提取)。

- [x] 新增独立的可选头文件 `neo/KeywordExtractor.hpp`，提供 TF-IDF 和 TextRank。
- [x] 复用 `Workspace` 和借用结果，不给核心分词入口增加依赖。
- [x] 与原版 `KeywordExtractor` / `TextRankExtractor` 的结果做比较测试。
- [x] 可选的词性过滤（Python jieba 的 `allowPOS`），基于已有的词性标注。

### 词典加载与运行时加词

[`DictTrie`](include/neo/resource/DictTrie.hpp) 可从文件或内存加载，见 [从内存加载词典](docs/usage.md#从内存加载词典)。

- [x] 支持从内存加载主词典、用户词典、HMM 模型、IDF 和停用词（`DictSource::memory`），
  覆盖资源内嵌、WASM 等不便访问文件系统的场景；用户词典可有多个。
- [x] 支持就地加词和删词（`add_word`、`add_word_with_freq`、`remove_word`），对应 CppJieba 的
  `InsertUserWord` 和 Python jieba 的 `add_word` / `del_word`。权重按加载时主词典的频率总和归一化。
- [x] 确定并发模型：库内部不加锁。分词入口为 `const`，增删词为非 `const`，需要独占访问；
  调用方按场景选择加锁、写时复制或单线程使用，分词热路径不加锁。
- [x] 提供 `Jieba::clone()` 支持写时复制，并提供 `compact()` 回收删词和加词留下的无效节点、恢复紧凑存储。
- [x] 关键词提取器不再保存 `Jieba`，每次调用显式传入 `const Jieba &`。
- [ ] 支持 Python jieba 的 `suggest_freq`。

## 1.4 — 扩展 C ABI v1

只新增符号，不改变已有 v1 结构布局和函数语义。

- [x] **明确并发契约。** 头文件要求同一 engine 的调用串行，而 C++ 的 `Jieba::cut*` 是 `const`，
  且每次调用使用独立 `Workspace`。确认 `Logging` 等静态状态无数据竞争后，将契约放宽为同一 engine
  可并发分词，并增加 TSan 测试。这决定语言绑定能否在分词时释放 GIL 或跨线程共享 engine。
- [x] 增加可复用的 workspace 和结果对象，例如 `neo_jieba_v1_workspace_create` 与 `cut_utf8_into`，
  避免每次调用重新分配和复制原文；提供只返回字节区间、不复制原文的变体。
- [x] 从内存创建 engine（依赖 1.3 的内存加载），以及词典快照的加载和写出。
- [x] 1.3 的 C++ 接口稳定后，暴露词性和关键词提取，以及运行时增删词和写时复制。
- [ ] 日志回调，与下文的日志策略一致。

## 1.5 — 库的健壮性

### CI 质量门禁

[Quality 工作流](.github/workflows/quality.yml) 运行以下检查，本地命令见 [质量检查](docs/building.md#质量检查)。

- [x] ASan + UBSan 任务，覆盖单元测试和共享、静态两种 C ABI 测试。
- [x] TSan 任务，覆盖 C++ 并发分词、关键词提取和 `clone()` 写时复制，以及 C ABI 共享 engine 和共享结果的并发调用。
- [x] libFuzzer 目标：UTF 解码、主词典 / 用户词典 / HMM 模型解析、`neo_jieba_v1_cut_utf8_owned`。
- [x] clang-format 与 clang-tidy 检查。
- [x] 启用 `misc-include-cleaner`：门面头文件以 `// IWYU pragma: export` 标注转出的角色头文件，其余代码直接 include。
- [x] 头文件自足检查：每个库头文件单独编译，模板中依赖实参的调用不依赖包含方先引入的声明。
- [x] llvm-cov 覆盖率报告。

### 日志与错误处理

[`util/Logging.hpp`](include/neo/util/Logging.hpp) 直接写 stderr，断言失败时调用 `std::terminate`。

- [ ] 提供可替换的日志输出，并允许完全静默。
  现状：库内已无日志调用点；接收器式的 `log` 收进 `detail` 留作归档，失败的 `assert_check` 仍写 stderr 并终止。
- [ ] 区分调用方输入错误（异常或状态码）与内部不变量破坏（保持 terminate），在文档中列出各入口的失败方式。
- [ ] 评估 `std::expected` 风格的入口，例如 `Jieba::open(...)` 和 `try_cut`，服务禁用异常的调用方。

### 加载性能

- [x] 预编译的词典快照：`Jieba::encode_dict_snapshot` 生成，`Jieba::from_snapshot` 加载，旧版本快照持续可读，
  见 [词典快照格式](docs/snapshot.md)。[`load_benchmark`](benchmark/load_benchmark.cpp) 测得内置词典的 `Jieba`
  构造从约 85 ms 降到约 19 ms，冷页缓存下从约 88 ms 降到约 22 ms。

### 分词吞吐（SIMD）

2026-11 的剖析中，可向量化的阶段是 UTF-8 解码（约 7%）、分隔符扫描（约 5%）和 HMM Viterbi（约 14%），
合计约占 MIX 耗时的 27%，预计整体收益 10%–15%。路由 DP 与 trie 查找（约 58%）是串行依赖加指针追逐，
改进方向在数据布局。GPU 不予考虑：单次调用工作量小，逐位一致需要 FP64，且与仅头文件分发冲突。

- [ ] 分隔符扫描改为可移植的分块扫描：整块无分隔符时直接跳过，否则用 ctz 取位置，不使用 intrinsics。
- [ ] UTF-8 解码快速路径：ASCII 按 8 字节检查、连续 3 字节序列快速解码、预分配写入；
  非法输入回退到现有标量路径，异常位置和信息保持不变。
- [ ] CPP 规则的 Viterbi 按 4 个目标状态向量化（SSE2 / NEON，AVX2 可选），保持加法顺序和严格 `>` 的平局规则。

每项须在 `cut_compare_benchmark` 中让 MIX 或 SEARCH 提升至少 3%，各模式回退不超过 1%，输出逐位一致，否则放弃。
SIMD 代码集中在独立头文件，按编译期特性选择，保留标量实现和对应测试；暂不做运行时 CPU 派发。

### 降低工具链要求

当前要求 GCC 15、LLVM 23 / libc++ 或 Visual Studio 2026，主要因为使用了 `<print>` 和 `<format>`。

- [ ] 统计库头文件中对这些设施的依赖；若仅限日志和诊断，引入一层薄封装。
- [ ] 目标支持 GCC 14、Clang 18 和主流发行版自带工具链，并增加最低版本 CI 任务固定这条下限。

## 1.6+ — 生态与易用性

- [ ] **Python 绑定**：基于 nanobind 或 C ABI，通过 cibuildwheel 发布 wheel。
- [ ] 视需求提供 Rust `-sys` crate、Go cgo 绑定。
- [ ] **包管理器**：vcpkg port、Conan recipe、xmake 包。
- [ ] **发布产物**：tag 触发的 release workflow，将 C API CI 已生成的各平台包附加到 GitHub Release，
  附带校验和与构建来源证明。
- [ ] **英文文档**：`README.en.md` 和英文使用指南；生成 API 参考站点（Doxygen 或 mkdocs）。
- [ ] **集成示例**：WASM（emscripten）构建示例；SQLite FTS5 或搜索引擎分词器插件示例。

## 维护清理

不依赖版本节奏，可随时处理。

- [x] [`include/neo/Jieba.hpp`](include/neo/Jieba.hpp) 中 `Jieba` 的类注释仍描述旧接口
  （"These usage notes describe the former facade"）；`cut_runes_into` 中的 "Former policy" 注释同理，改写为当前契约。
- [x] 明确公开边界：`DictTrie`、`HMModel`、`Trie`、各 `*Segment` 位于 `detail/` 目录却在公开命名空间
  `neo_cppjieba` 中，`PosTag` 是公开类型却位于 `detail/`。见 [架构](docs/architecture.md#公开边界)。
- [x] 统一命名：项目名改为 neo-cppjieba，与命名空间和包名 `neo_cppjieba` 一致。词典改为安装到
  `share/neo_cppjieba/dict`，避免与上游 CppJieba 冲突；过渡选项 `CPPJIEBA_INSTALL_LEGACY_DICT_DIR` 保留旧位置，
  CMake 包导出 `neo_cppjieba_DICT_DIR`。
- [x] 删除 [`.github/workflows/cmake.yml`](.github/workflows/cmake.yml) 中的模板注释。
- [x] 清点 `test/testdata/`：移除上游 server 时代的文件（`server.conf`、`load_test.urls`、`curl.res`）及未引用的
  GBK 词典、分片词典、`review.100.res` 和 `testlines.gbk`。
- [x] 确认现代库不依赖 `deps/limonp`：它仅服务 benchmark 中的 FileIO 对照和 legacy 加载测试，见 [依赖说明](deps/README.md)。

## 优先顺序

1. C ABI 并发契约与 sanitizer / fuzz：成本低，直接提升可信度。
2. 1.3 的三项功能：决定 CppJieba 用户能否迁移。
3. 降低工具链要求与 Python 绑定：扩大可用人群最有效的两项。

欢迎通过 issue 讨论优先级或认领条目。

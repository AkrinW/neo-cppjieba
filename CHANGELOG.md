# Changelog

## Unreleased

### Added

- 新增词性标注：`cut`、`cut_with_workspace`、`cut_owned`、`cut_strings`、`cut_each` 接受模板参数
  `Tagging::Pos`（默认 `Tagging::None`），`cut_into` 接受 `TaggedPosition` 与 `TaggedWord` 输出，另增 `lookup_tag`。
  未登录词沿用 CppJieba 规则标为 `m`、`eng` 或 `x`；MIX 结果与 CppJieba `Tag` 一致。
  默认的 `Tagging::None` 生成的代码不变。见 [词性标注](docs/usage.md#词性标注)。
- `jieba_benchmark` 增加 TAG 项，与 CppJieba `Tag` 比较结果和吞吐。
- 新增可选头文件 `neo/KeywordExtractor.hpp`：`TfIdfExtractor` 和 `TextRankExtractor`，
  输入支持 UTF-8 / UTF-16 / UTF-32，结果含每次出现的位置，可通过 `KeywordWorkspace` 复用缓冲。
  分数与 CppJieba `KeywordExtractor` / `TextRankExtractor` 逐位一致，legacy 测试在语料上逐项比较。
  见 [关键词提取](docs/usage.md#关键词提取)。
- 关键词提取支持按词性过滤：`TfIdfOptions` / `TextRankOptions` 的 `allowed_tags` 对应 Python jieba 的 `allowPOS`，
  默认不过滤；`Keyword::tag` 返回每个关键词的词性。
- 新增 `DictSource`：主词典、用户词典、HMM 模型、IDF 和停用词均可从文件或内存加载，
  `Jieba`、`HMModel`、`TfIdfExtractor`、`TextRankExtractor` 增加对应构造函数；用户词典可有多个。
  见 [从内存加载词典](docs/usage.md#从内存加载词典)。
- 新增词典快照：`Jieba::encode_dict_snapshot` 把主词典和用户词典编译为快照，`Jieba::write_dict_snapshot`
  把它原子地写成文件，`Jieba::from_snapshot` 通过新的 `SnapshotSource`（文件或内存）加载。结果与从源词典加载完全相同，内置词典的加载时间约从 85 ms 降到 15 ms；
  新版本的库持续读取旧版本写出的快照。见 [词典快照](docs/usage.md#词典快照)。
- 新增 `load_benchmark`：校验快照与文本加载的分词结果一致后，分阶段计时词典加载，`--cold` 在每轮前清除页缓存。
- `Jieba` 支持运行时就地增删词：`add_word`、`add_word_with_freq`、`remove_word`，权重规则与用户词典一致，
  库内部不加锁；`clone()` 显式复制实例用于写时复制，`compact()` 回收增删留下的存储。
  见 [运行时增删词](docs/usage.md#运行时增删词)。
- 新增构建选项 `CPPJIEBA_BUILD_FUZZERS`（要求 Clang），构建 UTF 解码、主词典 / 用户词典 / HMM 模型解析和
  `neo_jieba_v1_cut_utf8_owned` 的 libFuzzer 目标；CI 增加格式、clang-tidy、sanitizer、fuzz 和覆盖率检查。
  见 [质量检查](docs/building.md#质量检查)。
- 每个 fuzz 目标在 `BUILD_TESTING` 下另建 `neo_fuzz_replay_<目标>` 测试，用任意编译器回放 `test/fuzz/corpus/` 中
  按边覆盖精简的语料，sanitizer 和覆盖率任务因此也运行这些输入；`tools/fuzz.sh` 从该语料出发。
- 新增 C ABI fuzz 目标：`c_api_ranges` 对照独立结果和 `lookup_tag` 检查复用存储的区间与标注，
  `c_api_sources` 对照 C++ API 检查内存源和快照的加载与分词，`c_api_edit` 在克隆引擎上随机增删词并 `compact`，
  `c_api_keyword` 检查两种排序的参数校验、顺序、出现位置和标注；每次调用都检查错误缓冲区契约。
- 新增 C++ fuzz 目标：`keyword` 从变异的 IDF 表和停用词构建提取器并检查结果，`snapshot_roundtrip` 检查快照编码确定，
  且在全部模式下与源词典分词、标注一致，`utf_cut` 检查同一文本的 UTF-8 / UTF-16 / UTF-32 输入分词、标注一致，
  源偏移落在各自的码元上。
- Fuzz CI 在运行之间缓存并精简语料，并按输入格式使用 libFuzzer 字典；PR 与 master 每个目标 fuzz 30 秒，
  新增的 Nightly fuzz 工作流每天为每个目标并行 fuzz 10 分钟。`tools/fuzz.sh` 增加 `--list`、`FUZZ_FORK` 和 `FUZZ_MINIMIZE`。
- ASan + UBSan 任务启用 `-D_GLIBCXX_ASSERTIONS`，检查标准库容器、span 和视图的越界访问。
- C ABI 兼容性检查：`c_api_abi_v1.c` 在各平台编译期冻结 v1 的类型、布局、数值和函数类型；C API 工作流用
  `tools/check-c-abi.sh` 调用 libabigail 的 `abidiff` 与最新发布版比较，已发布符号或公开类型变化时失败。
- Quality 工作流新增 GCC 与 Clang 警告即错误任务（`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`），
  覆盖全部容量变体；窄容量下原本隐式的缩窄转换改为显式，`-O3` 生成的机器码不变。
  为此 `TfIdfOptions` / `TextRankOptions` 的 `allowed_tags` 增加默认成员初始化，指定初始化不再触发
  `-Wmissing-field-initializers`；`uint128_t` 别名标为 `__extension__`，`-Wpedantic` 下不再告警。
- clang-tidy 增加 `bugprone-*` 检查，另有 `clang-analyzer` 任务运行 Clang 静态分析器；`tools/clang-tidy-recursive.sh`
  支持用 `CLANG_TIDY_CHECKS` 覆盖检查集。有意的写法以带理由的 `NOLINT` 标出，测试中的乘法改为显式宽度。
- Quality 工作流新增 Clang + libc++ 任务，以 `_LIBCPP_HARDENING_MODE_DEBUG` 运行单元测试、C ABI 测试和 fuzz 语料回放，
  检查越界、迭代器失效和标准库前置条件。
- Quality 工作流新增 Clang 隐式整数转换检查：单元测试、C ABI 测试和 fuzz 语料回放在 `-fsanitize=implicit-conversion` 下运行，
  本仓库代码中改变数值的截断或符号转换会使测试失败；标准库模板经 `tools/implicit-conversion.ignorelist` 排除。
- 新增 `instruction_workload` 与 `tools/instruction-counts.sh`：用 callgrind 分别统计加载、快照加载、分词、标注和关键词提取的
  指令数，Quality 工作流在 job summary 中报告与基准修订的差异。
- 覆盖率任务在 job summary 中按行和分支汇总总计、各目录和覆盖最少的文件，PR 另报告改动行的覆盖；
  统计取所有测试程序和模板实例的并集，只含右花括号的行随其前一可执行行计入，HTML 报告显示分支计数，
  构建目录中生成的头文件不再计入。
- C ABI v1 新增 25 个函数，已有符号、结构布局和函数语义不变。见 [C ABI](api/README.md#calls-and-ownership)。
  - 可复用的 workspace 和只含字节区间的结果：`neo_jieba_v1_cut_utf8_into` 把分词结果写成调用方原文中的
    字节区间，不复制原文，并复用 workspace 的临时存储和结果容量。
  - `neo_jieba_source_v1`：主词典、HMM 模型和任意数量的用户词典可从文件或内存加载
    （`engine_create_from_sources`），路径带显式长度；`write_dict_snapshot` 原子地写出词典快照，
    `engine_create_from_snapshot` 从文件或内存加载快照。
  - 词性标注：`cut_utf8_tagged_into` 在字节区间之外给出平行的 `neo_jieba_pos_tag_v1` 数组，
    `lookup_tag` 标注单个完整词语；规则与 C++ 的 `Tagging::Pos` 和 `lookup_tag` 相同。
  - 运行时增删词：`engine_add_word`、`engine_add_word_with_freq`、`engine_remove_word` 和 `engine_compact`
    需要独占 engine，库内部不加锁；`engine_clone` 只读原 engine，用于写时复制。
  - 关键词提取：`tfidf_create` / `textrank_create` 从文件或内存加载资源，`tfidf_extract_into` /
    `textrank_extract_into` 复用 workspace 和 `neo_jieba_keywords_v1` 结果，每个关键词给出权重、词性和
    原文中全部出现位置的字节区间，不复制原文；结果与 C++ 提取器一致。

### Changed

- C ABI v1 的并发契约放宽：多个线程可同时在同一 engine 上分词、同时读取同一结果，
  并发调用各自使用独立的输出槽和诊断缓冲；TSan 任务覆盖共享 engine 和共享结果的并发调用。
  已有符号、结构布局和函数语义不变。见 [C ABI](api/README.md#calls-and-ownership)。
- 门面头文件以 `// IWYU pragma: export` 标注公开类型所在的头文件，clangd 等 include-cleaner 工具把门面视为这些类型的
  提供者；内部头文件去掉未使用的标准库头文件，依赖门面间接引入 `<limits>`、`<cstddef>` 等头文件的代码需自行 include。
- **不兼容**：`TfIdfExtractor`、`TextRankExtractor` 只持有 IDF 和停用词，构造函数去掉选项参数；
  `TfIdfOptions`、`TextRankOptions` 改为 `extract` / `extract_into` 在 `top_n` 之后的参数，按 `const &` 传入且不被保存，
  `options()` 移除。`TextRankOptions::damping` 改为在每次调用入口检查。见 [关键词提取](docs/usage.md#关键词提取)。
- **不兼容**：移除 `WordRange::to_string`，值类型不再携带编码操作；改用 `Utf::encode<CharT>(range.slice(runes))`。
- 内部结构按角色进一步解耦，行为不变：`Workspace` 与 `KeywordWorkspace` 都经唯一的 `detail` 访问点拆包，
  关键词状态按算法分组；回调参数都有 concept，算法积木的参数按统一签名顺序排列；`Trie`、`DictTrie` 去掉解码字符串的
  查询重载；`DictTrie` 的 `UserWordWeightOption` 移到 `options/`，不再用默认参数。见 [设计原则](docs/design.md)。
- **不兼容**：头文件按角色重新组织。`include/neo/` 一级目录只保留门面 `Jieba.hpp`、`KeywordExtractor.hpp`、
  `Utf.hpp`（原 `Unicode.hpp`），编译期配置 `Config.hpp`，以及新增的前向声明头 `Fwd.hpp`；其余头文件按角色移入
  `value/`、`options/`、`workspace/`、`concept/`、`resource/`、`algorithm/`、`util/`，原 `detail/` 目录取消，
  `Traits.hpp` 改为 `concept/Text.hpp`，`Token.hpp` 并入 `value/Tokens.hpp`。命名空间不变；用户代码只 include
  一级目录的头文件。见 [设计原则·目录](docs/design.md#目录)。
- 公开 API 收缩到门面、结果类型、配置、上下文和输入 concept：Unicode 工具改为 `Utf::is_valid`、`Utf::decode`、
  `Utf::encode<CharT>`；`is_valid_utf`、`decode`、`encode`、`decode_one`、`encode_one`、`decode_with_offset*`、
  `as_view`、`as_code_units`、`Unicode`、`Encoding`、`encoding_of`、`ByteType`、`CodeUnit` 移入 `neo_cppjieba::detail`。
- `TfIdfExtractor`、`TextRankExtractor` 改为只能移动，复制用 `clone()`。
- 内部分词 policy 统一为 `cut_into(dict, model, runes, out, scratch)` 并由 `SegmentPolicy` concept 约束；
  `MPSegment::cut` 等静态便捷入口改为 `detail::cut<Policy>(dict, model, runes)`。

- **不兼容**：`TfIdfExtractor`、`TextRankExtractor` 不再保存 `Jieba`。构造函数去掉 `Jieba` 参数；
  `extract` / `extract_into` 的第一个参数改为 `const Jieba &`。见 [关键词提取](docs/usage.md#关键词提取)。

- **不兼容**：词典和模型改为安装到 `share/neo_cppjieba/dict/`，避免与上游 CppJieba 的 `share/cppjieba/dict/` 冲突。
  CMake 包新增 `neo_cppjieba_DICT_DIR`；过渡选项 `CPPJIEBA_INSTALL_LEGACY_DICT_DIR=ON` 同时安装到旧位置。
- 项目名改为 neo-cppjieba，CMake `project()` 名改为 `neo_cppjieba`，版本变量相应为 `neo_cppjieba_VERSION`；
  命名空间、包名、目标和 C ABI 不变。
- 移除 `test/testdata/` 中未使用的上游 server 配置、GBK 词典和分片词典等测试数据。
- 词性标记表移至 [docs/pos_tags.md](docs/pos_tags.md)。
- 整理内部架构，分词结果不变：`CutMode` 移至 `neo/options/CutMode.hpp`（`neo/Jieba.hpp` 仍包含它）；
  运行时 `CutMode` 与编译期 `SegmentationStyle` 只在 `algorithm/Segmenter.hpp` 中分派，`Jieba` 不再分支；
  CPP 风格的分隔符切分、MIX / SEARCH 共用的路由遍历各收敛为一处实现。见 [架构](docs/architecture.md)。
- `SegmentScratch` 不再携带 DAG：分词直接匹配 Trie，`Trie::find_dag` 仅用于诊断和对照测试。
  移除仅被测试使用的 `detail::mp_cut_segment`、`mp_cut_dag`、`mp_trace_route` 和 `mp_cut_one_segment`。
- **不兼容**：内部类型和函数移入 `neo_cppjieba::detail`，包括 `DictTrie`、`DictUnit`、`Trie`、
  `TrieStats`、`Dag`、`HMModel`、`HMMState`、`MIN_DOUBLE`、各 `*Segment`、`FileBuffer`、`read_file` 和
  `StringUtil.hpp` 中的解析与分隔符函数。只使用 `Jieba`、结果类型和关键词提取的代码不受影响；
  直接使用内部类型的代码需加 `detail::` 限定。见 [公开边界](docs/architecture.md#公开边界)。
- `PosTag` 和 `pos::` 常量移至 `neo/value/PosTag.hpp`，命名空间不变；`neo/Jieba.hpp` 仍包含它。
- `with_source_text` 移入 `neo_cppjieba::detail`；用户词典单字集合改用 `gtl::flat_hash_set`，
  且无单字时跳过查询。
- CI 每种工具链只用一个版本：Linux 任务从 Ubuntu 软件源安装 GCC 16，经 `tools/add-llvm-apt-source.sh` 从 apt.llvm.org
  安装 LLVM 23，与 macOS 的 Homebrew LLVM 23 一致；Clang 任务同时安装 libstdc++ 16。

### Upgrade notes

- 本版发布为 2.0.0。C ABI 仍是 v1：已有符号、结构布局和函数语义不变，共享库 SOVERSION 保持 1，
  已构建的程序无需重新编译。CMake 包版本随项目升至 2.0，C 用户把 `find_package(neo_cppjieba_c 1.2 ...)`
  改为 `find_package(neo_cppjieba_c 2.0 ...)`。
- 新增的 C 函数要求新版共享库。Linux 上它们带有符号版本 `NEO_CPPJIEBA_C_1.1`，
  旧库在加载时即报告缺少该版本。

## 1.2.0 - 2026-10-23

本版增加适合 C 和其他语言绑定的 C ABI，以及可直接通过 `find_package` 使用的 C++ 安装包。
文档按快速上手、使用、构建、编码转换和 C API 分开组织。

### Added

- 新增 [C ABI v1](api/README.md)：六个 C 函数覆盖引擎创建、UTF-8 分词、结果读取和资源释放。
  结果独立持有原文及 token 字节区间，错误通过状态码和调用方提供的诊断缓冲返回。
- 提供共享库和静态库，覆盖 Linux x64/ARM64、macOS ARM64 和 Windows x64 的 Debug / Release CI。
  CMake 包 `neo_cppjieba_c` 导出 `c_shared`、`c_static` 目标，并保留 ABI 主版本 1。
- 安装 C++ 包 `neo_cppjieba`，导出 `neo_cppjieba::neo_cppjieba` 和可选 `icu` 组件。
  增加安装目录迁移、独立消费端、组件发现和 C ABI 符号边界验证。
- 增加 [C++ 使用指南](docs/usage.md) 和 [构建指南](docs/building.md)，集中说明接口契约及构建选项。

### Changed

- C++ 文本入口统一接受带长度的字符串、视图和范围；字符指针通过显式长度视图接入。
  字符串字面量、字符数组以及已有的 `std::string` / `std::string_view` 调用方式继续适用。
- CMake 按项目选项、安装导出、依赖发现、平台规则和测试配置拆分模块。
- GoogleTest 优先复用父项目目标和本地安装，源码回退固定到 `v1.18.0`，支持离线源码目录。
- 项目、C++ 包和 C API 包的版本统一为 `1.2.0`；共享库 `SOVERSION` 保持为 `1`。

### Fixed

- 修复跨平台测试中的返回值忽略、静态成员未定义及文件关闭函数的模板属性警告。
- 修复 128 位容量配置的范围比较警告，保留各位宽的容量上限和 Trie 哨兵边界。
- 修复 macOS 上 `size_t` 与同宽固定宽度整数类型不同导致的容量测试编译失败。
- 更新 GoogleTest，清理其旧版 CMake 兼容性和字符转换警告；移除 macOS 的冗余、弃用链接参数。

### Upgrade notes

- 从原始指针接收文本时，同时传入长度，例如
  `jieba.cut(std::string_view{data, size}, neo_cppjieba::CutMode::MIX)`；长度覆盖完整输入，包括内嵌 NUL。
- 使用安装包时，将安装前缀加入 `CMAKE_PREFIX_PATH`，通过
  `find_package(neo_cppjieba 1.2 CONFIG REQUIRED)` 接入。C API 的组件选择见 [接入说明](api/README.md)。
- 升级头文件后重新编译使用本库的全部编译单元，并采用一致的 `Config.hpp` 配置。
  主词典、用户词典和 HMM 模型沿用已有文件格式。
- C ABI 的函数及结构版本为 v1，兼容范围是同一平台 ABI；部署时匹配架构、系统要求和运行库配置。

## 1.1.0 - 2026-10-08

本版增加可选编码转换，并修复 CPP 规则下词典权重精度导致的分词兼容性问题。

### Added

- 新增可选 `neo/encoding/IcuCodec.hpp`，支持显式指定 UTF-8、GBK、GB18030 的双向转换，
  并提供 Unicode 码点边界与编码字节位置的映射。
  通过 `CPPJIEBA_ENABLE_ICU=ON` 启用，链接 `neo_cppjieba::icu`；默认核心库仍无 ICU 依赖。
- 新增 `neo/Unicode.hpp` 中的 `is_valid_utf(input)`，按输入字符类型检查 UTF 编码合法性，
  无需分配解码缓冲。该接口不检测输入的原始编码。
- 增加 [编码转换说明](docs/encoding.md)，覆盖借用结果生命周期、原始字节位置映射及 ICU 依赖配置。
- 增加手动触发的 CI benchmark 文档更新：在 GitHub 托管 runner 上测量，生成图表和数据快照，
  通过文档 PR 更新 README 的性能数据。

### Changed

- 将分词规则相关分支改为 `if constexpr`，在编译期选择 CPP、RUST、PYTHON 对应实现。
- CMake 项目声明版本 `1.1.0`，可通过 `modern_cppjieba_VERSION` 获取。

### Fixed

- 所有分词规则统一使用 `double` 词典权重和 MP 路径评分，恢复旧 CppJieba 的评分精度。
  CPP 规则此前使用 `float`，可能将不同概率路径舍入为相同分数，进而选择错误路径。
- 增加近似平局回归测试：词频为 `甲=9999`、`乙丙=10001`、`甲乙=10000`、`丙=10000` 时，
  `甲乙丙` 正确切分为 `甲乙 / 丙`；此前的 `float` 评分会选择 `甲 / 乙丙`。

### Upgrade notes

- 升级后重新编译所有使用本库头文件的编译单元；词典权重及相关内部结构的布局已经变化。
- CPP 规则下受浮点舍入影响的文本可能得到不同分词结果。词典及模型文件格式沿用 `1.0.0`。
- 恢复 `double` 精度会增加相关数据结构大小，部分查询的耗时可能增加。

## 1.0.0 - 2026-09-28

modern-cppjieba 的首个独立版本。

### Added

- 新增 `include/neo/` 下的 C++23 头文件实现，使用 `neo_cppjieba` 命名空间。
- 提供 MIX、MP、HMM、FULL、SEARCH 分词，以及显式关闭 HMM 的模式。
- 支持 UTF-8、UTF-16、UTF-32 输入，返回原文编码单元位置和 Unicode 码点位置。
- 增加借用 token、拥有原文、独立字符串、回调与预解码码点等输出入口。
- 提供调用方管理的 `Workspace`，用于连续分词时复用解码和算法缓冲。
- 增加编译期 `CPP`、`RUST`、`PYTHON` 分词规则配置，以及文本索引、偏移和 Trie 节点的容量类型配置。
- 增加现代实现单元测试、应用示例及 CppJieba / jieba-rs / Python jieba 比较工具。
- 增加 macOS、Windows 平台适配和 CI 构建配置。
- README 增加 CppJieba、neo-cppjieba、jieba-rs 和 Python jieba 的性能折线图，并保存测量快照与绘图脚本。

### Changed

- `Jieba` 构造要求显式提供主词典、HMM 模型和用户词典路径；空用户词典路径表示禁用用户词典。
- 分词入口改为显式 `CutMode` 和按输出形式命名的方法，替代早期现代实现中的 `CutMethod` 模板入口。
- Unicode 编解码通过 `neo/Unicode.hpp` 提供；词典、模型及分词算法等实现头移入 `neo/detail/`，
  公共入口不再暴露 `dict()`、`model()`。
- 通过 Trie 转移存储、直接计算 MP 路径、复用 HMM 缓冲，以及合并 MIX / SEARCH 匹配流程减少中间数据；
  FULL 直接输出匹配，分词热路径不再构造完整 DAG。
- `cut_runes_into` 发生异常时直接传播，输出可能保留部分结果；调用方应丢弃该次输出。
- 将原版 CppJieba 和其他比较依赖隔离在 `deps/`，现代库安装仅包含 `neo/` 头文件及词典数据。
  单测、示例和 benchmark 可分别启用，benchmark 不作为 CTest 测试。
- 精简 README，集中说明接入、结果所有权及配置约束；更新日志独立记录现代实现的演进。
- 合并示例目标的公共链接配置，补充 Visual Studio 构建与运行说明。

### Fixed

- 修复零权重词条、重复词典键和分词评分相关问题。
- 加强非法 Unicode、词典和 HMM 模型输入的校验，并补充分词范围与状态不变量检查。
- 修复 Windows 编译问题，并支持以 UTF-8 参数打开 Windows 路径。

## 上游历史

原 CHANGELOG 中的 v2.1.1–v5.5.0 属于 **CppJieba**，不作为 neo-cppjieba 的发布版本。
完整内容保留在 [改写前的历史文件](https://github.com/AkrinW/neo-cppjieba/blob/3732abc0e5548c96b4a6ea55113a02df97acf761/CHANGELOG.md)，
上游后续记录见 [CppJieba CHANGELOG](https://github.com/yanyiwu/cppjieba/blob/master/CHANGELOG.md)。

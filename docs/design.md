# 设计原则

本文是项目对代码"放在哪里、写成什么形态"的统一约定。[架构](architecture.md) 描述现有的分层和数据流，
本文给出新增或重构代码时的判断标准。两者冲突时，以本文为准，并把架构文档更新为一致。

## 六种角色

每段代码先判断它属于哪种角色。角色之间解耦：算法不持有资源，资源不包含跨资源的算法，
配置和上下文从参数流入，不藏在全局或静态状态里。

| 角色 | 是什么 | 形态 | 生命周期与并发 | 现有例子 |
| --- | --- | --- | --- | --- |
| 资源 | 昂贵、构建后只在显式编辑时改变的数据 | 带不变量的类，只能移动，复制用 `clone()` | 调用方持有；`const` 访问可并发，编辑需独占 | `DictTrie`、`Trie`、`HMModel`、`IdfTable`、`StopWords` |
| 算法 | 组合资源、配置和上下文，产出结果 | policy（只有 static 成员的类）或 `detail` 自由函数 | 无状态 | `MPSegment`、`mp_cut_append`、`segment_into`、`weigh_text_rank` |
| 配置 | 选择算法行为的值 | `enum class` 或聚合 `struct`；编译期配置放在 `Config.hpp` | 随调用流入，标量按值、含容器的按 `const &`；资源的构造配置在构造时折算进资源 | `CutMode`、`TfIdfOptions`、`TextRankOptions`、`UserWordWeightOption`、`SegmentationStyle` |
| 上下文 | 一次调用的可变缓冲 | 调用方持有的对象，保留容量供复用 | 同一时间只属于一个调用 | `Workspace`、`SegmentScratch`、`KeywordWorkspace`（`KeywordState`） |
| 契约 | 角色之间接口的编译期约束 | C++20 `concept` | 无运行时存在 | `StringLike`、`CharType`、`CapacityInteger`、`OutputValue`、`ViewVisitor`、`SegmentPolicy`、`WordEmitter`、`MatchVisitor`、`TagLookup` |
| 门面 | 公开入口 | 持有必须一起出现的资源，成员函数校验、拆包并转发 | 查询为 `const`，编辑为非 `const` | `Jieba`、`TfIdfExtractor`、`TextRankExtractor`、`Utf`（无资源，只有 static 成员） |

依赖方向：门面 → 算法 → 资源。配置和上下文作为参数穿过这条链，契约约束每一处模板参数。

算法的统一签名顺序是 `(const 资源 &..., 输入, 配置, 输出 &, 上下文 &)`，与公开的 `cut_into(input, mode, out, workspace)`
和分派入口 `segment_into(dict, model, runes, mode, out, scratch)` 一致。描述输入位置的参数（如全局偏移 `pos`）紧随输入；
回调按它承担的角色排位：读取资源的回调（如词性查询 `tag_of`）紧随资源，交付结果的回调（发词、访问者）算作输出。

## 目录

目录与角色一一对应：先判断角色，文件位置随之确定。一级目录只放用户 include 或修改的文件，其余按角色进入二级目录。
值类型和基础设施不属于六种角色：前者是角色之间传递的输入与结果，后者被各角色共用。

| 位置 | 内容 | 文件 |
| --- | --- | --- |
| `neo/` | 门面、编译期配置、前向声明 | `Jieba.hpp`、`KeywordExtractor.hpp`、`Utf.hpp`、`Config.hpp`、`Fwd.hpp` |
| `neo/value/` | 值类型：调用的输入与结果，不含算法 | `Rune.hpp`、`TokenView.hpp`、`TaggedToken.hpp`、`Tokens.hpp`、`PosTag.hpp`、`Keyword.hpp`、`DictSource.hpp`、`SnapshotSource.hpp` |
| `neo/options/` | 配置：调用配置与资源的构造配置，及各自的取值检查 | `CutMode.hpp`、`Tagging.hpp`、`KeywordOptions.hpp`、`UserWordWeightOption.hpp` |
| `neo/workspace/` | 上下文：公开的工作区与它们的内部状态 | `Workspace.hpp`、`SegmentScratch.hpp`、`KeywordWorkspace.hpp`、`KeywordState.hpp` |
| `neo/concept/` | 契约：角色之间接口的 concept | `Text.hpp`、`Output.hpp`、`Segment.hpp`、`Match.hpp`、`Keyword.hpp` |
| `neo/resource/` | 资源、资源的词汇类型、诊断结果及词典快照 | `DictTrie.hpp`、`Trie.hpp`、`HMModel.hpp`、`KeywordResource.hpp`；`DictUnit.hpp`、`HMMState.hpp`；`Dag.hpp`、`TrieStats.hpp`；`DictSnapshot.hpp` |
| `neo/algorithm/` | 算法：policy、积木、分派、结果填充 | `Segmenter.hpp`、`MixSegment.hpp` 等分词 policy、`KeywordRank.hpp`、`PosTagging.hpp`、`Output.hpp` |
| `neo/util/` | 基础设施：不属于任何角色的支撑代码 | `Logging.hpp`、`Unicode.hpp`、`RuneString.hpp`、`StringUtil.hpp`、`FileIO.hpp`、`Platform.hpp`、`ByteOrder.hpp`、`Xxh64.hpp`、`ForceInline.hpp` |
| `neo/simd/` | 基础设施：按 `CPPJIEBA_SIMD` 选择实现的向量化内核，平台宏与 intrinsic 只出现在这里 | `SimdLevel.hpp`、`RuneMatch.hpp` |
| `neo/encoding/` | 需要额外 CMake 目标的可选模块 | `IcuCodec.hpp` |

- 公开与内部由命名空间区分，不由目录区分：`neo_cppjieba` 中不在 `detail` 的名字是公开 API，
  `neo_cppjieba::detail` 是内部实现。同一角色的公开类型和它的内部状态放在同一目录，例如 `Workspace` 与 `SegmentScratch`。
- 用户只 include 一级目录的头文件，每个门面头文件自足；二级目录的路径属于内部组织，可随重构调整。
- 门面以 `// IWYU pragma: export` 标注定义其公开类型的头文件，`misc-include-cleaner` 和 clangd 据此把门面视为
  这些类型的提供者；新增公开类型所在的头文件时同步标注。其余代码直接 include 所用名字的定义头文件，不依赖间接包含。
- 每个头文件自足：单独 include 即可编译，不依赖包含它的文件先引入的声明；模板中依赖实参的调用也须在本头文件的
  include 中找到声明，不靠实例化时的 ADL 补齐。`BUILD_TESTING` 构建的 `neo_header_check` 逐个编译头文件，
  `tools/check-headers.sh` 另查模板中的依赖查找。
- 一个头文件只承载一种角色，文件名与其中的主要类型同名。门面的拆包胶水（如 `detail::segment_candidates`）随门面留在一级。
- `Fwd.hpp` 前向声明全部公开类型，是公开名字的总表。定义这些类型的头文件 include 它，由编译器检查两处声明一致；
  `Tokens` 等别名模板需要完整定义，不在其中。
- 依赖方向：门面 → 算法 → 资源、上下文 → 值类型、配置。concept 文件依赖它约束的类型；`util/` 被各层共用，
  其中 `Unicode.hpp` 依赖 `value/Rune.hpp` 的码点类型。文件之间无环，二级目录的头文件不 include 门面。
- `simd/` 与 `util/` 同层，只依赖 `Config.hpp`、`value/` 和 `util/Logging.hpp`，由算法和 `util/` 调用。
  调用方总是调用内核的统一入口，入口按 `detail::simd::level` 选择不超过该级别的最高已实现版本，未开启时转发到标量实现；
  目录外的代码不检查平台宏。
- `NEO_FORCE_INLINE`（`util/ForceInline.hpp`）只用于测量过的逐字符热点函数：大翻译单元用完 GCC 的 `inline-unit-growth`
  预算后，这类函数会变成真实调用。宏是属性名，写进属性列表，如 `[[nodiscard, NEO_FORCE_INLINE]]`；罕见路径拆成单独的
  函数，不随强制内联复制到每个调用处。`test/cmake/CheckInlining.cmake` 列出全部使用者，在耗尽内联预算的探针中检查它们没有独立副本。
- 能归入某种角色的代码不放进 `util/`。例如默认分隔符是分词规则，放在 `algorithm/Separator.hpp`。
- 资源的词汇类型（`DictUnit`、`HMMState`）单独成头文件，上下文、契约和算法使用它们时不引入资源的实现。

## 资源

- 资源是带不变量的类。构造时完成全部校验，非法的词典、模型或数据文件通过 `check` 报告为用户错误。
- 成员函数只处理该类型自己的数据：查询（`find`、`find_dag`）、自身数据结构的编辑（`insert`、`erase`、`compact`）。
  需要组合多个资源的操作不放在资源类里。资源不解码文本：查询接收 rune 序列，解码是门面的事。
- 只能移动，不能隐式复制；需要复制时提供 `clone() const`，让整份数据的复制在调用点可见。
- 编辑是非 `const` 成员函数，库内不加锁。编辑入口在最外层用 `check` 校验公开参数，
  内部的数据结构操作只用 `assert_check` 维护不变量。
- 只在调用方确实需要单独持有或替换某个资源时，才把它提升为公开类型；否则留在 `detail` 中，保留重构空间。

## 配置

- 配置是选择行为的值：`enum class` 或可用指定初始化器构造的聚合 `struct`，例如 `TextRankOptions{.window = 1}`。
- 按大小传参：枚举和标量按值；含容器的（`TfIdfOptions`、`TextRankOptions` 的 `allowed_tags`）按 `const &`。
  对 16 字节以内、可平凡复制的值写 `const &` 只多一次间接访问。
- 调用配置随每次调用流入，门面只读取、不保存。提取器只持有 IDF 和停用词，选项由每次 `extract` 传入。
- 资源的构造配置随构造流入，校验后折算进资源自己的数据，例如 `UserWordWeightOption` 决定用户词的默认权重。
  它的类型放在 `options/`，不嵌在资源类里。
- 不用默认参数表达"使用默认配置"，调用方显式传入 `Options{}` 或具体取值。
- 取值检查与配置定义放在一起（`check_cut_mode`、`check_text_rank_options`、`check_user_word_weight_option`），
  由接收配置的门面或资源在入口处调用。
- 影响实例化的配置（容量类型、`SegmentationStyle`）是编译期配置，放在 `Config.hpp`。

## 上下文

- 上下文由调用方持有，保留缓冲容量以供复用；结果不借用上下文的缓冲，只借用调用方的原文。
- 同一时间只属于一个调用。并发或嵌套调用各用一个上下文。
- 公开的上下文类型保持不透明。内部状态放在 `detail` 中的普通 `struct` 里，经唯一访问点
  （`detail::segment_state`、`detail::keyword_state`）由门面拆包后传给算法。
- 算法只接收它实际读写的缓冲或 `detail` 状态结构，不接收整个公开上下文对象。状态按使用它的算法分组
  （`CandidateTable`、`TextRankScratch`、`SelectionScratch`），只读的输入以 `const &` 或 `std::span` 传入。

## 算法：policy 与自由函数

一组算法满足下列至少一条时写成 policy，否则写成 `namespace detail` 中的自由函数：

1. 会作为模板参数，在编译期于多种实现间选择。
2. 是一族可互换的实现，提供同一组签名，可以用一个 concept 约束。
3. 需要整组参数化或共享关联类型和常量，例如 `Policy<Style>::cut_into`、`Policy::Scratch`。

只有一个实现、直接按名字调用、也没有关联类型的类，应写成自由函数，不为它单独包一层类。

policy 的规则：

- 无状态：不含数据成员和静态可变状态，所有输入来自参数。
- 每一族 policy 有对应的 concept，每个实现旁边用 `static_assert(Concept<Impl>)` 检查是否满足。
- policy 负责选择和组合。被多个 policy 共用的积木（路由构建、Viterbi、DAG 遍历、TextRank 迭代）写成自由函数，
  policy 之间不靠继承或 friend 复用代码。
- 共用一个方法名约定：写入调用方输出的入口叫 `cut_into` / `extract_into`，返回新容器的便捷入口叫 `cut` / `extract`。

## 契约（concept）

concept 是角色之间接口的唯一书面定义，下列情况必须使用：

- 模板接受外部类型：输入（`StringLike`、`CharType`）、输出（`OutputValue`、`TaggedOutputValue`）、
  回调（`ViewVisitor`、`WordEmitter`、`MatchVisitor`、`SourceTextParser`）。接收泛型回调的 concept 用一个具体签名作原型检查，
  例如 `RouteEdges` 以函数指针代表发词回调。
- 一族 policy 的共同签名（见上一节）。
- 编译期配置的取值范围（`CapacityInteger`、`LogConfiguration`）。

规则：

- 用 concept 和 `requires` 子句替代 SFINAE、`enable_if` 和无约束的模板参数，让错误在调用点给出可读的诊断。
- concept 只描述语法要求；语义要求（复杂度、是否可重入、坐标系）写在 concept 上方 1–3 行注释中。
- concept 放在 `concept/`，按接口分文件。用户需要书写的（`StringLike`、`CharType`）在 `neo_cppjieba`，
  只用于约束的在 `neo_cppjieba::detail`。两类例外留在原处：约束编译期配置取值的（`CapacityInteger`、`LogConfiguration`）
  与被约束的设置一起留在 `Config.hpp`；只约束单个文件内部模板参数的（如 `Logging.hpp` 的 `CheckCondition`）留在该文件。
- 一个 concept 只对应一个角色的一个接口。只有一个满足者时直接写具体类型，不为它定义 concept。
- 公开 concept 的变更等同于公开 API 变更。

## 门面

- 门面持有"总是一起使用、彼此有约束"的资源。例如 `Jieba` 持有词典和 HMM 模型，用户词权重依赖主词典的频率总和。
- 成员函数只做四件事：用 `check` 校验公开参数、解码输入、调用算法、按输出类型填充结果。算法本身不写在门面里。
- 彼此独立的资源由各自的门面持有，其他门面在调用时以 `const &` 传入，不长期保存引用。
  例如提取器持有 IDF 和停用词，`Jieba` 由每次 `extract` 传入。
- 查询入口为 `const`；编辑入口为非 `const`，要求调用方独占访问。

## 访问控制与 friend

friend 只在两种情况下使用：

1. 门面与它专属的公开值类型之间，例如 `Tokens` 对 `Jieba` 开放构造。
2. 一个公开类型对唯一的 `detail` 访问函数开放，例如 `DictSource` 对 `detail::with_source_text`，
   `Workspace`、`KeywordWorkspace` 对 `detail::segment_state`、`detail::keyword_state`。

一个类型不为多个算法类分别开 friend；需要访问上下文内部缓冲的多处算法，经由唯一访问点取得 `detail` 状态结构，
再按需传参。

## 速查

```text
只读写某个类型自己的数据?                          → 该类型的成员函数（资源）
组合多个资源、配置、上下文?
├─ 作为模板参数 / 同签名的一族 / 需整组参数化?      → policy：无状态 + concept + static_assert
└─ 否                                              → namespace detail 自由函数
选择行为的值?                                      → 配置：随调用显式传入，标量按值、含容器的按 const &
资源构建时的选择?                                  → 配置放 options/，构造时校验并折算进资源
每次调用的可变缓冲?                                → 上下文：调用方持有，内部状态放 detail struct
模板接受的外部类型或一族实现的共同签名?            → 契约：concept
公开入口?                                          → 门面：校验、解码、转发、填充
需要私有访问?                                      → 门面专属值类型，或唯一的 detail 访问函数
新文件放在哪里?                                    → 角色对应的目录；用户 include 的入口才放一级目录
```

## 现状与待调整处

下列调整已按本约定完成：

- 分词 policy 共用签名 `cut_into(dict, model, runes, out, scratch)`，由 `concept/Segment.hpp` 的
  `SegmentPolicy` 约束，每个实现旁有 `static_assert`；`CppSegmentFamily` 与 `StyledSegmentFamily` 由
  `SegmentFamily` 约束，`algorithm/Segmenter.hpp` 经 `ActiveSegmentFamily` 分派。不用的资源参数不具名，没有运行时开销；
  返回新容器的便捷入口是 `algorithm/SegmentCommon.hpp` 的 `detail::cut<Policy>(dict, model, runes)`。
- `KeywordWorkspace` 只经 `detail::keyword_state` 一个访问点暴露 `detail::KeywordState`；候选收集、TF-IDF、TextRank
  和 Top-N 选择是 `algorithm/KeywordRank.hpp` 中的自由函数，`TextRankExtractor` 不再包含算法。
- IDF 表和停用词是 `detail::IdfTable`、`detail::StopWords` 资源类；`HMModel` 与提取器一样只能移动，复制用 `clone()`。
- 公开的 Unicode 工具收敛为 `Utf` 门面（`is_valid`、`decode`、`encode`）。
- 头文件按"目录"一节的规则重新组织：一级目录只剩门面、`Config.hpp` 和新增的 `Fwd.hpp`，原 `detail/` 按角色拆入二级目录。
- 配置与资源分开：提取器只持有 IDF 和停用词，`TfIdfOptions`、`TextRankOptions` 随每次调用按 `const &` 传入；
  `UserWordWeightOption` 移到 `options/`，`DictTrie` 不再用默认参数表达默认配置；各配置的取值检查与定义放在一起。
- `Workspace` 与 `KeywordWorkspace` 一样经唯一访问点暴露 `detail::SegmentState`；`KeywordState` 按算法分组，
  关键词算法只接收各自读写的组。上下文只依赖资源的词汇头 `HMMState.hpp` 和 `util/RuneString.hpp`。
- 资源与值类型只处理自己的数据：`Trie`、`DictTrie` 去掉解码字符串的 `find` / `find_dag` 重载，`WordRange` 去掉 `to_string`。
- 回调参数都有 concept（`WordEmitter`、`SegmentVisitor`、`RouteEdges`、`MatchVisitor`、`SourceTextParser` 等），
  算法积木的参数按统一签名顺序排列。

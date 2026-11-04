# 架构

本文说明头文件的分层、依赖方向和扩展方式，供修改库内部时参考。使用方式见 [使用指南](usage.md)；
资源、算法、配置、上下文、契约和门面的划分标准见 [设计原则](design.md)。

## 分层

目录按角色划分，规则见 [设计原则·目录](design.md#目录)。下表按依赖方向从上往下排列，上层 include 下层；
契约随它约束的接口出现在各层，基础设施被各层共用，二者不受此顺序限制。文件之间无环，二级目录的头文件不 include 门面：

| 层 | 头文件 | 职责 |
| --- | --- | --- |
| 门面 | `Jieba.hpp`、`KeywordExtractor.hpp`、`Utf.hpp` | 公开入口：校验、解码、调用算法、按输出类型填充结果 |
| 分派 | `algorithm/Segmenter.hpp` | `CutMode` 校验；`CutMode` × `SegmentationStyle` → policy 族，唯一的分支点 |
| 分词算法 | `algorithm/MPSegment.hpp` → `MixSegment.hpp` → `QuerySegment.hpp`；`FullSegment.hpp`、`HMMSegment.hpp`、`StyledSegment.hpp` | 在 rune 序列上产出 `WordRange`，不接触原文编码 |
| 关键词算法 | `algorithm/KeywordRank.hpp` | 候选收集、TF-IDF、TextRank、Top-N 选择，作用于 `KeywordState` |
| 算法积木与结果填充 | `algorithm/SegmentCommon.hpp`、`Separator.hpp`、`CharClass.hpp`、`Output.hpp`、`PosTagging.hpp` | 分隔符切分、结果校验、字符分类；把 rune 范围填充为调用方的输出类型 |
| 上下文 | `workspace/Workspace.hpp`、`SegmentScratch.hpp`、`KeywordWorkspace.hpp`、`KeywordState.hpp` | 调用方持有的可复用缓冲；公开的工作区不透明 |
| 资源 | `resource/DictTrie.hpp`、`Trie.hpp`、`HMModel.hpp`、`KeywordResource.hpp`；词汇头 `DictUnit.hpp`、`HMMState.hpp` | 词典、模型、IDF 表和停用词：只能移动，复制用 `clone()`；不解码文本 |
| 值类型与配置 | `value/`、`options/`、`Config.hpp`、`Fwd.hpp` | 公开的输入、结果与选项，不含算法 |
| 契约 | `concept/Text.hpp`、`Output.hpp`、`Segment.hpp`、`Match.hpp`、`Keyword.hpp` | 输入与加载、输出与访问者、policy 族与分词回调、词典匹配、标注回调的 concept；依赖它约束的类型 |
| 基础设施 | `util/Logging.hpp`、`Unicode.hpp`、`RuneString.hpp`、`StringUtil.hpp`、`FileIO.hpp`、`Platform.hpp` | 诊断、编解码、码点串哈希、解析、文件读取；被各层共用 |
| 向量化内核 | `simd/SimdLevel.hpp`、`RuneMatch.hpp`、`Viterbi.hpp` | `CPPJIEBA_SIMD` 选定的指令集级别；分隔符扫描和 HMM Viterbi 等内核按级别选择实现，平台宏与 intrinsic 只出现在 `simd/` |

`Dag.hpp` 仅供 `Trie::find_dag` 的诊断和对照测试使用，分词路径不物化 DAG。

## 一次分词的数据流

```text
input ──detail::as_view──▶ source ──decode_with_offset_into──▶ runes + offsets   (SegmentState::decoded)
runes ──detail::segment_into(mode)──▶ WordRange[]                         (SegmentState::ranges, scratch)
WordRange[] + offsets ──fill_output / emit_tokens / fill_tagged_output──▶ 调用方的输出类型
```

- 算法只看 rune 坐标；原文坐标由门面在最后一步通过 offsets 换算，所以三种编码共用同一套算法实例。
- 词性标注在分词之后按结果查表，`Tagging::None` 的实例化不含任何标注代码。

## 两个维度的分派

- **运行时**：`CutMode` 由调用方逐次选择，在 `detail::segment_into` 中经一个 `switch` 落到当前 policy 族的
  `Precise<hmm>`、`Search<hmm>`、`Full` 或 `Hmm`。MP 与 MIX_NO_HMM 是同一条路由。
- **编译期**：`compile_config::segmentation_style` 选择 CPP / RUST / PYTHON 规则。
  `ActiveSegmentFamily` 在 `CppSegmentFamily` 与 `StyledSegmentFamily`（`StyledSegment.hpp` 的参考实现）之间选择；
  两族都由 `SegmentFamily` concept 检查，未选中族的函数体不实例化。少数无法上提的细节（Viterbi 的平局规则、权重归一化、HMM 前的切块）
  仍在各自算法内部以 `if constexpr` 区分。

门面和关键词提取不感知风格。新增一种风格时，新增一个满足 `SegmentFamily` 的族并在 `ActiveSegmentFamily` 中选择，
再处理上述细节处的分支。

所有分词 policy 共用签名 `cut_into(dict, model, runes, out, scratch)`；不用的资源参数不具名，内联后没有开销。

## CPP 风格的算法组合

```text
for_each_separated（按默认分隔符切段，分隔符单独成词）
  └─ 每段：mp_build_route_from_matches（直接遍历 Trie 匹配做反向 DP，不建 DAG）
       ├─ MP / MIX_NO_HMM：mp_emit_route
       ├─ MIX：mix_emit_route<true>，单字串交给 hmm_emit_one_segment
       └─ SEARCH：同一路由，建路由时记录 2/3 字词表成员，发词前补出子词
FULL：for_each_separated + 每个起点的全部匹配
HMM：for_each_separated + hmm_emit_one_segment
```

算法通过回调 `emit(WordRange)` 交付局部坐标的词，由外层加上段偏移写入结果，因此 MIX 与 SEARCH
共用同一个路由遍历，只是回调不同。

## 内存与并发

- 所有分词入口是 `const`；`DictTrie`、`HMModel` 只在显式编辑时改变。`add_word`、`remove_word`、`compact`
  是非 `const` 操作，库内不加锁，由调用方保证独占访问；`clone()` 支持写时复制。
- 每次调用的可变状态都在调用方的 `Workspace` 中，内部是 `detail::SegmentState`：解码缓冲、结果缓冲和 `SegmentScratch`，
  只能经 `detail::segment_state` 取得。
  `SegmentScratch` 的各缓冲保留最高水位，算法只覆盖本次读取的行，不做整体清零。
- `KeywordWorkspace` 不透明，内部状态是 `detail::KeywordState`（内嵌一个 `Workspace`），只能经 `detail::keyword_state`
  取得；关键词提取不与分词共享可变状态。`KeywordState` 按算法分组：候选收集写 `CandidateTable`，TF-IDF 与 TextRank
  更新其中的权重，TextRank 另用 `TextRankScratch`，Top-N 选择用 `SelectionScratch`。
- 配置不进入任何对象的长期状态：`CutMode`、`TfIdfOptions`、`TextRankOptions` 都随调用传入；只有资源的构造配置
  （`UserWordWeightOption`）在构造时折算进 `DictTrie`。

## 公开边界

公开 API 是命名空间 `neo_cppjieba` 中不在 `detail` 的名字：门面（`Jieba`、`TfIdfExtractor`、`TextRankExtractor`、`Utf`）、
值类型（含 `PosTag` 和 `pos::` 常量）、配置、不透明的工作区和输入 concept；`Fwd.hpp` 列出全部公开类型。
用户只 include 一级目录的头文件，二级目录的路径属于内部组织，不保证兼容。`neo_cppjieba::detail` 中的名字都是内部实现。

测试内部实现的用例通过 `using namespace neo_cppjieba::detail;` 或 `detail::` 限定显式声明这一点，并直接 include
被测的二级头文件；公开名字经门面或被测头文件取得。只测公开接口的用例只 include 一级目录的头文件。

一处例外：`util/Logging.hpp` 的 `check`、`assert_check` 仍在 `neo_cppjieba` 中，
因为错误通过公开的 `LogConfig::Exception` 报告。它们随 [Roadmap](../ROADMAP.md#日志与错误处理) 的日志改造一起调整。

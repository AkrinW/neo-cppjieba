# 词典快照格式

词典快照是 `DictTrie` 的预编译形式：加载时跳过文本解析和逐词建树，直接按层序布局构造 Trie。
公开入口是 `Jieba::encode_dict_snapshot` 和 `Jieba::from_snapshot`，用法见 [词典快照](usage.md#词典快照)。
实现位于 `include/neo/resource/DictSnapshot.hpp`，层序布局与校验位于 `Trie.hpp` 和 `DictTrie.hpp`。

## 兼容性原则

- 任一版本的库加载快照，结果与该版本从同一份源文本加载完全一致：词、权重、词性、DAG 和统计量都相同。
- 快照是派生产物。源词典和生成工具是权威来源，快照被拒绝加载时，重新生成即可恢复。
- 快照只存规范的逻辑数据：层序 Trie 形状、原始整数频率、词性字符串和整数统计量。
  运行时存储（filter、compact 划分、hash 表、`TrieNodeId` 宽度）在加载时重建，因此快照跨存储改动、配置和架构通用。
- 快照不保存权重，加载时按当前规则从频率推导，因此权重公式和 `SegmentationStyle` 的变化与快照版本无关。
- 已发布的每个格式版本、section 编码和校验和类型都永久保留解码分支（`switch` 中的一个 `case`）。

## 版本维度

| 维度 | 标识 | 不匹配时 |
| --- | --- | --- |
| 容器格式 | header 中的 format version | 已知版本走各自的解码器；更新的版本报错 |
| section 编码 | section 表中的 encoding | 已知编码走各自的解码器；未知编码：必需 section 报错，可选 section 跳过 |
| 未知 section | section 表中的 kind | 必需报错，可选跳过 |
| 内容规则 | Meta 中的 content rules（`kDictContentRules`） | 列在 `snapshot_content_rules_supported` 中的值可读，其余报错并提示重新生成 |
| 校验和 | header 中的 checksum kind | 已知类型校验；未知类型报错 |

## 布局（format version 1）

所有整数为小端，按字段逐个读取，不要求对齐。

Header，48 字节：

| 偏移 | 类型 | 字段 |
| --- | --- | --- |
| 0 | char[8] | magic `NEOJBSNP` |
| 8 | u32 | format version = 1 |
| 12 | u32 | header size，至少 48；更大的 header 中多出的字节被忽略 |
| 16 | u64 | 文件总字节数 |
| 24 | u64 | XXH64（seed 0），覆盖 `[header size, 文件末尾)` |
| 32 | u32 | checksum kind = 1（XXH64） |
| 36 | u32 | section 数量 |
| 40 | u32 | section 表项大小，至少 24；更大的表项中多出的字节被忽略 |
| 44 | u32 | 保留，写 0 |

前 16 字节（magic、format version、header size）在所有版本中保持不变，任何版本都能识别文件并报告具体版本。

Section 表紧接 header，每项：`u32 kind`、`u16 encoding`、`u16 flags`（bit 0 = 必需）、`u64 offset`（自文件起始）、`u64 size`。

| kind | 名称 | encoding 1 内容 |
| --- | --- | --- |
| 1 | Meta | `u32 content rules`、`u8 option`（0 Min，1 Median，2 Max）、`i64 freq sum`、`i32 min`、`i32 max`、`i32 median` |
| 2 | Tags | `u64 count`，每个词性：`u8 长度` + 字节 |
| 3 | Trie | `u64 nodes`、`u64 edges`、`u64 values`；`u32 fanout[nodes]`；每条边 `u32 rune` + `u8 flags`（bit 0 有子节点，bit 1 有值）；每个值 `i32 freq` + `u32 tag index` |
| 4 | UserSingleRunes | `u64 count`，`u32 rune[count]`，严格递增 |

层序布局：节点按广度优先编号，根为 0，每个节点的边按 rune 严格递增；第 k 条带子节点的边指向节点 k，子节点 id 无需存储。
值按边的顺序排列，频率 0 表示用户词默认权重。相同的词典总是得到相同的字节。

## 演进规则

- **改动 section 字段**：给该 kind 增加新 encoding，writer 改写新 encoding。旧 encoding 的解码器保留，并补齐新字段的取值；
  无法补齐时，在旧解码器中报错并说明需要重新生成。`snapshot_encoding_supported` 与对应的解码 `switch` 同步增加分支。
- **新增数据**：新增 section kind。旧读者能忽略时标为可选，否则标为必需。kind 编号永不复用。
- **改动 header 或 section 表结构**：提升 format version，增加新的 `locate_snapshot_vN_sections`，旧版本的 `case` 保留。
- **改动解析、解码或优先级规则**（`DictTrie` 文本加载的结果会变）：提升 `kDictContentRules`。
  若旧规则下的快照在新规则下仍得到相同词典，把旧值保留在 `snapshot_content_rules_supported` 中；否则移除，旧快照需重新生成。
- **改动权重公式或 `SegmentationStyle`**：快照无需处理。
- 格式版本随首个包含它的 release 冻结。发布之前的布局调整仍属于该版本，同时用 golden 源词典（WordWeightMedian）
  重新生成它的 golden 文件；发布之后的任何改动都按以上规则演进。
- 每个发布过的格式版本在 `test/testdata/snapshot/` 下保留一份 golden 文件，永不重新生成；
  `DictSnapshotTest.FormatVersion1GoldenStillMatchesTextLoading` 及后续版本的同类测试保证它们持续可读。

## 加载与安全

`Jieba::write_dict_snapshot` 经由 `replace_file_atomically`（`util/FileIO.hpp`）发布快照：写入同目录的临时文件，
刷到存储后重命名替换目标，再同步目录，读者只会看到旧文件或完整的新文件。
`SnapshotSource::file` 在加载时先把文件完整读入自有内存（沿用 `FileBuffer` 的读前读后元数据检查），再校验和解码，
因此加载后文件被修改、截断或删除都不会影响词典；读取过程中的并发修改由校验和发现。
所有结构错误都通过 `check` 报告为可处理的错误：越界计数、未知 flag、乱序 rune、不可达节点、超出 `TrieNodeId` 范围的节点数、
不一致的统计量等。`test/fuzz/dict_snapshot_fuzzer.cpp` 在每次变异后重算校验和，以覆盖校验和之后的解码路径。

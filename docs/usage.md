# C++ 使用指南

[快速开始](../README.md#快速开始) · [构建与安装](building.md) · [应用示例](../examples/README.md)

入口类是 `neo/Jieba.hpp` 中的 `neo_cppjieba::Jieba`。用户代码只需 include `include/neo/` 一级目录的头文件，
二级目录是内部实现：

| 头文件 | 内容 |
| --- | --- |
| `Jieba.hpp` | 分词器 `Jieba`、结果类型和 `DictSource` |
| `KeywordExtractor.hpp` | TF-IDF 和 TextRank 关键词提取 |
| `Utf.hpp` | UTF 校验与编解码 |
| `Config.hpp` | 编译期配置 |
| `Fwd.hpp` | 公开类型的前向声明，供只在声明中用到类型名的头文件使用 |

构造 `Jieba` 时依次传入主词典、HMM 模型和用户词典的路径，用户词典可传 `""`。
相对路径基于进程的工作目录，Windows 上的路径按 UTF-8 解释。文件格式见 [词典说明](../dict/README.md)，
从内存加载和多个用户词典见 [从内存加载词典](#从内存加载词典)。

## 输入文本

| 字符类型 | 编码 |
| --- | --- |
| `char`、`char8_t`、字节缓冲 | UTF-8 |
| `char16_t` | UTF-16 |
| `char32_t` | UTF-32 |
| `wchar_t` | 按平台宽度使用 UTF-16 或 UTF-32 |

字符串、视图、字符数组和连续范围都按自身长度读取：字符串字面量去掉末尾的 NUL，内嵌的 NUL 作为普通字符保留。
借用型入口的结果引用输入，使用结果期间输入需保持有效。

从 C 接口或其他缓冲区拿到字符指针时，用已知长度构造视图。以下片段中
`buffer`、`byte_count` 由调用方提供，`jieba` 是已创建的分词器：

```cpp
#include <span>
#include <string_view>

const auto text = std::string_view{buffer, byte_count};
const auto tokens = jieba.cut(text, neo_cppjieba::CutMode::MIX);
const auto words = jieba.cut_strings(std::span<const char>{buffer, byte_count}, neo_cppjieba::CutMode::MIX);
```

从 1.1.x 的原始字符指针调用迁移时，也按这种方式改写。
GBK / GB18030 输入先通过 [编码适配器](encoding.md) 转换为 UTF-8。

## 分词模式

| `CutMode` | 说明 |
| --- | --- |
| `MIX` | 词典最大概率路径结合 HMM，识别未登录词 |
| `MIX_NO_HMM` / `MP` | 词典最大概率分词 |
| `SEARCH` | 在混合分词基础上补充较短词，适合搜索索引 |
| `SEARCH_NO_HMM` | 基于词典分词补充搜索词，关闭 HMM |
| `FULL` | 输出词典匹配，结果可以重叠 |
| `HMM` | 使用隐马尔可夫模型分词 |

`Config.hpp` 中的 `compile_config::segmentation_style` 可选 `CPP`、`RUST`、`PYTHON`，默认 `CPP`。
三套规则分别参考 CppJieba、jieba-rs 0.11.0 和 jieba 0.42.1；词典加载及用户词典采用本库的接口约定。
实现间的输出比较见 [Rust 基准](../benchmark/RUST.md) 和 [Python 基准](../benchmark/PYTHON.md)。

## 选择输出形式

| 入口 | 结果和适用场景 |
| --- | --- |
| `cut(text, mode)` | 借用原文的 token 视图及位置，适合即时消费 |
| `cut_owned(string, mode)` | 持有原文和位置，适合保存结果或接收临时字符串 |
| `cut_strings(text, mode)` | 每个词独立存储的字符串数组 |
| `cut_with_workspace(text, mode, workspace)` | 复用工作区，返回借用原文的结果 |
| `cut_into(text, mode, out, workspace)` | 复用调用方的位置数组或字符串数组 |
| `cut_each(text, mode, emit, workspace)` | 通过返回 `void` 的回调逐词接收 token |
| `cut_runes(runes, mode)` / `cut_runes_into(runes, mode, out, workspace)` | 对已校验的 Unicode 标量分词，输出 `WordRange` |

`cut_runes` 的输入可用 `neo/Utf.hpp` 的 `Utf` 门面准备：`Utf::decode(text)` 返回码点数组，
`Utf::encode<CharT>(runes)` 编码回字符串，`Utf::is_valid(text)` 只做校验。

`token.word` 指向原文，请在原文修改或销毁之前读取。`cut_owned` 的结果被移动后，需重新获取 token 视图。
每个词需要单独保存时，使用 `cut_strings`。完整的所有权示例见 [owned_results.cpp](../examples/owned_results.cpp)。

## 词性标注

`cut`、`cut_with_workspace`、`cut_owned`、`cut_strings` 和 `cut_each` 接受模板参数 `Tagging`，
默认 `Tagging::None`。传入 `Tagging::Pos` 时，分词结果与同一模式相同，每个词另带一个 `PosTag`：

| 入口 | 带词性的结果 |
| --- | --- |
| `cut<Tagging::Pos>(text, mode)` / `cut_with_workspace<Tagging::Pos>(...)` | 借用原文的 `TaggedTokenView`，含 `word`、`position`、`tag` |
| `cut_owned<Tagging::Pos>(string, mode)` | 持有原文的带词性结果 |
| `cut_strings<Tagging::Pos>(text, mode)` | `TaggedWord` 数组，每项含独立的 `word` 和 `tag` |
| `cut_each<Tagging::Pos>(text, mode, emit, workspace)` | 回调逐词接收 `TaggedTokenView` |
| `cut_into(text, mode, out, workspace)` | 由输出类型决定：`std::vector<TaggedPosition>` 或 `std::vector<TaggedWord<CharT>>` |
| `lookup_tag(word)` | 直接查询一个完整词的词性；也接受 `std::span<const Rune>` |

返回类型 `Tokens<CharT, Tagging::Pos>` 也可写作 `TaggedTokens<CharT>`。

```cpp
using neo_cppjieba::CutMode, neo_cppjieba::Tagging;
for (const auto token : jieba.cut<Tagging::Pos>("我来到北京清华大学，使用iPhone6", CutMode::MIX)) {
    std::println("{}/{}", token.word, token.tag.to_string_view());
}
// 我/r 来到/v 北京/ns 清华大学/nt ，/x 使用/v iPhone6/eng
```

词性取自主词典或用户词典的第三列。词典中没有该词，或用户词典未给出词性时，沿用 CppJieba 的规则：
从词首开始统计 ASCII 字符，统计数达到词长一半即停止；没有 ASCII 为 `x`，全为数字为 `m`，否则为 `eng`。
因此单字词总是 `x`，`3.14` 为 `eng`。三种 `segmentation_style` 使用同一套词性规则。
Python jieba 的 `posseg` 另有一个词性 HMM 模型，本库没有实现它；`dict/pos_dict/` 中的同名数据来自上游，当前未使用。

`PosTag` 最多 4 个 ASCII 字符，按值存储，可与字符串或 `neo_cppjieba::pos` 中的常量比较，
例如 `token.tag == neo_cppjieba::pos::ns` 或 `token.tag == "ns"`。缩写含义见 [词性标记表](pos_tags.md)。

| CppJieba / Python jieba | neo-cppjieba |
| --- | --- |
| `jieba.Tag(s, words)` | `jieba.cut_strings<Tagging::Pos>(s, CutMode::MIX)` |
| `jieba.LookupTag(word)` | `jieba.lookup_tag(word)` |
| `jieba.posseg.lcut(s)` / `lcut(s, HMM=False)` | `cut_strings<Tagging::Pos>(s, CutMode::MIX)` / `CutMode::MIX_NO_HMM` |

只有 `Tagging::Pos` 的调用才查询词性，默认的 `Tagging::None` 没有额外开销。

## 关键词提取

可选头文件 `neo/KeywordExtractor.hpp` 提供两种提取器。提取器只持有 IDF 和停用词等资源，`const Jieba &` 和选项
在每次调用时传入，因此一个提取器可以搭配任意 `Jieba` 实例和选项使用，包括增删词后 `clone()` 出的实例：

| 提取器 | 打分方式 | 需要的文件 |
| --- | --- | --- |
| `TfIdfExtractor(idf_path, stop_words_path)` | 词在本文中的出现次数 × 该词的 IDF；IDF 文件中没有的词取文件内 IDF 的平均值 | [`idf.utf8`](../dict/idf.utf8)、[`stop_words.utf8`](../dict/stop_words.utf8) |
| `TextRankExtractor(stop_words_path)` | 在窗口内共现的词之间连边，按类似 PageRank 的迭代打分，最高分归一为 1 | [`stop_words.utf8`](../dict/stop_words.utf8) |

两者先用 `CutMode::MIX` 分词，丢弃单字词和停用词，再对剩下的不同词打分，按分数从高到低取前 `top_n` 个；
分数相同时先出现的词在前。结果是 `std::vector<Keyword<CharT>>`，每项含独立的 `word`、`weight`、
`tag`（与 `lookup_tag` 相同）和该词每次出现的 `positions`（坐标与分词结果一致）。

```cpp
#include "neo/KeywordExtractor.hpp"

using namespace neo_cppjieba;
const auto extractor = TfIdfExtractor{"dict/idf.utf8", "dict/stop_words.utf8"};
const auto text = std::string_view{"我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。"};
for (const auto &keyword : extractor.extract(jieba, text, 5, TfIdfOptions{})) {
    std::println("{} {:.4f} @{}", keyword.word, keyword.weight, keyword.positions.front().source.begin);
}
// CEO 11.7392 @93 / 升职 10.8562 @72 / 加薪 10.6426 @78 / 手扶拖拉机 10.0089 @21 / 巅峰 9.4940 @111
```

### 按词性过滤

`TfIdfOptions` 和 `TextRankOptions` 的 `allowed_tags` 对应 Python jieba 的 `allowPOS`：列表非空时，
只保留词性在列表中的候选词；默认为空，保留全部候选词，与 CppJieba 相同。
`python_textrank_tags`（`ns`、`n`、`vn`、`v`）是 Python jieba `textrank` 的默认值。

```cpp
using namespace neo_cppjieba;
const auto nouns = TfIdfOptions{.allowed_tags = {pos::n, pos::nz, pos::vn}};
const auto keywords = extractor.extract(jieba, text, 5, nouns);
// 手扶拖拉机/n 巅峰/n 拖拉机/n 人生/n 学院/n

const auto textrank = TextRankExtractor{"dict/stop_words.utf8"};
const auto python = TextRankOptions{.allowed_tags = {python_textrank_tags.begin(), python_textrank_tags.end()}};
const auto ranked = textrank.extract(jieba, text, 5, python);
```

TF-IDF 逐词独立打分，过滤只移除不符合的词，保留下来的词分数不变。TextRank 把被过滤的词当作停用词：
它们既不进入词图，也不占窗口位置，其余词重新打分。
词性取自词典，例如主词典把"加薪"标为 `nr`；Python jieba 使用 `posseg` 的词性模型，两者的过滤结果会有出入。

连续处理文本时，用 `extract_into(jieba, text, top_n, options, out, keyword_workspace)` 复用 `KeywordWorkspace`
和输出数组，同一工作区可在两种提取器之间共用。选项只在本次调用中读取；
`TextRankOptions` 在每次调用入口检查 `damping` 是否在 [0, 1] 内。`TextRankOptions{}` 的 `window`、`iterations`、
`damping` 为 5、10、0.85，对应 CppJieba 同样随每次 `Extract` 传入的 `span`、`rankTime` 和固定的阻尼系数。

结果与 CppJieba 的 `KeywordExtractor` / `TextRankExtractor` 一致，分数逐位相同，只有两处细节有别：
同分词本库固定先出现者在前，CppJieba 未规定顺序；TextRank 中没有共现边的词（例如全文只剩一个候选词），
本库返回该词本身，CppJieba 返回空的 `word`，分数都是 0。
Python jieba 的 `extract_tags` / `textrank` 采用另一套规则（词频归一化、IDF 中位数、按词性过滤等），分数与本库不同。

| CppJieba | neo-cppjieba |
| --- | --- |
| `KeywordExtractor(dict, hmm, idf, stop_words, user_dict)` | `Jieba{dict, hmm, user_dict}` 加 `TfIdfExtractor{idf, stop_words}` |
| `TextRankExtractor(dict, hmm, stop_words, user_dict)` | `Jieba{dict, hmm, user_dict}` 加 `TextRankExtractor{stop_words}` |
| `KeywordExtractor::Extract(s, words, topN)` | `extract(jieba, s, top_n, TfIdfOptions{})` |
| `TextRankExtractor::Extract(s, words, topN, span, rankTime)` | `extract(jieba, s, top_n, TextRankOptions{.window = span, .iterations = rankTime})` |
| `Word::offsets` | `Keyword::positions[i].source.begin` |

## 从内存加载词典

`DictSource`（由 `neo/Jieba.hpp` 提供）描述一份词典或模型数据：`DictSource::file(path)` 在加载时读取文件；
`DictSource::memory(text, name)` 直接解析内存中的 UTF-8 文本，适合把词典编译进程序（如 `#embed`、资源段），
或在 WASM 这类难以访问文件系统的环境中使用。内存文本的格式与对应文件相同，`name` 用于错误信息。
内存文本只需在构造期间有效，构造完成后即可释放。

```cpp
#include "neo/Jieba.hpp"

using namespace neo_cppjieba;
extern const std::string_view embedded_dict, embedded_hmm, embedded_user_dict;

const auto user_dicts = std::array{DictSource::memory(embedded_user_dict, "user.dict")};
const auto jieba = Jieba{DictSource::memory(embedded_dict, "jieba.dict"), DictSource::memory(embedded_hmm),
                         user_dicts};
```

用户词典可以有多个，按顺序加载，同一个词以最后一次出现为准，对应 CppJieba 用 `;` 分隔的多个用户词典路径。
文件与内存来源可以混用。`TfIdfExtractor`、`TextRankExtractor` 同样接受 `DictSource`：

```cpp
const auto extractor = TfIdfExtractor{DictSource::memory(embedded_idf), DictSource::memory(embedded_stop_words)};
```

## 词典快照

主词典和用户词典可以预先编译成快照，加载时跳过文本解析和逐词建树。
使用内置词典时，`Jieba` 的构造从约 85 ms 降到约 19 ms（含 HMM 模型；发布构建，文件已在页缓存中，
见 [`load_benchmark`](../benchmark/README.md)）。

```cpp
#include "neo/Jieba.hpp"

using namespace neo_cppjieba;

// 生成并原子地写出：与 Jieba{dict, model, user_dicts} 构建的词典相同
const auto user_dicts = std::array{DictSource::file("user.dict.utf8")};
Jieba::write_dict_snapshot(DictSource::file("jieba.dict.utf8"), user_dicts, "jieba.snapshot");

// 加载：HMM 模型仍从 DictSource 读取
const auto jieba = Jieba::from_snapshot(SnapshotSource::file("jieba.snapshot"), DictSource::file("hmm_model.utf8"));
```

- 从快照加载的实例与从生成它的源词典加载的实例分词、词性和权重完全相同；权重在加载时按当前版本的规则推导。
- `SnapshotSource::file(path)` 在加载时把整个文件读入实例自己的内存；`SnapshotSource::memory(bytes, name)`
  的数据只需在加载期间有效。加载完成后实例与文件和内存再无关联，之后修改、截断或删除文件都没有影响；
  文件若在加载过程中被修改，校验和会发现并报错。
- `write_dict_snapshot` 先把快照写入目标同目录下的临时文件并刷到存储，再重命名替换目标：其他进程只会读到旧文件
  或完整的新文件，写入失败时旧文件保持不变。新文件使用默认权限（受 umask 影响）；进程在重命名前退出可能留下
  以 `.` 开头、`.tmp` 结尾的临时文件。需要快照字节（例如嵌入程序）时，使用 `encode_dict_snapshot`。
- 新版本的库可以读取旧版本写出的快照。如果某次升级改变了词典文本的解析规则，加载旧快照会报错，并提示从源词典重新生成。
  格式与兼容规则见 [词典快照格式](snapshot.md)。
- 损坏、截断、版本过新或不是快照的数据通过 `LogConfig::Exception` 报告，错误信息包含来源名称。
- 运行时增删词、`clone()` 和 `compact()` 对快照加载的实例同样适用。

## 运行时增删词

`Jieba` 可在构建后就地加词、删词，对应 CppJieba 的 `InsertUserWord` 和 Python jieba 的 `add_word` / `del_word`：

```cpp
auto jieba = Jieba{dict_path, model_path, ""};
jieba.add_word("杭研", PosTag{"nz"});                // 默认用户词权重
jieba.add_word_with_freq("云计算", 500, PosTag{"n"}); // 频率必须为正
const bool removed = jieba.remove_word("大厦");        // 词不存在时返回 false
```

- 运行时加入的词与写在用户词典中的同一行效果相同：权重按加载时主词典的频率总和归一化，其他词的权重保持不变。
- 重复加入同一个词以最后一次为准。`remove_word` 对主词典和用户词典中的词同样有效，删除后该词完全移除，
  不会回退到更早的权重。
- 空词、非法编码、非正频率，以及超出 `TrieNodeId` 容量的加词通过 `LogConfig::Exception` 报告，词典保持原样。

增删词是非 `const` 操作，需要独占该 `Jieba`，调用期间不能有其他调用。库内部不加锁，调用方可按场景选用互斥锁、
读写锁、写时复制或单线程。分词入口是 `const`，多个线程可同时分词，各自使用独立的 `Workspace`。

`Jieba` 支持移动；需要副本时调用 `clone()`，它复制完整的词典和模型，适合写时复制：

```cpp
auto next = jieba.clone();       // 旧实例在复制和编辑期间可继续并发分词
next.add_word("杭研", PosTag{"nz"});
// 发布方式由调用方决定，例如在自己的锁内替换实例，或通过原子指针切换
```

大量增删之后，可调用 `compact()` 重建词典存储，回收删词留下的节点并恢复紧凑的子节点存储，分词结果保持不变。
它同样是非 `const` 操作，耗时与重新构建词典相当。
关键词提取器在每次调用时接收 `Jieba`，增删词后可以继续使用同一个提取器。

## 位置与工作区

位置采用半开区间 `[begin, end)`：

- `position.source` 按原文编码单元计数，UTF-8 对应字节偏移，可用于原文 `substr`。
- `position.runes` 按 Unicode 码点计数，可用于编码转换的边界映射。
- FULL 和 SEARCH 可产生重叠区间。

编码转换后的分词结果对应转换后的文本；定位原始 GBK / GB18030 字节的方法见
[原始字节映射](encoding.md#original-byte-slices)。

连续处理文本时，把 `Workspace` 和输出数组放在循环外，复用已分配的容量。
并发或嵌套的调用各用一个工作区，输入和输出也使用各自的存储。
调用 `workspace.release()` 可释放保留的缓冲。参见 [batch_cut.cpp](../examples/batch_cut.cpp)。

## 容量与错误处理

[Config.hpp](../include/neo/Config.hpp) 集中定义容量类型，默认均为 32 位：

| 配置 | 约束的范围 |
| --- | --- |
| `RuneIndex` | 单次输入的 Unicode 码点位置 |
| `SourceOffset` | 原文编码单元位置 |
| `DagOffset` | DAG 物化接口中的边偏移 |
| `TrieNodeId` | 主词典、用户词典和运行时加词合并后的 Trie 节点编号 |

可选 8、16、32、64 位，以及工具链支持的原生 128 位无符号整数；容器大小和内存分配仍受平台本身的限制。
调用方负责让输入和词典规模落在所选容量内，Debug 构建会用断言检查。
升级头文件或修改配置后，所有使用本库的编译单元都要用同一配置重新编译。

文本入口在解码时校验 UTF；词典、模型和外部数据的错误通过 `LogConfig::Exception` 报告。
抛出异常后，该次调用留下的部分输出应丢弃，工作区可以继续复用。内部不变量由断言检查。
只想预先校验编码时，可使用 `neo/Utf.hpp` 的 `Utf::is_valid(input)`；它与编码识别的区别见
[UTF validation](encoding.md#utf-validation)。

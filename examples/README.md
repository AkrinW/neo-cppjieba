# 应用示例

每个示例演示一种应用场景，`.cpp` 文件可以单独阅读和修改。示例在启动时加载一次词典和 HMM 模型，输入为 UTF-8；
第三个构造参数 `""` 表示只用主词典，需要业务词典时换成自己的文件路径。

从仓库根目录构建，需要支持 C++23（包括 `<print>`）的编译器：

```sh
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCPPJIEBA_BUILD_EXAMPLES=ON
cmake --build build-examples --config Release --parallel
```

示例通过 `neo_cppjieba::neo_cppjieba` 获取头文件路径和 C++23 编译要求。
以下命令均在仓库根目录运行。第一个参数是词典目录，也可以改成安装后的 `share/neo_cppjieba/dict/`。

| 示例 | 场景 | 主要接口 |
| --- | --- | --- |
| [basic_cut.cpp](basic_cut.cpp) | 对比七种分词模式 | `cut`、`CutMode` |
| [batch_cut.cpp](batch_cut.cpp) | 从文件或管道逐行分词，复用临时缓冲 | `cut_with_workspace`、`Workspace` |
| [search_highlight.cpp](search_highlight.cpp) | 搜索词定位和原文高亮 | `SEARCH`、`position.source`、`position.runes` |
| [word_count.cpp](word_count.cpp) | 流式统计 token 频次 | `cut_each`、`TokenView` |
| [owned_results.cpp](owned_results.cpp) | 保存请求处理函数返回的分词结果 | `cut_owned`、`cut_strings` |

先用模式对比示例观察同一段文本的结果：

```sh
./build-examples/examples/jieba_basic_cut dict '我来到北京清华大学'
```

以下运行命令使用 Linux / macOS 的路径和 Shell 语法。使用 Visual Studio 生成器时，
可执行文件位于 `build-examples/examples/Release/`，例如在 PowerShell 中运行：

```powershell
.\build-examples\examples\Release\jieba_basic_cut.exe dict '我来到北京清华大学'
```

日常文本可从 `MIX` 开始，搜索索引可选择 `SEARCH`。
完整模式表见 [C++ 使用指南](../docs/usage.md#分词模式)。

批量示例对每个输入行输出一行，token 用方括号括起，空行照样输出为空行：

```sh
printf '中国科学院\n我来到北京清华大学\n' | ./build-examples/examples/jieba_batch_cut dict
./build-examples/examples/jieba_batch_cut dict < test/testdata/testlines.utf8
```

`Workspace` 放在循环外，解码和算法缓冲跨行复用。`cut_with_workspace` 每次返回新的位置数组，
其中的词视图指向当前输入行，所以示例在下一次 `getline` 改写该行之前完成输出。
输出数组也要复用时，改用 `cut_into`：

```cpp
neo_cppjieba::Workspace workspace;
std::vector<neo_cppjieba::TokenPosition> positions;
jieba.cut_into(text, neo_cppjieba::CutMode::MIX, positions, workspace);
```

工作区的生命周期由调用方决定，也可以声明为 `thread_local`。一个工作区同一时刻服务一次调用，
并发调用或在 visitor 内嵌套分词时各用一个工作区。调用 `workspace.release()` 可释放保留的缓冲。

搜索示例匹配一个完整的 SEARCH token，并分别展示每次命中的高亮：

```sh
./build-examples/examples/jieba_search_highlight dict '中国科学院' '科学'
```

输出包含 `bytes=[6, 12) runes=[2, 4)` 和 `中国[科学]院`。
UTF-8 字符串的 `substr` 使用 `position.source` 字节偏移；`position.runes` 用于 Unicode 码点定位。
示例按 token 精确匹配，多词查询可在应用层组合。

词频示例从标准输入读取文本，按词的字符串顺序输出 `次数<TAB>词`：

```sh
printf '北京\n北京\n上海\n' | ./build-examples/examples/jieba_word_count dict
```

示例用 visitor 逐词计数。map 的 key 是持有词文本的 `std::string`，输入行被改写后计数依然有效。
统计包含标点在内的全部 token，需要过滤停用词时可在 visitor 中添加。

需要保存函数返回的结果时，运行所有权示例：

```sh
./build-examples/examples/jieba_owned_results dict '我来到北京清华大学'
```

`tokenize_message` 把参数字符串移动给 `cut_owned`，返回值持有整段原文和位置数组，函数返回后仍可读取。
`cut_strings` 返回每个词各自存储的字符串数组。示例同时展示两种方式，应用中选其一即可。
原文在使用期间保持有效时，借用型的 `cut` / `cut_with_workspace` 开销最小；`cut_owned` 的结果被移动后，需重新获取词视图。

退出码：成功为 0，用法错误为 2，词典、模型、输入编码或读取失败为 1。

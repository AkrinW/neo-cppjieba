# 词典与模型

本目录存放 UTF-8 编码的词典和模型。`Jieba` 构造时接收主词典、HMM 模型和用户词典的路径，部署时把需要的文件随程序一起提供。
安装后默认位于 `share/neo_cppjieba/dict/`，CMake 包通过 `neo_cppjieba_DICT_DIR` 给出该目录。
这些文件也可以编译进程序，通过 `DictSource::memory` 从内存加载，见 [从内存加载词典](../docs/usage.md#从内存加载词典)。

| 文件 | 用途 |
| --- | --- |
| `jieba.dict.utf8` | 主词典，每行包含词、词频和词性 |
| `hmm_model.utf8` | HMM 未登录词识别模型 |
| `user.dict.utf8` | 用户词典示例，按业务需要替换 |
| `idf.utf8` | `TfIdfExtractor` 使用的逆文档频率，每行为词和 IDF |
| `stop_words.utf8` | 关键词提取使用的停用词，每行一个 |
| `pos_dict/` | 上游 Python jieba `posseg` 的词性 HMM 模型数据，本库当前未使用 |

## 用户词典

用户词典每行接受以下形式，字段以空格分隔：

| 格式 | 含义 |
| --- | --- |
| `词` | 使用默认权重和默认词性 |
| `词 词性` | 使用默认权重，指定词性 |
| `词 词频 词性` | 指定词频和词性 |

例如：

```text
自然语言处理 1000 n
清华大学 100000 nt
```

构造时用户词典参数传 `""`，即只加载主词典。
完整用法见 [C++ 指南](../docs/usage.md)，词性缩写可参考 [词性标记表](../docs/pos_tags.md)。

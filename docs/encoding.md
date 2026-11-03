# Text encoding

[C++ usage](usage.md) · [Build and installation](building.md) · [Release notes](../CHANGELOG.md)

Jieba accepts UTF-8, UTF-16 and UTF-32 through the input character type.
For other encodings, convert the input to UTF before segmentation. The optional
`neo/encoding/IcuCodec.hpp` adapter provides static operations for explicitly
labeled `UTF8`, `GBK` and `GB18030` byte strings.

## Enable ICU

Install ICU4C development headers and the `uc` and `data` libraries, then configure
this project with `CPPJIEBA_ENABLE_ICU=ON`. Source consumers link to
`neo_cppjieba::icu`; installed consumers select the `icu` component:

```cmake
find_package(neo_cppjieba 2.0 CONFIG REQUIRED COMPONENTS icu)
target_link_libraries(my_app PRIVATE neo_cppjieba::icu)
```

Set `CMAKE_PREFIX_PATH` to the neo-cppjieba installation and use `ICU_ROOT` to select
the ICU installation. CMake reports the resolved ICU version and include path.
CMake looks for ICU only when the `icu` component is selected; all public headers,
including `IcuCodec.hpp`, install with the core library.

For a manual header integration, enable C++23 and link ICU explicitly:

```cmake
find_package(ICU REQUIRED COMPONENTS uc data)
target_link_libraries(my_app PRIVATE ICU::uc ICU::data)
```

## Convert text

| Purpose | Interface | Result |
| --- | --- | --- |
| Convert to UTF-8 | `to_utf8(bytes, encoding)` | Owned UTF-8 string |
| Convert from UTF-8 | `from_utf8(utf8, encoding)` | Owned target-encoding string |
| Map original bytes | `decode_with_offset_into(bytes, encoding, decoded)` | Runes and source byte boundaries |
| Map encoded bytes | `from_utf8_with_offset(utf8, encoding)` | `EncodedTextWithOffset` with `bytes` and `offsets` |

Conversions read the input only during the call and return owned storage, so
temporary strings can be passed directly.

```cpp
#include "neo/Jieba.hpp"
#include "neo/Utf.hpp"
#include "neo/encoding/IcuCodec.hpp"

#include <string>
#include <string_view>

using namespace neo_cppjieba;

const auto jieba = Jieba{"dict/jieba.dict.utf8", "dict/hmm_model.utf8", ""};
const auto bytes = std::string{"\xD6\xD0\xB9\xFA"}; // 中国 in GBK.
const auto utf8 = IcuCodec::to_utf8(bytes, EncodingId::GBK);
const auto result = jieba.cut(utf8, CutMode::MIX);
// result[0].word borrows utf8; its source range is [0, 6).

auto workspace = Workspace{};
const auto reused = jieba.cut_with_workspace(utf8, CutMode::SEARCH, workspace);

const auto utf8_result = jieba.cut_owned(IcuCodec::to_utf8(bytes, EncodingId::GBK), CutMode::MIX);
// utf8_result owns UTF-8 text; its source range is [0, 6).
```

Keep converted text alive and unchanged while using borrowed tokens, or transfer
the text into `cut_owned`. Token source positions count code units in the UTF text
passed to Jieba. For example, 中国 occupies four GBK bytes and six UTF-8 bytes.

## Encoded-byte mapping

Use rune ranges to locate words in the newly encoded output:

```cpp
const auto gbk = IcuCodec::from_utf8(utf8, EncodingId::GBK);
const auto gb18030 = IcuCodec::from_utf8(utf8, EncodingId::GB18030);
const auto encoded = IcuCodec::from_utf8_with_offset(utf8, EncodingId::GBK);
const auto range = result[0].position.runes;
const auto begin = encoded.offsets[range.begin];
const auto end = encoded.offsets[range.end];
const auto gbk_word = std::string_view{encoded.bytes}.substr(begin, end - begin);
```

`offsets[i]` is the byte position of rune boundary `i` in `encoded.bytes`.
For `A中B`, GBK boundaries are `{0, 1, 3, 4}` and UTF-8 boundaries are `{0, 1, 4, 5}`.
A supplementary character contributes one rune. The table has rune count plus
one entries, ends at `bytes.size()`, and contains `{0}` for empty input.
Mapped positions must fit `SourceOffset`.

Use rune ranges from the same, unchanged text. The mapped result owns its bytes
and offsets; `gbk_word` borrows that result. For conversion alone, `from_utf8`
constructs only the output string.

## Original-byte slices

Preserve the source mapping when exact original bytes are required:

```cpp
auto decoded = UnicodeWithOffset{};
IcuCodec::decode_with_offset_into(bytes, EncodingId::GBK, decoded);
const auto mapped_utf8 = Utf::encode<char>(decoded.runes);
const auto mapped_result = jieba.cut(mapped_utf8, CutMode::SEARCH);
const auto original_range = mapped_result[0].position.runes;
const auto original_begin = decoded.offsets[original_range.begin];
const auto original_end = decoded.offsets[original_range.end];
const auto original_word = std::string_view{bytes}.substr(original_begin, original_end - original_begin);
```

This retains the actual source bytes, including alternative byte sequences for
the same character. Keep `bytes` alive and unchanged while reading its slices.
Re-encoding uses the installed ICU mapping and can select a different sequence.

## UTF validation

`Utf::is_valid(input)` in `neo/Utf.hpp` checks the encoding associated with the
[input character type](usage.md#输入文本). It returns true for empty input and
false for malformed sequences, using a linear scan without decoding buffers.

```cpp
const auto valid = Utf::is_valid(utf8);
const auto valid_utf16 = Utf::is_valid(std::u16string_view{u"中国😀"});
```

Segmentation validates its input while decoding, in Release builds too, so a separate
check before each cut only adds a scan. Use `is_valid` when validation is needed on its
own. `cut_runes` accepts already validated
Unicode scalars; the UTF-32 text entry point validates externally supplied points.

Choose the source encoding from the caller, protocol or file metadata. For example,
bytes C2 A9 are valid in both UTF-8 and GBK and represent different text. Stored
character types describe the intended encoding; validation checks their contents.

## Errors and reproducibility

Malformed input and characters unavailable in the target encoding throw through
`LogConfig::Exception`. Encoding errors identify the target, scalar and UTF-8 byte
offset: `中A😀` fails at byte 4 when targeting GBK; GB18030 supports the final scalar.
Embedded NUL is processed as ordinary input; normalization and BOM handling are left to the caller.

Owned conversions return a complete result on success. A failed
`decode_with_offset_into` can leave partial output; discard it and reuse the buffers
on the next call, which clears them first. UTF-8 conversions use the native checked
decoder. GBK and GB18030 use ICU converters with STOP callbacks and optional
fallback mappings disabled.

Each call owns its converter. Pin the ICU release and conversion data when matching
byte mappings across deployments, and supply matching headers, libraries and runtime
files for each platform. On Windows, deploy the matching ICU DLLs with the application.
ICU's private-use and reverse fallback mappings still apply, so exact source-byte
recovery uses the original mapping above. See the
[converter API](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/ucnv_8h.html)
and [conversion guide](https://unicode-org.github.io/icu/userguide/conversion/converters.html).

## Tests

Enable ICU to include `encoded_text_test.cpp` in the modern unit suite:

```sh
cmake -S . -B build-icu -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DCPPJIEBA_ENABLE_ICU=ON
cmake --build build-icu --config Debug --target test_neo.run --parallel
ctest --test-dir build-icu -C Debug -R '^neo_unit_tests$' --output-on-failure
```

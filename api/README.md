# C ABI libraries

[Build guide](../docs/building.md) · [Public C header](include/neo_cppjieba.h) · [Release notes](../CHANGELOG.md)

Release 1.2.0 adds the v1 C API for UTF-8 segmentation and language bindings.
Release 2.0.0 extends v1 with reusable workspaces, memory and snapshot loading,
part-of-speech tags, runtime dictionary edits and keyword extraction; the 1.2.0
functions keep their layouts and behavior.
Shared and static libraries expose the same functions, fixed-width fields and
independently owned results. Package versions follow the project release; the ABI
major version and shared-library `SOVERSION` remain `1`.

## Build and install

```sh
cmake -S . -B build-api -DCPPJIEBA_BUILD_C_API=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-api --config Release
cmake --install build-api --config Release --prefix install
```

`CPPJIEBA_BUILD_C_API` defaults to `OFF`. When enabled, both
`CPPJIEBA_C_API_BUILD_SHARED` and `CPPJIEBA_C_API_BUILD_STATIC` default to `ON`;
select at least one library form.

| Platform in CI | Shared library | Static library |
| --- | --- | --- |
| Linux x64/ARM64, GCC 16 | `libneo_cppjieba_c.so.1` | `libneo_cppjieba_c.a` |
| macOS ARM64, LLVM 23/libc++ | `libneo_cppjieba_c.1.dylib` | `libneo_cppjieba_c.a` |
| Windows x64, MSVC 2026 | `neo_cppjieba_c.dll` and import library `neo_cppjieba_c.lib` | `neo_cppjieba_c_static.lib` |

CI covers Debug and Release builds. Binary compatibility applies within the same
platform ABI, with matching architecture, deployment target and system requirements.
Deploy the dictionary and model files with the application and pass their paths explicitly.

## Link an installed package

Set `CMAKE_PREFIX_PATH` to the installation prefix. A shared-library consumer uses C:

```cmake
project(example LANGUAGES C)
find_package(neo_cppjieba_c 1.2 CONFIG REQUIRED COMPONENTS shared)
add_executable(example main.c)
target_link_libraries(example PRIVATE neo_cppjieba::c_shared)
```

Static consumers enable the C++ linker and runtime for their C sources:

```cmake
project(example LANGUAGES C CXX)
find_package(neo_cppjieba_c 1.2 CONFIG REQUIRED COMPONENTS static)
add_executable(example main.c)
target_link_libraries(example PRIVATE neo_cppjieba::c_static)
```

Both targets carry the public include directory; `c_static` also defines
`NEO_CPPJIEBA_C_STATIC`. Manual static linking requires that definition and a compatible
C++ runtime. `neo_cppjieba::c` selects shared when available, then static; use explicit
targets to choose the linkage. A complete C caller is available in
[c_api_smoke.c](../test/unittest_neo/c_api_smoke.c).

## Calls and ownership

The header groups its functions by the handle they work on. Every call that can fail
returns a status and describes the failure in an optional error buffer; see
[Status and diagnostics](#status-and-diagnostics).

### Engines

| Function | Purpose |
| --- | --- |
| `neo_jieba_v1_max_input_bytes` | Query this build's conservative input byte limit |
| `neo_jieba_v1_engine_create` | Load dictionary/model paths and return an engine |
| `neo_jieba_v1_engine_create_from_sources` | Load an engine from file or memory sources, with any number of user dictionaries |
| `neo_jieba_v1_engine_create_from_snapshot` | Load an engine from a dictionary snapshot and an HMM model source |
| `neo_jieba_v1_write_dict_snapshot` | Encode dictionary sources into a snapshot file, replacing it atomically |
| `neo_jieba_v1_engine_clone` | Copy an engine for copy-on-write edits |
| `neo_jieba_v1_engine_destroy` | Release an engine |

Create an engine with nonempty main dictionary and model paths; an empty user
dictionary path selects the main dictionary alone. Paths are UTF-8; on Windows they are
converted to UTF-16 for the file system.

A `neo_jieba_source_v1` names one input: `NEO_JIEBA_V1_SOURCE_FILE` with a path of
`size` bytes, or `NEO_JIEBA_V1_SOURCE_MEMORY` with the contents such a file would have,
for embedded resources or platforms without a usable file system. Sources are borrowed
only during the call; `reserved` must be zero. User dictionaries load in array order and a
repeated word takes the last entry. Diagnostics name memory sources by role, such as
`<memory:model>`. All sources are checked before loading starts.

`write_dict_snapshot` encodes the main and user dictionaries into a snapshot file;
`engine_create_from_snapshot` loads it from a file or memory, skipping dictionary parsing,
and the resulting engine cuts exactly as one built from those sources. Snapshots from
earlier releases stay readable. The file is replaced atomically, so readers see the
previous file or the complete snapshot.

### Engine edits

| Function | Purpose |
| --- | --- |
| `neo_jieba_v1_engine_add_word` | Add or replace a user word with the default user weight |
| `neo_jieba_v1_engine_add_word_with_freq` | Add or replace a user word weighted by a positive frequency |
| `neo_jieba_v1_engine_remove_word` | Remove a main or user dictionary word and report whether it was present |
| `neo_jieba_v1_engine_compact` | Reclaim storage left by edits without changing results |

Runtime edits behave like user dictionary entries, weighted against the main dictionary
total fixed at load time. Tags passed to `engine_add_word` must be NUL-padded, so they
read back unchanged; an all-zero tag is empty. To keep serving reads during edits, clone
the engine, edit the clone, then switch readers to it: cloning only reads the original.
Existing results never refer to an engine, so edits leave them valid.

### Owned tokens

| Function | Purpose |
| --- | --- |
| `neo_jieba_v1_cut_utf8_owned` | Segment a byte buffer with an explicit length and return an owned result |
| `neo_jieba_v1_tokens_borrow_data` | Read the result's UTF-8 text and token byte ranges |
| `neo_jieba_v1_tokens_destroy` | Release a result and its borrowed storage |

Input pointers are borrowed during the call. Results own a copy of the input and all
token ranges, independently of the engine and earlier results. Engines may be destroyed
after producing results.

Read each token with its `byte_offset` and `byte_length`. Lengths are authoritative:
text may contain NUL bytes and individual tokens are not NUL-terminated.
Borrowed text/range pointers remain valid until their result is destroyed; release
the result through `tokens_destroy` in the library that created it.

### Reusable segmentation

| Function | Purpose |
| --- | --- |
| `neo_jieba_v1_workspace_create` | Create reusable segmentation and keyword scratch storage |
| `neo_jieba_v1_workspace_destroy` | Release a workspace |
| `neo_jieba_v1_ranges_create` | Create a reusable byte-range result |
| `neo_jieba_v1_ranges_destroy` | Release a ranges result and its borrowed storage |
| `neo_jieba_v1_cut_utf8_into` | Segment a byte buffer into byte ranges of that buffer, without copying it |
| `neo_jieba_v1_cut_utf8_tagged_into` | Segment like `cut_utf8_into` and attach a part-of-speech tag to each range |
| `neo_jieba_v1_ranges_borrow_data` | Read the latest byte ranges and tags |
| `neo_jieba_v1_lookup_tag` | Tag one complete word without segmenting it |

`cut_utf8_into` serves callers that keep their input alive. It fills a reusable
ranges result with byte ranges into the caller's input, without copying the input,
and reuses a workspace's scratch storage across calls. Each fill replaces the previous
ranges, and a failed call leaves them empty. Borrowed range pointers remain valid until
the next fill or `ranges_destroy`. A workspace holds no results: ranges survive their
workspace and engine.

`cut_utf8_tagged_into` produces the same ranges plus a parallel `tags` array of
`neo_jieba_pos_tag_v1`: up to four bytes, NUL-padded, with no terminator when all four
are used. A word takes its dictionary tag; otherwise CppJieba's rule gives `m` for
digits, `eng` for other ASCII and `x` for the rest. `lookup_tag` applies the same rule to
one complete word. After an untagged fill, `tags` is NULL.

### Keywords

| Function | Purpose |
| --- | --- |
| `neo_jieba_v1_tfidf_create` | Load an IDF table and stop words for TF-IDF keywords |
| `neo_jieba_v1_tfidf_destroy` | Release a TF-IDF extractor |
| `neo_jieba_v1_textrank_create` | Load stop words for TextRank keywords |
| `neo_jieba_v1_textrank_destroy` | Release a TextRank extractor |
| `neo_jieba_v1_keywords_create` | Create a reusable keywords result |
| `neo_jieba_v1_keywords_destroy` | Release a keywords result and its borrowed storage |
| `neo_jieba_v1_tfidf_extract_into` | Fill a keywords result with the top TF-IDF keywords of a byte buffer |
| `neo_jieba_v1_textrank_extract_into` | Fill a keywords result with the top TextRank keywords of a byte buffer |
| `neo_jieba_v1_keywords_borrow_data` | Read the latest keywords and their occurrence ranges |

Keyword extraction matches CppJieba's `KeywordExtractor` and `TextRankExtractor`:
it segments with the given engine in MIX mode, drops one-rune words and stop words,
and keeps at most `top_n` keywords from the highest weight. Extractors hold only their
IDF table and stop words, so one extractor serves any engine, including an edited clone.
Each `neo_jieba_keyword_v1` names a slice of the borrowed occurrence array; its first
range spells the word in the caller's input, which is never copied. Options are
required: pass `{NULL, 0}` allowed tags to keep every candidate, and window 5,
iterations 10 and damping 0.85 for CppJieba's TextRank defaults. A workspace serves
segmentation and extraction alike.

### Threads and lifetimes

Threads may call functions on a shared engine, extractor or result concurrently:
segmentation and extraction only read them and keep scratch storage per call. Concurrent
calls use separate output slots, diagnostic buffers, workspaces and results being
filled, and input, output and diagnostic buffers use separate live storage. The engine
edits are the exception: they need exclusive access to their engine, and the library
takes no locks. Release a previous handle before reusing its output slot, finish all
calls and reads before destroying a handle, and release all handles before unloading
the library. All destroy functions accept NULL, and text input pointers may be NULL
when their size is zero. FULL and SEARCH can return overlapping byte ranges.

## Status and diagnostics

The public header defines `OK`, `INVALID_ARGUMENT`, `OPERATION_FAILED`,
`OUT_OF_MEMORY` and `LIMIT_EXCEEDED`. Invalid UTF-8 and dictionary/model failures
return `OPERATION_FAILED`. The byte-limit query describes representable sizes;
allocations can still fail within that limit.

Pass a caller-owned `neo_jieba_error_buffer_v1`, NULL to omit diagnostics, or
`{NULL, 0, 0}` to request the required message length. `message_size` reports the
full byte length excluding NUL. Positive buffer capacity receives a NUL-terminated
prefix; truncation preserves the operation's status. Successful calls clear the message.

Failed create/cut calls clear their output handles. A failed borrow call clears a
valid output descriptor. C++ exceptions are translated into status values inside
the adapter; internal invariant failures remain fatal.

## Runtime and validation

Shared libraries export only the functions declared in the public header and bundle
private C++ runtimes: GNU C++/unwind archives on Linux, libc++/libc++abi/libunwind on
macOS, and `/MT` or `/MTd` on Windows. Allocation ownership stays inside the library.

New releases only add functions to ABI v1, so programs built against an earlier
release keep running. On Linux, functions added after 1.2.0 carry the
`NEO_CPPJIEBA_C_1.1` symbol version: a program that uses them reports the missing
version when loaded against an older library, before any call runs.

Two checks keep that promise. `test/unittest_neo/c_api_abi_v1.c` freezes every v1
type, layout, numeric value and function type at compile time on each platform, and
`tools/check-c-abi.sh` runs libabigail's `abidiff` against the latest release on Linux,
failing when a released symbol or public type changes.

The macOS build searches beside the selected compiler, following the Homebrew LLVM
layout in CI. Set `CPPJIEBA_C_API_LIBCXX`, `CPPJIEBA_C_API_LIBCXXABI` and
`CPPJIEBA_C_API_LIBUNWIND` to matching PIC archives for other layouts.

Static libraries use the final application's runtime and symbol visibility.
On MSVC, match `CMAKE_MSVC_RUNTIME_LIBRARY` and the build configuration between
the library and its consumers. The static `.lib` and DLL import `.lib` serve their
respective targets and compile definitions.

With `BUILD_TESTING=ON`, tests cover both variants, exported symbols, runtime
dependencies and installed C consumers. Linux also checks a C host with conflicting
C++ runtime symbols. Configuration commands are listed in the [build guide](../docs/building.md#测试).

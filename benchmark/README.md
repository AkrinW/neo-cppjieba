# Benchmarks

All benchmark executables live here and are enabled with
`CPPJIEBA_BUILD_BENCHMARKS=ON`. They are separate from GoogleTest and CTest.

Keep benchmark measurements, environment records and raw logs locally under
`benchmark/results/`. This directory is ignored by Git.

The README chart and its compact numeric snapshot are committed under
[`docs/benchmarks/`](../docs/benchmarks/). The measurement date, source revision and
environment are shown with the chart. It compares four owned-string output paths,
two corpora and five samples per case.
It plots UTF-8 MiB/s on a linear axis starting at zero, calculated as
`utf8_bytes * rounds / (median_ms / 1000) / 1048576`;
the whiskers convert maximum/minimum elapsed time to minimum/maximum throughput.
Each snapshot records the measurement date, environment and revision. Raw reports remain local; the snapshot
retains their hashes, input hashes, dependency versions, timings and comparison counts.

Use the [manual CI update](#updating-the-readme-from-ci) to refresh the published
numbers. Once its documentation PR has been merged, regenerate the SVG locally
with Python and Matplotlib 3.3 or newer:

```sh
python3 docs/benchmarks/plot.py docs/benchmarks/snapshot.json docs/benchmarks/comparison.svg
```

For local measurements, use the commands in [PYTHON.md](PYTHON.md). Keep the
implementation, output contract, input sizes and timing units consistent across series.
For a release-to-release regression measurement, build both revisions with the
same compiler and flags, pin them to the same CPU, and alternate their runs.
Record source revisions and compare token sequences as well as timings.

`cut_compare_benchmark.cpp` compiles old CppJieba, Neo and the Rust adapters in one translation unit, which
sits at GCC's `inline-unit-growth` limit: an unrelated change can move a per-rune helper such as
`decode_one_utf8` out of line and shift Neo timings by about 10%. When comparing two builds with GCC,
configure both with `-DCMAKE_CXX_FLAGS=--param=inline-unit-growth=200`, and check that
`nm -C cut_compare_benchmark | grep -E 'decode_one_utf8|checked_decode_step'` prints nothing for either.

```sh
git submodule update --init deps/cppjieba deps/limonp
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DCPPJIEBA_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
```

Executables are written to `build-bench/benchmark/` (with a configuration
subdirectory for multi-configuration generators).

| Target | Measures | Comparison dependency |
| --- | --- | --- |
| `jieba_benchmark` | End-to-end segmentation | old cppjieba |
| `dict_trie_benchmark` | Dictionary loading and lookup | old cppjieba |
| `dag_benchmark` | DAG construction | old cppjieba |
| `trie_benchmark` | Trie construction and queries | old cppjieba |
| `fileio_benchmark` | Dictionary parsing with streams and mapped views | limonp |
| `load_benchmark` | Dictionary loading from text and from snapshots | none |
| `load_test` | Legacy segmentation and keyword extraction throughput | old cppjieba |
| `unicode_benchmark` | Unicode decoding and encoding | none |
| `prefilter_benchmark` | Input prefiltering | none |
| `query_segment_benchmark` | Query segmentation strategies | none |
| `cut_compare_benchmark` | Old, neo and Rust segmentation | old cppjieba, jieba-rs |
| `python_compare_benchmark.py` | The same native paths plus Python `list[str]` | `cut_compare_benchmark`, Python jieba |

The first ten targets are built by the benchmark option. The Rust target also
requires `CPPJIEBA_BUILD_RUST_BENCHMARKS=ON` and Cargo. Its adapter links through
`jieba_rs::jieba_rs`; see [RUST.md](RUST.md) for the output contracts and commands.

`load_benchmark` first checks that snapshot and text loading cut and tag the corpus identically, then times
each loading stage. `--rounds N` sets the rounds per case (default 9); `--cold` evicts the input files from
the page cache before every round, which Linux allows without root. Model loading writes info logs to
stderr, so redirect it to read the table alone:

```sh
build-bench/benchmark/load_benchmark --cold 2>/dev/null
```

The Python script runs the native benchmark and Python serially using the same
corpus, dictionary, HMM model, rounds and samples. It requires Python 3.11 or
newer and the `deps/python-jieba` submodule; see [PYTHON.md](PYTHON.md).

Shared input paths still refer to the repository's `dict/` and `test/testdata/`.
`QuerySegmentCompare.hpp` in `test/` supplies the same query comparison logic to
the correctness tests and standalone benchmark.

FileIO correctness is covered by `test/unittest_neo/fileio_test.cpp`.
The standalone benchmark validates its inputs and reports timing observations.

## Instruction counts

Timings on shared CI runners vary between runs, by up to 2x when a run lands on different
hardware; instruction counts repeat exactly.
`instruction_workload` runs one phase per invocation — `load`, `snapshot_load`, `cut` (every mode),
`tag` or `keywords` — over the repository dictionaries and `weicheng.utf8`, and each phase lives in its
own `measure_<phase>` function, so callgrind can count it alone:

```sh
tools/instruction-counts.sh build-instructions origin/master
```

The script builds the current workload source with `-O2` against the baseline's and the current headers,
counts each phase with `valgrind --tool=callgrind --toggle-collect`, and writes a table to
`build-instructions/summary.md`. A baseline that lacks the measured API is reported and skipped. Counts
repeat exactly between runs, so any change comes from the code or the compiler; the Quality workflow
publishes the table for every pull request without enforcing a threshold.

## GitHub Actions

The [build guide](../docs/building.md) describes the platform and unit-test matrix.
CI uploads JUnit results and CTest logs for each matrix job, including failed tests.

The Linux `Release benchmarks / Rust=ON` job builds the C++ benchmarks and the
pinned Rust adapter, runs the Rust and Python adapter tests, then compares old C++,
neo C++, Rust and Python through `python_compare_benchmark.py`. The `Rust=OFF` job
checks the standalone C++ benchmark configuration.

All four implementations run serially on one allowed CPU of the same runner,
using the common dictionary and HMM model. CI uses GCC 16, Rust 1.99.0 and Python
3.14.8; comparison dependencies are pinned by submodule revisions and `Cargo.lock`.
GitHub assigns hosted runners from a mixed hardware pool (AMD EPYC 7763, 9V74 and 9V45
have all appeared), so compare ratios within one run rather than absolute times across runs.

| Corpus | Rounds per sample | Samples |
| --- | ---: | ---: |
| `testlines.utf8` | 10,000 | 5 |
| `weicheng.utf8` | 3 | 5 |

The job summary shows timings for MIX, MP, FULL and SEARCH and the output
differences, plus the standalone old/neo C++ HMM comparison. Columns are grouped
by implementation: old C++, neo owned/borrowed/reused, Rust owned/borrowed, Python.
The native JSON exports HMM medians, minima, maxima and output differences in `hmm`.
The Python script writes a clean Markdown table beside each combined JSON report;
CI uses these tables for summaries and PR comments. Warnings and progress output
remain in the raw logs. The `benchmarks-linux-x64-release` artifact retains Markdown,
combined JSON, native JSON, raw logs and compiler/CPU information for 14 days. Reports stay
under the ignored `benchmark/results/ci/` directory.

Build, test and benchmark execution errors fail CI. Timing ratios have no pass/fail
threshold on shared runners; the existing benchmark suppresses rankings when
segmentation results differ. macOS and Windows compile the C++ benchmarks as part
of their platform jobs; the four-language measurements currently run on Linux x64.

Pull requests from branches in this repository also receive one benchmark comment,
updated in place by `marocchino/sticky-pull-request-comment`. It reuses the Actions
summary saved as `summary.md` in the report artifact, with the job status, head and
tested revisions, the runner CPU model, and a link to the workflow and full reports. A separate job has
`pull-requests: write` permission and reads only the artifact. It updates the comment
only for the current head of an open PR; PR code executes in the measurement job.

Fork PRs publish summaries and artifacts. The tables compare implementations on
the same inputs. Measuring a PR against its base branch requires a separate
base-revision measurement with matching conditions.

## Updating the README from CI

The `CMake` workflow accepts a manual `update_benchmark_docs` input, disabled by
default. To publish a new measurement:

1. In **Settings → Actions → General → Workflow permissions**, enable
   **Allow GitHub Actions to create and approve pull requests**.
   See the [action's permission requirements](https://github.com/peter-evans/create-pull-request#workflow-permissions).
2. Open **Actions → CMake → Run workflow**, select the default branch (`master`),
   and enable `update_benchmark_docs`.
3. After the Linux builds, tests and benchmarks pass, review and merge the
   generated PR from `docs/benchmark-results`. Later manual runs update that PR
   while it remains open.

The measurement uses the existing GitHub-hosted `ubuntu-26.04` job: all four
implementations run serially on one CPU with five samples per case. The generator
checks matching revisions, environments, dictionary/model hashes and native binary
hashes, then retains timing ranges and token-sequence differences in the snapshot.
Shared runner timings can vary; the workflow records observations without a speed gate.

The PR contains only `README.md`, `docs/benchmarks/comparison.svg` and
`docs/benchmarks/snapshot.json`. The README's `benchmark:start` / `benchmark:end`
markers delimit its generated measurement section; retain both markers when editing.
Release history and upgrade instructions belong in `CHANGELOG.md`.

Runner metadata, reports and hashes link the chart to its Actions run. The
`benchmarks-linux-x64-release` artifact retains the raw results for 14 days;
`benchmark-documentation` contains the generated files. The committed snapshot
keeps the numeric results and provenance after artifacts expire. Existing dated
snapshots retain their original measurement source.

Only a manual run on the default branch creates a documentation PR. Its separate
publication job has `contents: write` and `pull-requests: write`; measurement jobs
keep read-only repository permissions. If the default branch advances before
publication, rerun against its latest commit. Chart data changes when the generated
documentation PR is merged.

GitHub may require a maintainer to select **Approve workflows to run** on the
generated PR before its checks start. See
[triggering workflows with GITHUB_TOKEN](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).

To check the snapshot generator locally:

```sh
python3 -B -m unittest discover -s docs/benchmarks -p '*_test.py' -v
```

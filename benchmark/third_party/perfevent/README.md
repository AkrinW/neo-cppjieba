Vendored PerfEvent header from [viktorleis/perfevent](https://github.com/viktorleis/perfevent/tree/a09a94ad2ed2c7e7b04ce4c851f69cba3a7d4d17), commit `a09a94ad2ed2c7e7b04ce4c851f69cba3a7d4d17` (2026-01-07).

PerfEvent.hpp and LICENSE are unmodified; SHA256SUMS records their upstream bytes. The MIT license and copyright notice are retained.

PerfEvent.hpp reads Linux perf_event hardware counters around a measured region and compiles to no-op stubs on other platforms. No benchmark includes it at present; a benchmark that does includes `third_party/perfevent/PerfEvent.hpp`.

Keep it out of project formatting and linting: `.clang-format-ignore` stops clang-format, including direct invocations, and the `third_party/` pattern in the repository's `.clang-tidy` excludes it from the format and tidy scripts. Update it only by replacing both files with a newer upstream revision and refreshing this README and SHA256SUMS.

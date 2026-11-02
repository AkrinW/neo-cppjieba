#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

# Package and GoogleTest discovery checks configure separate projects and add no library coverage.
readonly CTEST_EXCLUDE='^neo_(cpp_api_install|c_api_install|googletest_)'
# Report library headers and the C ABI adapter only.
readonly IGNORE_REGEX='(^|/)(test|benchmark|examples|deps|_deps|third_party|generated)/|^/usr/'

usage() {
    echo "usage: $0 <build-dir> <output-dir>" >&2
    echo "The build must use Clang with -fprofile-instr-generate -fcoverage-mapping." >&2
    echo "Set COVERAGE_BASE to a git revision to add coverage of the lines changed since it to summary.md." >&2
}

resolve_tool() {
    local override=$1
    local name=$2
    if [[ -n "${override}" ]]; then
        printf '%s\n' "${override}"
    elif command -v "${name}" >/dev/null 2>&1; then
        command -v "${name}"
    else
        echo "${name} not found" >&2
        return 1
    fi
}

main() {
    if [[ $# -ne 2 ]]; then
        usage
        return 2
    fi

    local build_dir output_dir
    build_dir=$(cd -- "$1" && pwd)
    mkdir -p "$2"
    output_dir=$(cd -- "$2" && pwd)

    local llvm_profdata llvm_cov python
    llvm_profdata=$(resolve_tool "${LLVM_PROFDATA:-}" llvm-profdata)
    llvm_cov=$(resolve_tool "${LLVM_COV:-}" llvm-cov)
    python=$(resolve_tool "${PYTHON:-}" python3)

    local profile_dir="${build_dir}/coverage-profiles"
    rm -rf "${profile_dir}"
    mkdir -p "${profile_dir}"
    LLVM_PROFILE_FILE="${profile_dir}/%p-%m.profraw" \
        ctest --test-dir "${build_dir}" -E "${CTEST_EXCLUDE}" --output-on-failure --no-tests=error

    local profdata="${output_dir}/coverage.profdata"
    "${llvm_profdata}" merge -sparse "${profile_dir}"/*.profraw -o "${profdata}"

    local -a binaries=()
    mapfile -t binaries < <(find "${build_dir}/test" -maxdepth 1 -type f -name '*.run' | sort)
    if [[ ${#binaries[@]} -eq 0 ]]; then
        echo "No test executables found under ${build_dir}/test" >&2
        return 1
    fi
    # The shared C ABI library carries its own instrumented copy of the adapter and headers.
    local -a libraries=()
    if [[ -d "${build_dir}/api" ]]; then
        mapfile -t libraries < <(find "${build_dir}/api" -maxdepth 1 -type f -name 'libneo_cppjieba_c.so*' | sort)
    fi
    local -a objects=("${binaries[0]}")
    local binary
    for binary in "${binaries[@]:1}" "${libraries[@]}"; do
        objects+=(-object "${binary}")
    done

    local -a common=(-instr-profile="${profdata}" -ignore-filename-regex="${IGNORE_REGEX}")
    "${llvm_cov}" report "${objects[@]}" "${common[@]}" | tee "${output_dir}/summary.txt"
    "${llvm_cov}" show "${objects[@]}" "${common[@]}" -format=html -show-line-counts-or-regions \
        -show-branches=count -output-dir="${output_dir}/html" -project-title=neo-cppjieba
    "${llvm_cov}" export "${objects[@]}" "${common[@]}" -format=lcov > "${output_dir}/coverage.lcov"

    # The lcov export merges every binary and instantiation per line, so summary.md reports union coverage;
    # summary.txt keeps llvm-cov's per-function view, which differs when instantiations disagree.
    local -a summary_args=("${output_dir}/coverage.lcov")
    if [[ -n "${COVERAGE_BASE:-}" ]]; then
        summary_args+=(--base "${COVERAGE_BASE}")
    fi
    "${python}" "${SCRIPT_DIR}/coverage_summary.py" "${summary_args[@]}" > "${output_dir}/summary.md"
    echo "Coverage report: ${output_dir}/html/index.html"
}

cd "${REPO_ROOT}"
main "$@"

#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

usage() {
    echo "usage: $0 <work-dir> [baseline-revision]" >&2
    echo "Compares the shared C ABI library with a release, by default the latest v* tag reachable from HEAD." >&2
    echo "ABIDIFF selects the abidiff binary; CC and CXX select the compilers for both builds." >&2
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

# The baseline worktree to remove on exit, including after a failed build or comparison.
BASELINE_WORKTREE=''
remove_baseline_worktree() {
    if [[ -n "${BASELINE_WORKTREE}" ]]; then
        git -C "${REPO_ROOT}" worktree remove --force "${BASELINE_WORKTREE}" >/dev/null 2>&1 || true
    fi
}

# Build only the shared C library with debug information, which abidiff reads for types and layouts.
build_library() {
    local source_dir=$1
    local build_dir=$2
    cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
        -DCPPJIEBA_BUILD_C_API=ON -DCPPJIEBA_C_API_BUILD_SHARED=ON -DCPPJIEBA_C_API_BUILD_STATIC=OFF >/dev/null
    cmake --build "${build_dir}" --parallel --target neo_cppjieba_c >/dev/null
}

main() {
    if [[ $# -lt 1 || $# -gt 2 ]]; then
        usage
        return 2
    fi

    mkdir -p "$1"
    local work
    work=$(cd -- "$1" && pwd)
    local baseline=${2:-}
    if [[ -z "${baseline}" ]]; then
        baseline=$(git -C "${REPO_ROOT}" describe --tags --abbrev=0 --match 'v*')
    fi
    local abidiff
    abidiff=$(resolve_tool "${ABIDIFF:-}" abidiff)

    local baseline_src="${work}/baseline-src"
    git -C "${REPO_ROOT}" worktree remove --force "${baseline_src}" >/dev/null 2>&1 || rm -rf "${baseline_src}"
    git -C "${REPO_ROOT}" worktree add --detach "${baseline_src}" "${baseline}" >/dev/null
    BASELINE_WORKTREE=${baseline_src}
    trap remove_baseline_worktree EXIT

    build_library "${baseline_src}" "${work}/baseline-build"
    build_library "${REPO_ROOT}" "${work}/current-build"

    # Public headers limit the comparison to the documented types; opaque handles may change freely.
    local -a inputs=(--headers-dir1 "${baseline_src}/api/include" --headers-dir2 "${REPO_ROOT}/api/include"
        "${work}/baseline-build/api/libneo_cppjieba_c.so" "${work}/current-build/api/libneo_cppjieba_c.so")
    # Added symbols are compatible; any other reported difference changes what released binaries rely on.
    local status=0
    "${abidiff}" --no-added-syms "${inputs[@]}" > "${work}/incompatible.txt" || status=$?
    "${abidiff}" "${inputs[@]}" > "${work}/report.txt" || true

    {
        printf '## C ABI compared with %s\n\n' "${baseline}"
        if [[ ${status} -eq 0 ]]; then
            printf 'No released symbol or public type changed. Full report, including additions:\n\n'
        else
            printf '**Released symbols or public types changed (abidiff status %s):**\n\n```text\n' "${status}"
            cat "${work}/incompatible.txt"
            printf '```\n\nFull report:\n\n'
        fi
        printf '```text\n'
        cat "${work}/report.txt"
        printf '```\n'
    } > "${work}/summary.md"
    cat "${work}/summary.md"
    return $((status == 0 ? 0 : 1))
}

main "$@"

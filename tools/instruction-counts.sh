#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

readonly PHASES=(load snapshot_load cut tag keywords)
readonly WORKLOAD_SOURCE="${REPO_ROOT}/benchmark/instruction_workload.cpp"

usage() {
    echo "usage: $0 <work-dir> <baseline-revision>" >&2
    echo "Counts the instructions of each workload phase under callgrind for the baseline and the current tree." >&2
    echo "CXX selects the compiler for both builds; VALGRIND selects the valgrind binary." >&2
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

# The baseline worktree to remove on exit, including after a failed build or run.
BASELINE_WORKTREE=''
remove_baseline_worktree() {
    if [[ -n "${BASELINE_WORKTREE}" ]]; then
        git -C "${REPO_ROOT}" worktree remove --force "${BASELINE_WORKTREE}" >/dev/null 2>&1 || true
    fi
}

# Build the current workload source against one revision's headers with release optimization.
build_workload() {
    local cxx=$1
    local include_dir=$2
    local output=$3
    "${cxx}" -std=c++23 -O2 -g -DNDEBUG -I"${include_dir}" "${WORKLOAD_SOURCE}" -o "${output}"
}

# Print the instructions the measured function of one phase executes; setup before it is not collected.
count_phase() {
    local valgrind=$1
    local workload=$2
    local phase=$3
    local log=$4
    "${valgrind}" --tool=callgrind --callgrind-out-file=/dev/null --log-file="${log}" \
        "--toggle-collect=*measure_${phase}*" "${workload}" "${phase}" \
        "${REPO_ROOT}/dict/jieba.dict.utf8" "${REPO_ROOT}/dict/hmm_model.utf8" "${REPO_ROOT}/dict/idf.utf8" \
        "${REPO_ROOT}/dict/stop_words.utf8" "${REPO_ROOT}/test/testdata/weicheng.utf8" >/dev/null
    sed -nE 's/.*Collected : ([0-9]+).*/\1/p' "${log}"
}

main() {
    if [[ $# -ne 2 ]]; then
        usage
        return 2
    fi

    mkdir -p "$1"
    local work
    work=$(cd -- "$1" && pwd)
    local baseline=$2
    local cxx valgrind
    cxx=$(resolve_tool "${CXX:-}" c++)
    valgrind=$(resolve_tool "${VALGRIND:-}" valgrind)

    local baseline_src="${work}/baseline-src"
    git -C "${REPO_ROOT}" worktree remove --force "${baseline_src}" >/dev/null 2>&1 || rm -rf "${baseline_src}"
    git -C "${REPO_ROOT}" worktree add --detach "${baseline_src}" "${baseline}" >/dev/null
    BASELINE_WORKTREE=${baseline_src}
    trap remove_baseline_worktree EXIT

    local summary="${work}/summary.md"
    printf '## Instruction counts against %s\n\n' "${baseline}" > "${summary}"
    build_workload "${cxx}" "${REPO_ROOT}/include" "${work}/current-workload"
    # A baseline without the measured API cannot build the workload; there is nothing to compare yet.
    if ! build_workload "${cxx}" "${baseline_src}/include" "${work}/baseline-workload" 2> "${work}/baseline-build.log"; then
        printf 'The baseline cannot build `benchmark/instruction_workload.cpp`, so nothing is compared.\n' >> "${summary}"
        cat "${summary}"
        return 0
    fi

    {
        printf 'Report only. callgrind counts the instructions of each phase of `benchmark/instruction_workload.cpp`, '
        printf 'built with `-O2` against each revision and run over the repository dictionaries and '
        printf '`test/testdata/weicheng.utf8`. Counts are deterministic, so any change comes from the code.\n\n'
        printf '| Phase | Baseline | Current | Change |\n| --- | ---: | ---: | ---: |\n'
    } >> "${summary}"
    local phase before after
    for phase in "${PHASES[@]}"; do
        before=$(count_phase "${valgrind}" "${work}/baseline-workload" "${phase}" "${work}/baseline-${phase}.log")
        after=$(count_phase "${valgrind}" "${work}/current-workload" "${phase}" "${work}/current-${phase}.log")
        awk -v phase="${phase}" -v before="${before}" -v after="${after}" \
            'BEGIN { printf "| %s | %d | %d | %+.2f%% |\n", phase, before, after, 100 * (after - before) / before }' \
            >> "${summary}"
    done
    cat "${summary}"
}

main "$@"

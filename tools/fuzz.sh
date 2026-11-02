#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

readonly FUZZERS=(utf_decode main_dict user_dict hmm_model dict_snapshot snapshot_roundtrip keyword utf_cut c_api_cut
    c_api_ranges c_api_sources c_api_edit c_api_keyword)
readonly FUZZ_SECONDS_DEFAULT=60

usage() {
    echo "usage: $0 <build-dir> [seconds-per-fuzzer] [fuzzer...]" >&2
    echo "       $0 --list" >&2
    echo "fuzzers: ${FUZZERS[*]}" >&2
    echo "FUZZ_FORK=<n> runs n fuzzing processes per target; FUZZ_MINIMIZE=1 merges each working corpus afterwards." >&2
}

# The libFuzzer dictionary for each target's input format, under test/fuzz/dict.
dictionary_for() {
    case $1 in
        main_dict | user_dict | snapshot_roundtrip | keyword | c_api_sources) echo dictionary ;;
        hmm_model) echo hmm_model ;;
        dict_snapshot) echo dict_snapshot ;;
        *) echo text ;;
    esac
}

# Seed each corpus from repository data so mutations start from well-formed inputs.
write_seeds() {
    local corpus=$1
    local name
    for name in "${FUZZERS[@]}"; do
        mkdir -p "${corpus}/${name}"
    done

    cp "${REPO_ROOT}/test/testdata/testlines.utf8" "${corpus}/utf_decode/"
    head -n 64 "${REPO_ROOT}/dict/jieba.dict.utf8" > "${corpus}/main_dict/jieba_head.utf8"
    cp "${REPO_ROOT}"/test/testdata/userdict* "${REPO_ROOT}/dict/user.dict.utf8" "${corpus}/user_dict/"
    # Keep 16 emission entries per state; the full model is too large for quick mutation.
    awk -F, '/^#/ || NF <= 16 { print; next } { line = $1; for (i = 2; i <= 16; ++i) line = line "," $i; print line }' \
        "${REPO_ROOT}/dict/hmm_model.utf8" > "${corpus}/hmm_model/hmm_model_head.utf8"
    cp "${REPO_ROOT}"/test/testdata/snapshot/format_v*.bin "${corpus}/dict_snapshot/"

    # c_api_cut and utf_cut read their mode from the first byte.
    # c_api_ranges also reads fill flags, here tagging both fills, then two inputs split at a 0xFF byte.
    # c_api_keyword reads the ranker order, top_n, window, iterations, a damping choice and a tag count first.
    local line mode index=0
    while IFS= read -r line; do
        for mode in 0 1 2 3 4 5 6; do
            printf "\\$(printf '%03o' "${mode}")%s" "${line}" > "${corpus}/c_api_cut/line${index}_mode${mode}"
            printf "\\$(printf '%03o' "${mode}")%s" "${line}" > "${corpus}/utf_cut/line${index}_mode${mode}"
            printf "\\$(printf '%03o' "${mode}")\\003%s\\377%s" "${line}" "${line}" \
                > "${corpus}/c_api_ranges/line${index}_mode${mode}"
        done
        printf '\000\005\005\012\002\000%s' "${line}" > "${corpus}/c_api_keyword/line${index}"
        index=$((index + 1))
    done < "${REPO_ROOT}/test/testdata/testlines.utf8"

    # c_api_sources reads a flag byte: 0 loads dictionaries split at 0xFF bytes, 1 loads a snapshot.
    local testdata="${REPO_ROOT}/test/testdata" user_dict
    for user_dict in "${testdata}"/userdict* "${testdata}/snapshot/user.dict.utf8"; do
        { printf '\000'; cat "${testdata}/snapshot/main.dict.utf8"; printf '\377'; cat "${user_dict}"; } \
            > "${corpus}/c_api_sources/${user_dict##*/}"
    done
    { printf '\001'; cat "${testdata}/snapshot/format_v1.bin"; } > "${corpus}/c_api_sources/format_v1.bin"

    # keyword reads top_n, then an IDF table, stop words and the input split at 0xFF bytes.
    local idf_head stop_head
    idf_head=$(head -n 200 "${REPO_ROOT}/dict/idf.utf8")
    stop_head=$(head -n 40 "${REPO_ROOT}/dict/stop_words.utf8")
    index=0
    while IFS= read -r line; do
        printf '\012%s\n\377%s\n\377%s' "${idf_head}" "${stop_head}" "${line}" > "${corpus}/keyword/line${index}"
        index=$((index + 1))
    done < "${REPO_ROOT}/test/testdata/testlines.utf8"

    # snapshot_roundtrip reads a flag byte: 0 adds user dictionaries to the small dictionary, 1 fuzzes the main one.
    for user_dict in "${testdata}"/userdict* "${testdata}/snapshot/user.dict.utf8"; do
        { printf '\000'; cat "${user_dict}"; } > "${corpus}/snapshot_roundtrip/${user_dict##*/}"
    done
    {
        printf '\001'
        cat "${testdata}/snapshot/main.dict.utf8"
        printf '\377'
        cat "${testdata}/snapshot/user.dict.utf8"
    } > "${corpus}/snapshot_roundtrip/main_and_user"

    # c_api_edit reads a diagnostic byte, then edits of a kind, a word length and word, a tag and a frequency byte:
    # add 杭研 tagged nz, add 云计算 with frequency 112, remove 大学, and compact.
    printf '\004\000\006杭研\002\000\001\011云计算\002\200\002\006大学\000\000\003\000\000\000' \
        > "${corpus}/c_api_edit/add_remove_compact"
}

# The library logs every rejected input, so target output is closed; libFuzzer still reports crashes.
# New inputs go to the working corpus; the checked-in corpus is read only.
run_fuzzer() {
    local binary=$1
    local corpus=$2
    local checked_in=$3
    local artifacts=$4
    local seconds=$5
    local dictionary=$6

    local -a parallel=()
    if [[ ${FUZZ_FORK:-1} -gt 1 ]]; then
        parallel=(-fork="${FUZZ_FORK}")
    fi
    echo "--- ${binary##*/}: ${seconds}s ---"
    "${binary}" "${corpus}" "${checked_in}" "${parallel[@]}" -dict="${dictionary}" \
        -max_total_time="${seconds}" -timeout=10 -rss_limit_mb=2048 \
        -close_fd_mask=3 -print_final_stats=1 -artifact_prefix="${artifacts}/"
}

# Keep the inputs of a working corpus that add coverage, so a cached corpus stays small between runs.
minimize_corpus() {
    local binary=$1
    local corpus=$2

    local merged="${corpus}.merged"
    rm -rf "${merged}"
    mkdir -p "${merged}"
    "${binary}" -merge=1 -close_fd_mask=3 "${merged}" "${corpus}" 2>&1 | grep -E '^MERGE-OUTER: [0-9]+ new files' || true
    rm -rf "${corpus}"
    mv "${merged}" "${corpus}"
}

main() {
    if [[ $# -lt 1 ]]; then
        usage
        return 2
    fi
    if [[ $1 == --list ]]; then
        printf '%s\n' "${FUZZERS[@]}"
        return 0
    fi

    local build_dir=$1
    local seconds=${2:-${FUZZ_SECONDS_DEFAULT}}
    local -a fuzzers=("${@:3}")
    if [[ ${#fuzzers[@]} -eq 0 ]]; then
        fuzzers=("${FUZZERS[@]}")
    fi

    local work="${build_dir}/fuzz-run"
    write_seeds "${work}/corpus"
    mkdir -p "${work}/artifacts"

    local name binary
    for name in "${fuzzers[@]}"; do
        binary="${build_dir}/fuzz/${name}_fuzzer"
        run_fuzzer "${binary}" "${work}/corpus/${name}" "${REPO_ROOT}/test/fuzz/corpus/${name}" "${work}/artifacts" \
            "${seconds}" "${REPO_ROOT}/test/fuzz/dict/$(dictionary_for "${name}").dict"
        if [[ ${FUZZ_MINIMIZE:-} == 1 ]]; then
            minimize_corpus "${binary}" "${work}/corpus/${name}"
        fi
    done
}

main "$@"

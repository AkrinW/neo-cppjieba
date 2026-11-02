#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

readonly BUILD_DIR_DEFAULT="${REPO_ROOT}/build"
readonly MAX_PASSES_DEFAULT=10
readonly PER_FILE_TIMEOUT_DEFAULT=120

read_clang_tidy_field() {
    local field=$1
    local config="${REPO_ROOT}/.clang-tidy"
    [[ -f "${config}" ]] || return 1
    sed -n "s/^${field}:[[:space:]]*//p" "${config}" \
        | sed "s/^['\"]//; s/['\"][[:space:]]*$//"
}

parse_config() {
    local -a scan_roots=()
    SOURCE_ROOTS=()
    HEADER_ROOTS=()
    EXCLUDE_REGEX=""

    local regex
    if regex=$(read_clang_tidy_field "HeaderFilterRegex") && [[ -n "${regex}" ]]; then
        local inner="${regex#'^'}"
        inner="${inner%'/'}"
        inner="${inner#'('}"
        inner="${inner%')'}"
        IFS='|' read -ra scan_roots <<< "${inner}"
    fi
    if [[ ${#scan_roots[@]} -eq 0 ]]; then
        scan_roots=("include/neo" "test" "benchmark")
    fi

    local root
    for root in "${scan_roots[@]}"; do
        case "${root}" in
            include/*) HEADER_ROOTS+=("${root}") ;;
            *)         SOURCE_ROOTS+=("${root}") ;;
        esac
    done

    if regex=$(read_clang_tidy_field "ExcludeHeaderFilterRegex") && [[ -n "${regex}" ]]; then
        EXCLUDE_REGEX="${regex}"
    fi

    # Build absolute-path header-filter for clang-tidy.
    # .clang-tidy uses relative paths like '^(include/neo|test|benchmark)/' but
    # clang-tidy matches against absolute file paths, so we anchor to REPO_ROOT.
    local raw_filter
    if raw_filter=$(read_clang_tidy_field "HeaderFilterRegex") && [[ -n "${raw_filter}" ]]; then
        # Strip leading ^ if present; we'll re-anchor against the repo root
        raw_filter="${raw_filter#'^'}"
        local escaped_root
        escaped_root=$(printf '%s' "${REPO_ROOT}" | sed 's/[.\[\]*+?{}|()^$]/\\&/g')
        HEADER_FILTER="^${escaped_root}/${raw_filter}"
    else
        HEADER_FILTER=".*"
    fi
}

resolve_clang_tidy() {
    if [[ -n "${CLANG_TIDY:-}" ]]; then
        printf '%s\n' "${CLANG_TIDY}"
        return 0
    fi

    local candidate
    for candidate in \
        "/home/cwang/opt/llvm/bin/clang-tidy" \
        "/usr/bin/clang-tidy" \
        "/bin/clang-tidy"
    do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    if command -v clang-tidy >/dev/null 2>&1; then
        command -v clang-tidy
        return 0
    fi

    echo "clang-tidy not found" >&2
    return 1
}

collect_source_files() {
    (
        cd "${REPO_ROOT}"
        local root
        for root in "${SOURCE_ROOTS[@]}"; do
            [[ -d "${root}" ]] || continue
            while IFS= read -r -d '' file; do
                if [[ -n "${EXCLUDE_REGEX}" ]] && printf '%s' "${file}" | grep -qE "${EXCLUDE_REGEX}"; then
                    echo "  [skip] ${file}" >&2
                    continue
                fi
                printf '%s\0' "${file}"
            done < <(find "${root}" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) -print0)
        done
    )
}

collect_header_files() {
    (
        cd "${REPO_ROOT}"
        local root
        for root in "${HEADER_ROOTS[@]}"; do
            [[ -d "${root}" ]] || continue
            while IFS= read -r -d '' file; do
                if [[ -n "${EXCLUDE_REGEX}" ]] && printf '%s' "${file}" | grep -qE "${EXCLUDE_REGEX}"; then
                    echo "  [skip] ${file}" >&2
                    continue
                fi
                printf '%s\0' "${file}"
            done < <(find "${root}" -type f \( -name '*.hpp' -o -name '*.hh' -o -name '*.h' -o -name '*.ipp' \) -print0)
        done
    )
}

snapshot_hash() {
    local files=("$@")
    if [[ ${#files[@]} -eq 0 ]]; then
        printf 'empty\n'
        return 0
    fi

    (
        cd "${REPO_ROOT}"
        sha256sum "${files[@]}"
    ) | sha256sum | awk '{print $1}'
}

# Shared clang-tidy arguments; an empty GCC_TOOLCHAIN uses the compiler's default C++ standard library.
# CLANG_TIDY_CHECKS, when set, is passed as --checks to every run, e.g. to run only the slower analyzer checks.
set_tidy_args() {
    CHECK_ARGS=()
    if [[ -n "${CLANG_TIDY_CHECKS:-}" ]]; then
        CHECK_ARGS=(--checks="${CLANG_TIDY_CHECKS}")
    fi
    SOURCE_ARGS=(
        -quiet
        -p "${BUILD_DIR}"
        --header-filter="${HEADER_FILTER}"
        "${CHECK_ARGS[@]}"
    )
    HEADER_COMPILER_ARGS=(
        -x c++-header
        --std=c++23
        -I"${REPO_ROOT}/include"
    )
    if [[ -n "${GCC_TOOLCHAIN}" ]]; then
        SOURCE_ARGS+=(--extra-arg-before=--gcc-toolchain="${GCC_TOOLCHAIN}")
        HEADER_COMPILER_ARGS=(--gcc-toolchain="${GCC_TOOLCHAIN}" "${HEADER_COMPILER_ARGS[@]}")
    fi
}

run_source_tidy() {
    local mode=$1
    shift
    local files=("$@")
    if [[ ${#files[@]} -eq 0 ]]; then
        return 0
    fi

    local args=("${SOURCE_ARGS[@]}")
    if [[ "${mode}" == "fix" ]]; then
        args+=(--fix --format-style=file)
    fi

    local file rc
    for file in "${files[@]}"; do
        echo "  [${mode}] ${file}"
        rc=0
        (
            cd "${REPO_ROOT}"
            timeout "${PER_FILE_TIMEOUT}" "${CLANG_TIDY_BIN}" "${args[@]}" "${file}"
        ) || rc=$?
        if [[ ${rc} -eq 124 ]]; then
            echo "  TIMEOUT after ${PER_FILE_TIMEOUT}s: ${file}" >&2
        elif [[ ${rc} -ne 0 ]]; then
            return ${rc}
        fi
    done
}

run_header_tidy() {
    local mode=$1
    shift
    local files=("$@")
    if [[ ${#files[@]} -eq 0 ]]; then
        return 0
    fi

    local file
    for file in "${files[@]}"; do
        echo "  [${mode}] ${file}"
        local args=(
            -quiet
            "${file}"
            --header-filter="${HEADER_FILTER}"
            "${CHECK_ARGS[@]}"
        )
        if [[ "${mode}" == "fix" ]]; then
            args+=(--fix --format-style=file)
        fi
        args+=(-- "${HEADER_COMPILER_ARGS[@]}")

        local rc=0
        (
            cd "${REPO_ROOT}"
            timeout "${PER_FILE_TIMEOUT}" "${CLANG_TIDY_BIN}" "${args[@]}"
        ) || rc=$?
        if [[ ${rc} -eq 124 ]]; then
            echo "  TIMEOUT after ${PER_FILE_TIMEOUT}s: ${file}" >&2
        elif [[ ${rc} -ne 0 ]]; then
            return ${rc}
        fi
    done
}

# Only count diagnostics from files inside REPO_ROOT
warning_count() {
    local log_file=$1
    grep ': warning:' "${log_file}" | grep -c "^${REPO_ROOT}/" || true
}

error_count() {
    local log_file=$1
    grep ': error:' "${log_file}" | grep -c "^${REPO_ROOT}/" || true
}

# Check each file once without fixes, JOBS processes at a time; CI uses this mode.
# Fix mode stays serial because fixes from different files may edit the same header.
run_check() {
    local source_count=$1
    shift
    local -a source_files=("${@:1:${source_count}}")
    local -a header_files=("${@:$((source_count + 1))}")

    local log_file
    log_file=$(mktemp)
    local failed=0
    if [[ ${#source_files[@]} -gt 0 ]]; then
        printf '%s\0' "${source_files[@]}" | xargs -0 -P "${JOBS}" -I{} \
            timeout "${PER_FILE_TIMEOUT}" "${CLANG_TIDY_BIN}" "${SOURCE_ARGS[@]}" {} >>"${log_file}" 2>&1 || failed=1
    fi
    if [[ ${#header_files[@]} -gt 0 ]]; then
        printf '%s\0' "${header_files[@]}" | xargs -0 -P "${JOBS}" -I{} \
            timeout "${PER_FILE_TIMEOUT}" "${CLANG_TIDY_BIN}" -quiet {} --header-filter="${HEADER_FILTER}" \
            "${CHECK_ARGS[@]}" -- "${HEADER_COMPILER_ARGS[@]}" >>"${log_file}" 2>&1 || failed=1
    fi
    cat "${log_file}"

    local warnings errors
    warnings=$(warning_count "${log_file}")
    errors=$(error_count "${log_file}")
    rm -f "${log_file}"

    if [[ "${errors}" != "0" || ${failed} -ne 0 ]]; then
        echo "clang-tidy reported ${errors} compiler error(s), or a file failed or timed out (see above)." >&2
        return 2
    fi
    if [[ "${warnings}" != "0" ]]; then
        echo "clang-tidy reported ${warnings} warning(s); run tools/clang-tidy-recursive.sh to fix them." >&2
        return 1
    fi
    echo "clang-tidy is clean."
}

main() {
    local mode=fix
    if [[ "${1:-}" == "--check" ]]; then
        mode=check
        shift
    fi

    CLANG_TIDY_BIN=$(resolve_clang_tidy)
    BUILD_DIR=${BUILD_DIR:-${BUILD_DIR_DEFAULT}}
    # Set GCC_TOOLCHAIN to an empty value to use the compiler's default GCC installation.
    GCC_TOOLCHAIN=${GCC_TOOLCHAIN-}
    MAX_PASSES=${MAX_PASSES:-${MAX_PASSES_DEFAULT}}
    PER_FILE_TIMEOUT=${PER_FILE_TIMEOUT:-${PER_FILE_TIMEOUT_DEFAULT}}
    JOBS=${JOBS:-$(nproc)}

    parse_config
    set_tidy_args
    "${CLANG_TIDY_BIN}" --version

    if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
        echo "compile_commands.json not found under ${BUILD_DIR}" >&2
        return 2
    fi
    if [[ -n "${GCC_TOOLCHAIN}" && ! -d "${GCC_TOOLCHAIN}" ]]; then
        echo "GCC toolchain not found: ${GCC_TOOLCHAIN}" >&2
        return 2
    fi

    local -a source_files=()
    local -a header_files=()
    mapfile -d '' -t source_files < <(collect_source_files)
    mapfile -d '' -t header_files < <(collect_header_files)

    if [[ ${#source_files[@]} -eq 0 && ${#header_files[@]} -eq 0 ]]; then
        echo "No files matched for clang-tidy."
        return 0
    fi

    echo "Found ${#source_files[@]} source file(s), ${#header_files[@]} header file(s)."

    if [[ "${mode}" == "check" ]]; then
        run_check "${#source_files[@]}" "${source_files[@]}" "${header_files[@]}"
        return
    fi

    local -a tracked_files=("${source_files[@]}" "${header_files[@]}")
    local pass
    for ((pass = 1; pass <= MAX_PASSES; ++pass)); do
        local before after
        local fix_log check_log
        before=$(snapshot_hash "${tracked_files[@]}")
        fix_log=$(mktemp)
        check_log=$(mktemp)

        echo "--- pass ${pass}: fix ---"
        if ! {
            run_source_tidy fix "${source_files[@]}"
            run_header_tidy fix "${header_files[@]}"
        } 2>&1 | tee "${fix_log}"; then
            rm -f "${fix_log}" "${check_log}"
            return 2
        fi

        after=$(snapshot_hash "${tracked_files[@]}")

        echo "--- pass ${pass}: check ---"
        if ! {
            run_source_tidy check "${source_files[@]}"
            run_header_tidy check "${header_files[@]}"
        } 2>&1 | tee "${check_log}"; then
            rm -f "${fix_log}" "${check_log}"
            return 2
        fi

        local warnings errors
        warnings=$(warning_count "${check_log}")
        errors=$(error_count "${check_log}")

        if [[ "${errors}" != "0" ]]; then
            echo "clang-tidy reported ${errors} compiler error(s) (see above)." >&2
            rm -f "${fix_log}" "${check_log}"
            return 2
        fi

        if [[ "${warnings}" == "0" ]]; then
            echo "clang-tidy is clean after pass ${pass}."
            rm -f "${fix_log}" "${check_log}"
            return 0
        fi

        if [[ "${before}" == "${after}" ]]; then
            echo "clang-tidy still reports ${warnings} warning(s), but no further fixes applied (see above)." >&2
            rm -f "${fix_log}" "${check_log}"
            return 1
        fi

        rm -f "${fix_log}" "${check_log}"
        echo "clang-tidy pass ${pass} applied fixes; rerunning to check for remaining diagnostics."
    done

    echo "clang-tidy reached MAX_PASSES=${MAX_PASSES} before converging." >&2
    return 1
}

main "$@"

#!/usr/bin/env bash

set -euo pipefail

usage() {
    echo "usage: $0 <llvm-major-version>" >&2
    echo "Adds the apt.llvm.org repository of that LLVM version for the running Ubuntu release, then updates apt." >&2
    echo "Install packages afterwards with their versioned names, e.g. clang-23 or libc++-23-dev." >&2
}

main() {
    if [[ $# -ne 1 ]]; then
        usage
        return 2
    fi
    local version=$1

    local codename
    codename=$(. /etc/os-release && printf '%s\n' "${VERSION_CODENAME}")
    # The key is trusted only for this repository, not for every apt source.
    local keyring=/etc/apt/keyrings/apt.llvm.org.asc
    sudo install -d -m 0755 "$(dirname -- "${keyring}")"
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee "${keyring}" >/dev/null
    printf 'deb [signed-by=%s] https://apt.llvm.org/%s/ llvm-toolchain-%s-%s main\n' \
        "${keyring}" "${codename}" "${codename}" "${version}" \
        | sudo tee "/etc/apt/sources.list.d/llvm-${version}.list" >/dev/null
    sudo apt-get update
}

main "$@"

#!/usr/bin/env bash
# clang-tidy over the stm32 compile database.
#
# clang-tidy parses with clang, which infers the arm-none-eabi target from
# the compiler path recorded in the compile database but does not know where
# that GCC toolchain keeps its C and C++ headers: the system include paths
# are queried from the cross compiler and passed explicitly.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-${repo_root}/software/build/stm32}"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
    echo "tidy_stm32: no compile database in ${build_dir} (configure the stm32 preset first)" >&2
    exit 1
fi

extra_args=()
while IFS= read -r path; do
    extra_args+=("-extra-arg=-isystem${path}")
done < <(arm-none-eabi-g++ -E -x c++ - -v </dev/null 2>&1 |
    sed -n '/#include <...> search starts here:/,/End of search list./p' |
    sed '1d;$d;s/^ //')

run-clang-tidy -p "${build_dir}" -quiet "${extra_args[@]}" \
    "${repo_root}/software/(components|drone_boot|drone_firmware)/"

#!/usr/bin/env bash
# ASCII-only guard for the repo hard rule: no em dashes, arrows, typographic
# quotes or math symbols anywhere in tracked text files. Accented Latin
# letters are tolerated (proper names only). Binary assets are not scanned.
set -euo pipefail
cd "$(dirname "$0")/.."

mapfile -t files < <(git ls-files \
    '*.cpp' '*.hpp' '*.c' '*.h' '*.ld' '*.cmake' '*CMakeLists.txt' '*CMakePresets.json' \
    '*.md' '*.py' '*.gd' '*.sh' '*.yml' '*.yaml' '*.json' '*.clang-format' '*.clang-tidy')

# Everything outside tab/newline/printable ASCII, except the accented Latin
# letters of Latin-1 (multiplication and division signs excluded).
pattern='[^\x{09}\x{0A}\x{0D}\x{20}-\x{7E}\x{C0}-\x{D6}\x{D8}-\x{F6}\x{F8}-\x{FF}]'

if LC_ALL=C.UTF-8 grep -HnP "${pattern}" -- "${files[@]}"; then
    echo "check_ascii: non-ASCII characters found (see above)" >&2
    exit 1
fi
echo "check_ascii: ${#files[@]} files clean"

#!/usr/bin/env bash
# Prints one sha256 over everything the packaged extension is built from:
# the sources, the assets, the build and package manifests, and the two
# schemas its codec is generated from. Hashes the working tree, not a
# commit, so an uncommitted change counts. Read by `pnpm build`, which
# stores it in dist/source-hash, and by install.sh, which compares it with
# the one the installed extension carries: same hash, same extension.
set -euo pipefail
cd "$(dirname "$0")"

{
    find src syntaxes media -type f -not -path 'src/gen/*' -print
    printf '%s\n' package.json pnpm-lock.yaml esbuild.js tsconfig.json .vscodeignore
    printf '%s\n' ../../software/components/protocol/mark4.proto \
        ../../software/components/protocol/gateway.proto
} | LC_ALL=C sort | while IFS= read -r file; do
    # Path and content both: a rename is a change too.
    printf '%s\n' "${file}"
    cat "${file}"
done | sha256sum | cut -d' ' -f1

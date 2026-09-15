#!/usr/bin/env bash
# Keeps the installed Mark4 editor extension equal to the workspace sources.
# Content-based: the build stores a hash of the sources in dist/source-hash,
# the vsix carries it, and this script compares it with the one the
# installed copy carries. Same hash: nothing happens. Anything else (no
# extension, another hash): build, package, install over the existing copy.
# The version in package.json is frozen at 0.0.0 and means nothing; only the
# hash tells the copies apart. Runs as the folderOpen task (needs the
# integrated-terminal environment for the `code` CLI); safe to run by hand.
set -euo pipefail
cd "$(dirname "$0")"

id="tmagne.vscode-mark4"
version=$(node -p "require('./package.json').version")
vsix="vscode-mark4-${version}.vsix"
wanted=$(./source_hash.sh)

# The terminal often inherits a dead VSCODE_IPC_HOOK_CLI socket (WSL
# devcontainer): probe the sockets, newest first, and keep the first live
# one. A dead CLI must not read as "not installed" and reinstall every run.
listed="" live=""
for sock in "${VSCODE_IPC_HOOK_CLI:-}" \
    $(ls -t "/run/user/$(id -u)"/vscode-ipc-*.sock 2>/dev/null); do
    [ -S "${sock}" ] || continue
    if listed=$(VSCODE_IPC_HOOK_CLI="${sock}" code --list-extensions 2>/dev/null); then
        export VSCODE_IPC_HOOK_CLI="${sock}"
        live=1
        break
    fi
done
if [ -z "${live}" ]; then
    echo "mark4 extension: no live code CLI socket, skipping" >&2
    exit 1
fi

# The hash the installed copy carries, empty when none is installed. The
# copy's folder comes from the extensions.json of the host's extensions
# directory (the server's in a container, the client's otherwise).
installed=""
if grep -qx "${id}" <<<"${listed}"; then
    for extensions in "${HOME}"/.vscode-server/extensions "${HOME}"/.vscode/extensions \
        "${HOME}"/.vscode-insiders/extensions; do
        [ -f "${extensions}/extensions.json" ] || continue
        folder=$(node -e '
            const list = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
            const entry = list.find((item) => item.identifier.id === process.argv[2]);
            process.stdout.write(entry ? entry.location.path : "");
        ' "${extensions}/extensions.json" "${id}")
        if [ -n "${folder}" ] && [ -f "${folder}/dist/source-hash" ]; then
            installed=$(cat "${folder}/dist/source-hash")
            break
        fi
    done
fi

if [ "${installed}" = "${wanted}" ]; then
    echo "mark4 extension up to date (${wanted:0:12})"
    exit 0
fi

echo "mark4 extension: installed '${installed:0:12}', workspace ${wanted:0:12}: installing"
pnpm install --frozen-lockfile
pnpm build
pnpm package
# --force: the version never changes, so without it the CLI would keep the
# copy already installed.
code --install-extension "${vsix}" --force
echo "mark4 extension installed: reload the window to activate it"

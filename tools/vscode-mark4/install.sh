#!/usr/bin/env bash
# Builds and installs the Mark4 editor extension when the workspace version
# differs from the installed one. Version-based on purpose: bump "version"
# in package.json to ship a change. A vsix already packaged for the wanted
# version is reused as is. Runs as the folderOpen task (needs the
# integrated-terminal environment for the `code` CLI); safe to run by hand.
set -euo pipefail
cd "$(dirname "$0")"

wanted=$(node -p "require('./package.json').version")
# The terminal often inherits a dead VSCODE_IPC_HOOK_CLI socket (WSL
# devcontainer): probe the sockets, newest first, and keep the first live
# one. A dead CLI must not read as "not installed" and reinstall every run.
listed="" live=""
for sock in "${VSCODE_IPC_HOOK_CLI:-}" \
    $(ls -t "/run/user/$(id -u)"/vscode-ipc-*.sock 2>/dev/null); do
    [ -S "${sock}" ] || continue
    if listed=$(VSCODE_IPC_HOOK_CLI="${sock}" code --list-extensions \
        --show-versions 2>/dev/null); then
        export VSCODE_IPC_HOOK_CLI="${sock}"
        live=1
        break
    fi
done
if [ -z "${live}" ]; then
    echo "mark4 extension: no live code CLI socket, skipping" >&2
    exit 1
fi
installed=$(sed -n 's/^tmagne\.vscode-mark4@//p' <<<"${listed}")

if [ "${installed}" = "${wanted}" ]; then
    echo "mark4 extension ${wanted} already installed"
    exit 0
fi

echo "mark4 extension: installed '${installed:-none}', workspace ${wanted}: installing"
if [ ! -f "vscode-mark4-${wanted}.vsix" ]; then
    pnpm install --frozen-lockfile
    pnpm build
    pnpm package
fi
code --install-extension "vscode-mark4-${wanted}.vsix"
echo "mark4 extension ${wanted} installed: reload the window to activate it"

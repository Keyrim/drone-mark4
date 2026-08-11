#!/usr/bin/env bash
# Builds and installs the Mark4 editor extension when the workspace version
# differs from the installed one. Version-based on purpose: bump "version"
# in package.json to ship a change. Runs as the folderOpen task (needs the
# integrated-terminal environment for the `code` CLI); safe to run by hand.
set -euo pipefail
cd "$(dirname "$0")"

wanted=$(node -p "require('./package.json').version")
installed=$(code --list-extensions --show-versions 2>/dev/null \
    | sed -n 's/^tmagne\.vscode-mark4@//p' || true)

if [ "${installed}" = "${wanted}" ]; then
    echo "mark4 extension ${wanted} already installed"
    exit 0
fi

echo "mark4 extension: installed '${installed:-none}', workspace ${wanted}: installing"
pnpm install --frozen-lockfile
pnpm build
pnpm package
code --install-extension "vscode-mark4-${wanted}.vsix"
echo "mark4 extension ${wanted} installed: reload the window to activate it"

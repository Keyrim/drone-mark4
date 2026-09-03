#!/usr/bin/env bash
# Exports the host X11 cookie for the devcontainer (initializeCommand).
# Native X servers (Xwayland) require MIT-MAGIC-COOKIE auth; WSLg does not,
# so on WSL this leaves an empty file and nothing changes.
set -eu

OUT="$HOME/.drone-mark4-xauth"
touch "$OUT"
chmod 600 "$OUT"

if [ -n "${DISPLAY:-}" ] && command -v xauth >/dev/null 2>&1; then
    TMP="$(mktemp)"
    # Rewrite the address family to FamilyWild (ffff) so the cookie matches
    # no matter what hostname the container sees.
    xauth nlist "$DISPLAY" | sed -e 's/^..../ffff/' | xauth -f "$TMP" nmerge -
    # Write in place: the container bind-mounts this file, so its inode must
    # not change (xauth itself replaces the file on write).
    cat "$TMP" > "$OUT"
    rm -f "$TMP"
fi

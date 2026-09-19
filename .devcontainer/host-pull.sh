#!/usr/bin/env bash
# Refreshes the local copy of the dev image (initializeCommand, runs on the
# HOST before the container is created or started). compose is set to
# pull_policy: missing, so without this step a machine keeps the :latest it
# pulled the first time forever, and a tool added to the Dockerfile since
# (qrencode for the QR pairing of adb_wifi.sh, say) never shows up. The pull
# is a manifest check when the copy is current (a few seconds) and never
# blocks the start: offline, the local copy is used as before. An updated
# copy becomes the running container on the next "Rebuild Container"; the
# named volumes keep the logins and caches through it.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="$(sed -n 's/^ *image: *//p' "$HERE/docker-compose.yml" | head -n1)"

if [ -z "$IMAGE" ]; then
    echo "host-pull: no image in docker-compose.yml, skipping pull" >&2
    exit 0
fi

if ! docker pull "$IMAGE"; then
    echo "host-pull: pull of $IMAGE failed (offline?), keeping the local copy" >&2
fi
exit 0

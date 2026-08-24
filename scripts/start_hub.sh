#!/usr/bin/env bash
# Bring the bench hub up to date and run it. Idempotent: the pages are
# always rebuilt (the hub serves them off disk on every request, so a
# running hub picks them up), the hub process itself is only built and
# started when nothing answers on its port yet.
set -euo pipefail
cd "$(dirname "$0")/.."

(cd software/hub/pages && pnpm install --frozen-lockfile && pnpm build)

HUB_URL="http://127.0.0.1:47810"
# Any HTTP response means a hub owns the port; only a refused connection
# or a silent socket makes curl fail here (no -f: a 404 is still a live
# hub). The timeout matters: a stale VS Code port forward accepts the
# connection with no hub behind it and would hang an unbounded curl.
if curl -s --max-time 2 -o /dev/null "$HUB_URL"; then
    echo "hub already running on $HUB_URL, pages refreshed"
    exit 0
fi

[ -d software/build/desktop ] || (cd software && cmake --preset desktop)
(cd software && cmake --build --preset desktop --target hub)
exec ./software/build/desktop/hub/hub

#!/usr/bin/env python3
"""Build one app by name, then run it.

The app and its run command line come from apps.json at the repo root; the
build goes through build_app.py so run and build agree on the (preset,
target) pair. The run command replaces this process, so Ctrl-C reaches the
app directly. An app with no "run" field (firmware, drone_replay) is
launched from its debug configuration instead.

    scripts/run_app.py hub
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_app import REPO_ROOT, build, load_apps  # noqa: E402


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    name = sys.argv[1]
    apps = load_apps()
    if name not in apps:
        print(f"run_app: no app named '{name}' in apps.json", file=sys.stderr)
        return 1
    app = apps[name]
    if "run" not in app:
        print(f"run_app: '{name}' declares no run command (use its debug launch)",
              file=sys.stderr)
        return 1
    code = build(app)
    if code != 0:
        return code
    argv = [app["run"][0] if os.path.isabs(app["run"][0]) else
            os.path.join(REPO_ROOT, app["run"][0])] + app["run"][1:]
    if not os.path.isfile(argv[0]):
        # ctest and friends live on the PATH, not in the tree
        argv[0] = app["run"][0]
    os.chdir(REPO_ROOT)
    os.execvp(argv[0], argv)


if __name__ == "__main__":
    sys.exit(main())

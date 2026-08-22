#!/usr/bin/env python3
"""Build one or more apps by name.

An app is a (cmake preset, cmake target) pair declared in software/apps.json:
naming the app is enough, the preset comes with it. The preset is
configured first when its build directory does not exist yet, then the
target is built. Several names build sequentially, first failure stops.

    scripts/build_app.py --list
    scripts/build_app.py drone_sim hub
"""

import argparse
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOFTWARE_DIR = os.path.join(REPO_ROOT, "software")


def load_apps() -> dict:
    with open(os.path.join(SOFTWARE_DIR, "apps.json"), encoding="utf-8") as file:
        return {app["name"]: app for app in json.load(file)["apps"]}


def build(app: dict) -> int:
    preset = app["cmakePreset"]
    if not os.path.isfile(os.path.join(SOFTWARE_DIR, "build", preset, "CMakeCache.txt")):
        code = subprocess.call(["cmake", "--preset", preset], cwd=SOFTWARE_DIR)
        if code != 0:
            return code
    return subprocess.call(
        ["cmake", "--build", "--preset", preset, "--target", app["target"]], cwd=SOFTWARE_DIR
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*", help="apps to build, in order")
    parser.add_argument("--list", action="store_true", help="name the declared apps")
    args = parser.parse_args()

    apps = load_apps()
    if args.list or not args.names:
        for app in apps.values():
            print(f"{app['name']:<16} {app['cmakePreset']} / {app['target']}")
        return 0
    for name in args.names:
        if name not in apps:
            print(f"build_app: no app named '{name}' in apps.json", file=sys.stderr)
            return 1
        code = build(apps[name])
        if code != 0:
            return code
    return 0


if __name__ == "__main__":
    sys.exit(main())

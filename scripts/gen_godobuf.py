#!/usr/bin/env python3
"""Generate the GDScript codec of a .proto with godobuf, inside headless Godot.

godobuf's command line script (sim-godot/addons/godobuf/godobuf_cmdln.gd)
exits 0 whatever happens and only says "Compilation failed." on its output,
so this wrapper is what turns a broken generation into a failed build: the
output must exist, be non-empty and be newer than the input. Paths are made
absolute, res:// paths break the generator.

    scripts/gen_godobuf.py --godot godot --project sim-godot \\
        --input software/components/protocol/mark4.proto \\
        --output sim-godot/scripts/gen/mark4.gd
"""

import argparse
import os
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True, help="godot 4 binary")
    parser.add_argument("--project", required=True, help="Godot project holding addons/godobuf")
    parser.add_argument("--input", required=True, help=".proto to compile")
    parser.add_argument("--output", required=True, help=".gd to write")
    args = parser.parse_args()

    project = os.path.abspath(args.project)
    proto = os.path.abspath(args.input)
    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    if os.path.exists(output):
        os.remove(output)

    done = subprocess.run(
        [
            args.godot,
            "--headless",
            "--path",
            project,
            "--script",
            "addons/godobuf/godobuf_cmdln.gd",
            f"--input={proto}",
            f"--output={output}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    failed = (
        done.returncode != 0
        or "Compilation failed" in done.stdout
        or not os.path.isfile(output)
        or os.path.getsize(output) == 0
    )
    if failed:
        sys.stderr.write(done.stdout)
        sys.stderr.write(done.stderr)
        print(f"gen_godobuf: godobuf did not produce {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

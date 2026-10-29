"""Run FASTBuild on the generated .bff."""

import json
import os
import subprocess
import sys
from collections.abc import Sequence
from typing import Any

from marlin.paths import EnginePaths


def load_manifest(paths: EnginePaths) -> dict[str, Any]:
    """Return the manifest of the last ``marlin generate``, or an empty dict if there is none."""
    if not paths.generated_manifest.is_file():
        return {}
    return json.loads(paths.generated_manifest.read_text(encoding="utf-8"))


def run_fbuild(paths: EnginePaths, targets: Sequence[str], extra_args: Sequence[str] = ()) -> int:
    """Build *targets* with the environment the manifest records; return FBuild's exit code.

    FBuild allows one instance per working directory; ``-wait`` queues this one behind another, such as the
    projects Visual Studio builds in parallel, instead of failing.
    """
    if not paths.fbuild_exe.is_file():
        print(f"marlin: FBuild not found at {paths.fbuild_exe}", file=sys.stderr)
        return 1
    if not paths.generated_bff.is_file():
        print(f"marlin: {paths.generated_bff} does not exist; run `marlin generate` first.", file=sys.stderr)
        return 1
    env = {**os.environ, **load_manifest(paths).get("env", {})}
    command = [str(paths.fbuild_exe), "-config", str(paths.generated_bff), "-wait", *extra_args, *targets]
    return subprocess.run(command, check=False, cwd=paths.root, env=env).returncode

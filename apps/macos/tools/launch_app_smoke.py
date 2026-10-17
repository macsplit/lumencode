#!/usr/bin/env python3
"""Launch the built app from a neutral cwd, then close it after five seconds.

This checks native app startup, not mouse/keyboard behavior or visual correctness.
Use the rendered GUI audit for content and responsiveness checks.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--app", type=Path, required=True)
parser.add_argument("--project", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
binary = args.app.resolve() / "Contents/MacOS/LumenCode"
with tempfile.TemporaryDirectory(prefix="lumencode-app-smoke-") as directory:
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([str(binary), str(args.project.resolve())], cwd=directory, stdout=log, stderr=log)
        try:
            code = process.wait(timeout=5)
            alive = False
        except subprocess.TimeoutExpired:
            alive = True
            process.terminate()
            code = process.wait(timeout=5)
        log.seek(0)
        diagnostics = log.read().decode("utf-8", errors="replace")
result = dict(launched_and_remained_running=alive, exit_after_shutdown=code, diagnostics=diagnostics, method="Production app executable launched with project folder from temporary cwd; terminated after five seconds")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result))
raise SystemExit(not alive)

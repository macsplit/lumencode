#!/usr/bin/env python3
"""Verify a copied runtime launches without developer library paths."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--target-macos", help="also fail if a library requires a newer macOS version")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="lumencode-runtime-") as temporary:
    root = Path(temporary)
    runtime = root / "Moved Runtime"
    shutil.copytree(args.runtime, runtime)
    helper = runtime / "bin/lumencode-engine"
    bad_dependencies = []
    for file in [helper, *sorted((runtime / "lib").iterdir())]:
        lines = subprocess.check_output(["otool", "-L", str(file)], text=True).splitlines()[1:]
        for line in lines:
            dependency = line.strip().split(" (compatibility")[0]
            if not dependency.startswith(("@loader_path/", "@executable_path/", "/System/", "/usr/lib/")):
                bad_dependencies.append(dict(file=file.name, dependency=dependency))
    fixture = root / "Source With Spaces.swift"
    fixture.write_text("func answer(value: Int) -> Int { return value + 1 }\n")
    environment = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    process = subprocess.run([str(helper), "--dump-file", str(fixture)], cwd=root, env=environment, text=True, capture_output=True, timeout=5)
    analysis = json.loads(process.stdout) if process.returncode == 0 else {}
    symbols = analysis.get("symbols", [])
    relocation_passed = not bad_dependencies and len(symbols) == 1 and symbols[0]["name"] == "answer" and symbols[0]["parameters"] == [{"name": "value", "type": "Int"}]
    minimums = json.loads((runtime / "minimum-macos.json").read_text())
    def version(value):
        return tuple((list(map(int, value.split("."))) + [0, 0, 0])[:3])
    incompatible = {name: minimum for name, minimum in minimums.items()
                    if args.target_macos and version(minimum) > version(args.target_macos)}
    passed = relocation_passed and not incompatible
    result = dict(passed=passed, relocation_passed=relocation_passed, exit_code=process.returncode,
                  external_dependencies=bad_dependencies, libraries=len(list((runtime / "lib").iterdir())),
                  relocated_path_with_spaces=True, minimum_macos=minimums, target_macos=args.target_macos,
                  incompatible_dependencies=incompatible)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result))
raise SystemExit(not passed)

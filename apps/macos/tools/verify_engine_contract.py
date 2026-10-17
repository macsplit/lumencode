#!/usr/bin/env python3
"""Check original analysis contracts and optionally compare live original dumps."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import os

sys.dont_write_bytecode = True

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cli", type=Path, required=True)
parser.add_argument("--reference-cli", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
upstream = repo / "Engine/Upstream"
spec = importlib.util.spec_from_file_location("original_regression", upstream / "tools/regression_sweep.py")
original = importlib.util.module_from_spec(spec)
spec.loader.exec_module(original)
manifest = json.loads((upstream / "tests/fixtures/baseline/manifest.json").read_text())
paths = sorted({(upstream / c["root"] / c["file"]).resolve() for c in manifest["cases"]})
issues, differences = [], []
# QHash-backed relation/link arrays are unordered upstream. Give both processes
# the same Qt hash seed so comparison does not mistake iteration order for drift.
environment = dict(os.environ, QT_HASH_SEED="0")
for path in paths:
    output = subprocess.check_output([str(args.cli.resolve()), "--dump-file", str(path)], text=True, timeout=15, env=environment)
    analysis = json.loads(output)
    original.validate_analysis_contract(analysis, path, issues, context="swift-helper")
    if args.reference_cli:
        reference = json.loads(subprocess.check_output([str(args.reference_cli.resolve()), "--dump-file", str(path)], text=True, timeout=15, env=environment))
        if analysis != reference:
            differences.append(str(path.relative_to(upstream)))
result = dict(files=len(paths), contract_issues=issues, compared_to_original=bool(args.reference_cli), qt_hash_seed=0, differing_files=differences)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(dict(files=len(paths), contract_issues=len(issues), differing_files=len(differences))))
raise SystemExit(bool(issues or differences))

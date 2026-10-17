#!/usr/bin/env python3
"""Run unchanged upstream corpus/damage probes, comparing every dump to its CLI.

Fetch with Engine/Upstream/tools/fetch_corpus.py --dest /tmp/lumencode-public-corpus.
The reference CLI must be built from Engine/upstream.json's revision.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import threading
import tempfile
import time

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("mode", choices=("corpus", "damage"))
parser.add_argument("--cli", type=Path, required=True)
parser.add_argument("--reference-cli", type=Path, required=True)
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--jobs", type=int, default=4)
parser.add_argument("--files", type=int, default=60, help="upstream damage probe files per language")
args = parser.parse_args()
# Keep compiler relinks from invalidating signed executable pages during a long
# sweep. Each run exercises an immutable CLI/runtime snapshot, not a build output
# that another verification command may replace.
source_cli = args.cli.resolve()
source_reference = args.reference_cli.resolve()
snapshot = tempfile.TemporaryDirectory(prefix="lumencode-resilience-runtime-")
snapshot_root = Path(snapshot.name)
args.cli = snapshot_root / "swift-cli"
args.reference_cli = snapshot_root / "original-cli"
shutil.copy2(source_cli, args.cli)
shutil.copy2(source_reference, args.reference_cli)
shutil.copytree(repo / ".build/EngineRuntime", snapshot_root / "EngineRuntime")
digests = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in (
    ("swift_cli", args.cli), ("reference_cli", args.reference_cli),
    ("engine", snapshot_root / "EngineRuntime/bin/lumencode-engine")
)}
sys.path.insert(0, str(repo / "Engine/Upstream/tools"))
corpus = importlib.import_module("corpus_scan")
damage = importlib.import_module("damage_probe")
environment = dict(os.environ, QT_HASH_SEED="0", LUMENCODE_ENGINE=str(snapshot_root / "EngineRuntime/bin/lumencode-engine"))
lock = threading.Lock()
comparisons = 0
failures = []
differences = []


def run_cli(cli, path):
    try:
        result = subprocess.run([str(cli.resolve()), "--dump-file", str(path)],
                                capture_output=True, text=True, timeout=20, env=environment)
        if result.returncode:
            return None, f"exit {result.returncode}: {result.stderr[-1000:]}"
        return json.loads(result.stdout), None
    except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        return None, str(error)


def paired_dump(path):
    global comparisons
    started = time.monotonic()
    actual, actual_error = run_cli(args.cli, path)
    elapsed = (time.monotonic() - started) * 1000
    expected, reference_error = run_cli(args.reference_cli, path)
    with lock:
        comparisons += 1
        if actual_error or reference_error:
            failures.append(dict(path=str(path), swift=actual_error, reference=reference_error))
        elif actual != expected:
            # Compare whole JSON values; no fields, paths or array ordering discarded.
            keys = sorted(key for key in actual.keys() | expected.keys() if actual.get(key) != expected.get(key))
            differences.append(dict(path=str(path), differing_keys=keys))
        if comparisons % 500 == 0:
            print(f"paired dumps: {comparisons}, differences: {len(differences)}, failures: {len(failures)}", flush=True)
    if actual_error:
        return dict(path=str(path), status="failure", ms=elapsed, stderr=actual_error)
    return dict(path=str(path), status="ok", ms=elapsed, parsed=actual)


corpus.dump = paired_dump
corpus.CLI_PATH = args.cli.resolve()
damage.dump = lambda path: paired_dump(path).get("parsed")
args.output.parent.mkdir(parents=True, exist_ok=True)
report = args.output.with_name(args.output.stem + "-upstream.json")
sys.argv = [sys.argv[0], "--root", str(args.root.resolve()), "--jobs", str(args.jobs), "--save", str(report)]
if args.mode == "damage":
    sys.argv.extend(["--files", str(args.files)])
started = time.monotonic()
code = (corpus if args.mode == "corpus" else damage).main()
if comparisons == 0:
    failures.append(dict(error="No dumps were exercised"))
coverage = json.loads(report.read_text())
if args.mode == "damage" and not coverage:
    failures.append(dict(error="No eligible damage mutations were exercised"))
manifest = json.loads((repo / "Engine/Upstream/tools/corpus.json").read_text())
revisions = []
for entry in manifest["repos"]:
    checkout = args.root / entry["name"]
    head = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture_output=True, text=True)
    revisions.append(dict(name=entry["name"], expected=entry["commit"], actual=head.stdout.strip(), matches=head.stdout.strip() == entry["commit"]))
revision_failures = [entry for entry in revisions if not entry["matches"]]
result = dict(mode=args.mode, engine_revision=json.loads((repo / "Engine/upstream.json").read_text())["revision"],
              swift_cli=str(source_cli), reference_cli=str(source_reference), executable_sha256=digests,
              qt_hash_seed=0, comparisons=comparisons, differences=differences, failures=failures,
              corpus_revisions=revisions, all_corpus_revisions_match=not revision_failures,
              elapsed_seconds=round(time.monotonic() - started, 2), upstream_report=str(report))
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({key: result[key] for key in ("mode", "comparisons", "elapsed_seconds", "all_corpus_revisions_match")}
                 | dict(differences=len(differences), failures=len(failures))))
snapshot.cleanup()
raise SystemExit(bool(code or differences or failures or revision_failures))

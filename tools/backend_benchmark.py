#!/usr/bin/env python3
"""Measure process-inclusive CLI latency and the oversized-file boundary."""
import argparse
import json
import pathlib
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cli", type=pathlib.Path, required=True)
parser.add_argument("--original", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
parser.add_argument("--output", type=pathlib.Path, required=True)
args = parser.parse_args()
stress = pathlib.Path("/private/tmp/lumencode-audit-stress")
stress.mkdir(exist_ok=True)
for i in range(500):
    (stress / f"file{i:04}.js").write_text(f"export function helper{i}(value) {{ return value + {i}; }}\nexport function run{i}() {{ return helper{i}(1); }}\n")
generated = pathlib.Path("/private/tmp/lumencode-audit-generated")
generated.mkdir(exist_ok=True)
large = generated / "large.js"
large.write_text("function example() { return 1; }\n" * 75000)
root = args.original.resolve()
scenarios = {
    "swift_signature_dump": ["--dump-file", str(root / "tests/fixtures/baseline/swift_signatures/Greeter.swift")],
    "stress_single_file_dump": ["--dump-file", str(stress / "file0001.js")],
    "stress_project_selection": [str(stress), "--path", str(stress / "file0001.js")],
    "oversize_js_dump": ["--dump-file", str(large)],
}
results = {}
for name, arguments in scenarios.items():
    samples = []
    for _ in range(1 if name.startswith("oversize") else 5):
        start = time.perf_counter()
        try:
            proc = subprocess.run([str(args.cli.resolve()), *arguments], capture_output=True, text=True, timeout=3)
            data = json.loads(proc.stdout) if proc.returncode == 0 else {}
            selected = data.get("selectedFileData", data)
            samples.append(dict(ms=(time.perf_counter()-start)*1000, exit=proc.returncode, mode=selected.get("analysisSourceMode"), symbols=len(selected.get("symbols", [])), summary=selected.get("summary"), output_bytes=len(proc.stdout)))
        except subprocess.TimeoutExpired:
            samples.append(dict(ms=(time.perf_counter()-start)*1000, timed_out=True))
    results[name] = dict(samples=samples, median_ms=statistics.median(s["ms"] for s in samples))
    print(name, round(results[name]["median_ms"], 2))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(results, indent=2) + "\n")

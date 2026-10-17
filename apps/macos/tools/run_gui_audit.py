#!/usr/bin/env python3
"""Build an isolated GUI audit executable using the real app source.

First run swift build --scratch-path /tmp/lumencode-audit-build.
Then run this script to compile, and the printed command to launch the GUI.
"""
import argparse
import json
import os
import pathlib
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("/tmp/lumencode-audit-build"))
parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("/tmp/lumencode-gui-results"))
parser.add_argument("--run", action="store_true", help="launch audit and fail on native UI update warnings")
parser.add_argument("--max-heartbeat-ms", type=float, help="optional main-runloop pause gate")
args = parser.parse_args()
repo = pathlib.Path(__file__).resolve().parent.parent
scratch = pathlib.Path("/tmp/lumencode-gui-audit")
scratch.mkdir(exist_ok=True)
stress = pathlib.Path("/tmp/lumencode-audit-stress")
stress.mkdir(exist_ok=True)
for i in range(500):
    (stress / f"file{i:04}.js").write_text(f"export function helper{i}(value) {{ return value + {i}; }}\nexport function run{i}() {{ return helper{i}(1); }}\n")
app = (repo / "Sources/LumencodeSwiftApp/LumencodeApp.swift").read_text()
assert app.count("@main") == 1
source = scratch / "Audit.swift"
source.write_text(app.replace("@main", "", 1) + "\n" + (repo / "tools/gui_audit.swift").read_text())
binary = scratch / "gui-audit"
subprocess.run(["swiftc", "-parse-as-library", "-g", "-module-cache-path", str(scratch / "module-cache"), "-I", str(args.build / "debug"), str(source), "-L", str(args.build / "debug"), "-lLumencodeCore", "-o", str(binary)], check=True)
print(f"Run: {binary} {args.output.resolve()} {repo}")
if args.run:
    args.output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    if args.max_heartbeat_ms is not None:
        environment["LUMENCODE_AUDIT_HEARTBEAT_BUDGET_MS"] = str(args.max_heartbeat_ms)
    result = subprocess.run([str(binary), str(args.output.resolve()), str(repo)],
                            capture_output=True, text=True, env=environment)
    log = result.stdout + result.stderr
    (args.output / "gui.log").write_text(log)
    print(log, end="")
    warnings = [line for line in log.splitlines() if any(marker in line.lower() for marker in (
        "reentrant operation", "modifying state during view update", "publishing changes from within view updates"
    ))]
    (args.output / "gui-diagnostics.json").write_text(json.dumps(dict(
        process_exit_code=result.returncode, native_ui_warnings=warnings,
        warning_gate_passed=not warnings
    ), indent=2) + "\n")
    if warnings:
        print("Native UI warning gate failed:", "\n".join(warnings))
    raise SystemExit(result.returncode or bool(warnings))

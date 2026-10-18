#!/usr/bin/env python3
"""Capture complete shared-engine JSON for byte-identical inputs across OSes.

capture uses the existing corpus scanner's candidate policy. compare preserves
every JSON field and array order; only absolute snapshot-root path prefixes and
their path separators are mapped to a common root. Gzip keeps raw evidence small.
"""
import argparse
import concurrent.futures
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def read(path):
    with gzip.open(path, "rt", encoding="utf-8") if str(path).endswith(".gz") else open(path, encoding="utf-8") as stream:
        return json.load(stream)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(path, "wt", encoding="utf-8") if str(path).endswith(".gz") else open(path, "w", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def normalize(value, root):
    if isinstance(value, dict):
        return {key: normalize(item, root) for key, item in value.items()}
    if isinstance(value, list):
        return [normalize(item, root) for item in value]
    if isinstance(value, str):
        for prefix in {root.rstrip("/\\"), root.replace("\\", "/").rstrip("/")}:
            for lead in ("", "file://"):
                match = lead + prefix
                if value == match or value.startswith(match + "/") or value.startswith(match + "\\"):
                    return lead + "<CORPUS>" + value[len(match):].replace("\\", "/")
    return value


def first_difference(left, right, path="$", limit=12):
    if left == right:
        return []
    if type(left) is not type(right):
        return [path + ": type/value differs"]
    if isinstance(left, dict):
        result = [path + "." + key + ": missing key" for key in sorted(left.keys() ^ right.keys())]
        for key in sorted(left.keys() & right.keys()):
            result += first_difference(left[key], right[key], path + "." + key)
            if len(result) >= limit:
                break
        return result[:limit]
    if isinstance(left, list):
        result = [path + ": array length differs"] if len(left) != len(right) else []
        for index, (a, b) in enumerate(zip(left, right)):
            result += first_difference(a, b, f"{path}[{index}]")
            if len(result) >= limit:
                break
        return result[:limit]
    return [path + ": value differs"]


def unordered_for_diagnosis(value):
    """Classify ordering differences only; never changes the strict verdict."""
    if isinstance(value, dict):
        return {key: unordered_for_diagnosis(item) for key, item in value.items()}
    if isinstance(value, list):
        return sorted((unordered_for_diagnosis(item) for item in value), key=lambda item: json.dumps(item, sort_keys=True, ensure_ascii=False))
    return value


def capture(args):
    import corpus_scan
    paths = corpus_scan.discover(args.root, None)
    if args.manifest:
        wanted = json.loads(args.manifest.read_text(encoding="utf-8"))
        paths = [args.root / name for name in wanted]
    if not paths:
        raise SystemExit("No candidate inputs selected")
    environment = dict(os.environ, QT_HASH_SEED="0", QT_QPA_PLATFORM="offscreen")

    def one(path):
        started = time.monotonic()
        name = path.relative_to(args.root).as_posix()
        try:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            result = subprocess.run([str(args.cli.resolve()), "--dump-file", str(path.resolve())], capture_output=True, encoding="utf-8", timeout=30, env=environment)
            if result.returncode:
                return name, {"sha256": digest, "error": result.stderr[-2000:], "exitCode": result.returncode}
            return name, {"sha256": digest, "payload": json.loads(result.stdout), "elapsedMs": (time.monotonic() - started) * 1000}
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            return name, {"error": str(error)}

    started = time.monotonic()
    files = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, value in pool.map(one, paths):
            files[name] = value
            if len(files) % 500 == 0:
                print(f"Captured {len(files)}/{len(paths)}", flush=True)
    errors = [name for name, value in files.items() if "error" in value]
    write(args.output, {"platform": platform.platform(), "root": str(args.root.resolve()), "cli": str(args.cli.resolve()), "files": files, "errors": errors, "elapsedSeconds": time.monotonic() - started})
    print(json.dumps({"files": len(files), "errors": len(errors), "output": str(args.output)}), flush=True)
    return bool(errors)


def compare(args):
    reference = read(args.reference)
    selection = set(json.loads(args.manifest.read_text(encoding="utf-8"))) if args.manifest else None
    if not reference["files"] or selection == set():
        raise SystemExit("Cannot compare an empty reference or selection")
    results = []
    for path in args.captures:
        actual = read(path)
        reference_names, actual_names = set(reference["files"]), set(actual["files"])
        missing = sorted((selection - reference_names) | (selection - actual_names)) if selection is not None else sorted(reference_names ^ actual_names)
        different_inputs, differences, failures = [], [], []
        languages, modes = {}, {}
        for name in sorted(reference_names & actual_names & (selection if selection is not None else reference_names)):
            left, right = reference["files"][name], actual["files"][name]
            if "error" in left or "error" in right:
                failures.append(name)
                continue
            if left["sha256"] != right["sha256"]:
                different_inputs.append(name)
                continue
            normalized_left = normalize(left["payload"], reference["root"])
            normalized_right = normalize(right["payload"], actual["root"])
            if normalized_left != normalized_right:
                differences.append({"file": name, "fields": first_difference(normalized_left, normalized_right), "sameValuesAfterSortingArrays": unordered_for_diagnosis(normalized_left) == unordered_for_diagnosis(normalized_right)})
            language = right["payload"].get("language", "unknown")
            mode = right["payload"].get("analysisSourceMode", "unknown")
            languages[language] = languages.get(language, 0) + 1
            modes[mode] = modes.get(mode, 0) + 1
        results.append({"reference": reference["platform"], "actual": actual["platform"], "files": len(selection) if selection is not None else len(actual["files"]), "excludedFromReference": sorted(reference_names - selection) if selection is not None else [], "excludedFromActual": sorted(actual_names - selection) if selection is not None else [], "missingFiles": missing, "differentInputs": different_inputs, "failures": failures, "differences": differences, "languages": languages, "modes": modes})
    passed = all(not any(item[key] for key in ("missingFiles", "differentInputs", "failures", "differences")) for item in results)
    write(args.output, {"passed": passed, "normalization": "Only absolute snapshot-root path prefixes/separators; no fields or array ordering discarded", "comparisons": results})
    print(json.dumps({"passed": passed, "comparisons": [{"files": r["files"], **{key: len(r[key]) for key in ("missingFiles", "differentInputs", "differences", "failures")}} for r in results]}))
    return not passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("capture")
    command.add_argument("--cli", type=Path, required=True)
    command.add_argument("--root", type=Path, required=True)
    command.add_argument("--manifest", type=Path)
    command.add_argument("--jobs", type=int, default=4)
    command.add_argument("--output", type=Path, required=True)
    command = commands.add_parser("compare")
    command.add_argument("--reference", type=Path, required=True)
    command.add_argument("--manifest", type=Path, help="Compare this explicit subset; record every excluded file")
    command.add_argument("captures", type=Path, nargs="+")
    command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    return capture(args) if args.command == "capture" else compare(args)


if __name__ == "__main__":
    raise SystemExit(main())

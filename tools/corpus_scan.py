#!/usr/bin/env python3
"""Fast whole-corpus scan through `lumencode-cli --dump-file`.

Where tools/regression_sweep.py drives the interactive selection path on a small
sample, this runs the crash-isolated dump path over *every* candidate file in a
corpus (in parallel), and reports:

- crashes, timeouts and non-JSON output
- payload contract violations (same validator as regression_sweep.py)
- per-language coverage metrics: how many files yield symbols, members,
  calls/calledBy, dependencies, routes, signatures, and which analysis mode
  (ast / recovered / heuristic) was used
- per-language timing (p50 / p95 / max)

A report can be saved and later compared against, so a parser change can be
checked for regressions ("file X used to give 14 symbols, now gives 3") across
thousands of real files in a few seconds.

Usage:
    python3 tools/corpus_scan.py                          # scan $LUMENCODE_CORPUS
    python3 tools/corpus_scan.py --root ~/Code --jobs 8
    python3 tools/corpus_scan.py --save /tmp/before.json
    python3 tools/corpus_scan.py --compare /tmp/before.json
    python3 tools/corpus_scan.py --lang python --verbose-issues
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import regression_sweep as sweep  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
CLI_PATH = REPO_ROOT / "build" / "bin" / "lumencode-cli"

# Files worth scanning even if lumencode does not (yet) support them, so the
# report shows the coverage gap instead of silently skipping it.
SCAN_EXTENSIONS = sweep.SUPPORTED_EXTENSIONS | {
    ".mjs", ".cjs", ".mts", ".cts", ".go", ".rb", ".kt", ".kts", ".sh", ".bash", ".zsh",
    ".vb", ".sql", ".c", ".h", ".scss", ".less", ".htm", ".phtml",
}

EXCLUDED_PARTS = sweep.EXCLUDED_PARTS - {"bin", "build", "vendor"} | {"node_modules", ".git"}

TIMEOUT_SECONDS = 20


def default_root() -> Path:
    env = os.environ.get("LUMENCODE_CORPUS")
    if env:
        return Path(env)
    return Path.home() / ".cache" / "lumencode-corpus"


def discover(root: Path, max_per_repo: int | None) -> list[Path]:
    files: list[Path] = []
    per_repo: Counter = Counter()
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SCAN_EXTENSIONS:
            continue
        rel = path.relative_to(root)
        if set(rel.parts) & EXCLUDED_PARTS:
            continue
        name = path.name.lower()
        if name.startswith(".") or name == "package-lock.json" or ".min." in name:
            continue
        repo = rel.parts[0] if len(rel.parts) > 1 else "."
        if max_per_repo is not None and per_repo[repo] >= max_per_repo:
            continue
        per_repo[repo] += 1
        files.append(path)
    return files


def dump(path: Path) -> dict:
    started = time.monotonic()
    try:
        proc = subprocess.run([str(CLI_PATH), "--dump-file", str(path)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, timeout=TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired:
        return {"path": str(path), "status": "timeout", "ms": TIMEOUT_SECONDS * 1000}
    elapsed = (time.monotonic() - started) * 1000
    if proc.returncode != 0:
        return {"path": str(path), "status": "crash", "rc": proc.returncode,
                "stderr": proc.stderr.strip()[-400:], "ms": elapsed}
    try:
        parsed = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        return {"path": str(path), "status": "bad_json", "message": str(exc), "ms": elapsed}
    return {"path": str(path), "status": "ok", "ms": elapsed, "parsed": parsed}


def summarize_file(parsed: dict) -> dict:
    symbols = parsed.get("symbols", []) or []
    flat = list(sweep.iter_symbols(symbols))
    calls = sum(len(s.get("calls", []) or []) for s in flat)
    called_by = sum(len(s.get("calledBy", []) or []) for s in flat)
    callables = [s for s in flat if s.get("kind") in sweep.CALLABLE_KINDS]
    with_params = sum(1 for s in callables if s.get("parameters"))
    typed_returns = sum(1 for s in callables
                        if any((r.get("text") or "") not in ("", "none") for r in (s.get("returns") or [])))
    return {
        "language": parsed.get("language", "") or "unknown",
        "mode": parsed.get("analysisSourceMode", ""),
        "astErrors": bool(parsed.get("analysisHasAstErrors")),
        "partial": bool(parsed.get("analysisPartial")),
        "topSymbols": len(symbols),
        "allSymbols": len(flat),
        "members": len(flat) - len(symbols),
        "callables": len(callables),
        "callablesWithParams": with_params,
        "callablesWithReturns": typed_returns,
        "calls": calls,
        "calledBy": called_by,
        "dependencies": len(parsed.get("dependencies", []) or []),
        "routes": len(parsed.get("routes", []) or []),
        "quickLinks": len(parsed.get("quickLinks", []) or []),
        "relatedFiles": len(parsed.get("relatedFiles", []) or []),
        "summary": parsed.get("summary", ""),
    }


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(pct / 100.0 * (len(ordered) - 1))))
    return ordered[index]


def aggregate(files: dict[str, dict]) -> dict[str, dict]:
    by_lang: dict[str, list[dict]] = defaultdict(list)
    for entry in files.values():
        by_lang[entry.get("language", "unknown")].append(entry)
    result = {}
    for lang, entries in sorted(by_lang.items()):
        ok = [e for e in entries if e["status"] == "ok"]
        times = [e["ms"] for e in entries]
        n = max(1, len(ok))

        def share(predicate):
            return round(100.0 * sum(1 for e in ok if predicate(e)) / n, 1)

        callables = sum(e["callables"] for e in ok)
        result[lang] = {
            "files": len(entries),
            "failures": len(entries) - len(ok),
            "modes": dict(Counter(e["mode"] for e in ok)),
            "pctWithSymbols": share(lambda e: e["topSymbols"] > 0),
            "pctWithMembers": share(lambda e: e["members"] > 0),
            "pctWithRelations": share(lambda e: e["calls"] + e["calledBy"] > 0),
            "pctWithDependencies": share(lambda e: e["dependencies"] > 0),
            "pctWithRoutes": share(lambda e: e["routes"] > 0),
            "pctWithQuickLinks": share(lambda e: e["quickLinks"] > 0),
            "pctAstErrors": share(lambda e: e["astErrors"]),
            "symbolsPerFile": round(sum(e["allSymbols"] for e in ok) / n, 1),
            "relationsPerFile": round(sum(e["calls"] + e["calledBy"] for e in ok) / n, 1),
            "pctCallablesWithParams": round(100.0 * sum(e["callablesWithParams"] for e in ok) / max(1, callables), 1),
            "pctCallablesWithReturns": round(100.0 * sum(e["callablesWithReturns"] for e in ok) / max(1, callables), 1),
            "msP50": round(percentile(times, 50), 1),
            "msP95": round(percentile(times, 95), 1),
            "msMax": round(max(times) if times else 0.0, 1),
        }
    return result


def print_table(languages: dict[str, dict]) -> None:
    cols = [("files", "files"), ("fail", "failures"), ("sym%", "pctWithSymbols"), ("mem%", "pctWithMembers"),
            ("rel%", "pctWithRelations"), ("dep%", "pctWithDependencies"), ("route%", "pctWithRoutes"),
            ("err%", "pctAstErrors"), ("sym/f", "symbolsPerFile"), ("rel/f", "relationsPerFile"),
            ("param%", "pctCallablesWithParams"), ("p95ms", "msP95"), ("maxms", "msMax")]
    header = f"{'language':<10}" + "".join(f"{label:>8}" for label, _ in cols) + "  modes"
    print(header)
    print("-" * len(header))
    for lang, stats in languages.items():
        row = f"{lang[:10]:<10}" + "".join(f"{stats[key]:>8}" for _, key in cols)
        modes = ",".join(f"{mode or '-'}:{count}" for mode, count in sorted(stats["modes"].items()))
        print(f"{row}  {modes}")


def compare(previous: dict, current: dict, threshold: int) -> list[str]:
    lines: list[str] = []
    prev_files = previous.get("files", {})
    for path, now in sorted(current["files"].items()):
        before = prev_files.get(path)
        if not before:
            continue
        if before["status"] == "ok" and now["status"] != "ok":
            lines.append(f"NEW FAILURE {now['status']}: {path}")
            continue
        if before["status"] != "ok" or now["status"] != "ok":
            continue
        for key in ("allSymbols", "calls", "calledBy", "dependencies", "routes"):
            drop = before.get(key, 0) - now.get(key, 0)
            if drop >= threshold or (before.get(key, 0) > 0 and now.get(key, 0) == 0):
                lines.append(f"DROP {key} {before.get(key, 0)} -> {now.get(key, 0)}: {path}")
    for lang, stats in current["languages"].items():
        old = previous.get("languages", {}).get(lang)
        if not old:
            lines.append(f"NEW LANGUAGE {lang}: {stats['files']} files")
            continue
        deltas = []
        for key in ("pctWithSymbols", "pctWithRelations", "pctWithDependencies", "symbolsPerFile",
                    "relationsPerFile", "pctCallablesWithParams", "msP95"):
            delta = round(stats[key] - old.get(key, 0), 1)
            if delta:
                deltas.append(f"{key} {old.get(key, 0)}->{stats[key]}")
        if deltas:
            lines.append(f"{lang}: " + "; ".join(deltas))
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=default_root())
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--max-per-repo", type=int, default=None)
    parser.add_argument("--lang", action="append", default=None, help="only report these languages")
    parser.add_argument("--save", type=Path, default=None, help="write the full report as JSON")
    parser.add_argument("--compare", type=Path, default=None, help="compare against a saved report")
    parser.add_argument("--drop-threshold", type=int, default=3)
    parser.add_argument("--verbose-issues", action="store_true", help="print every contract issue")
    args = parser.parse_args()

    if not CLI_PATH.exists():
        print(f"CLI not found at {CLI_PATH}; build with: cmake --build build --target lumencode-cli")
        return 1
    if not args.root.exists():
        print(f"corpus root {args.root} does not exist; run tools/fetch_corpus.py first")
        return 1

    paths = discover(args.root, args.max_per_repo)
    print(f"scanning {len(paths)} files under {args.root} with {args.jobs} jobs", flush=True)

    files: dict[str, dict] = {}
    issues: list[dict] = []
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(dump, path): path for path in paths}
        for done, future in enumerate(as_completed(futures), start=1):
            path = futures[future]
            outcome = future.result()
            rel = str(path.relative_to(args.root))
            entry = {"status": outcome["status"], "ms": round(outcome["ms"], 1)}
            if outcome["status"] == "ok":
                parsed = outcome["parsed"]
                entry.update(summarize_file(parsed))
                file_issues: list[dict] = []
                sweep.validate_analysis_contract(parsed, path, file_issues, context="corpus")
                entry["issues"] = len(file_issues)
                issues.extend(file_issues)
            else:
                entry["language"] = "failed:" + path.suffix.lower().lstrip(".")
                entry["detail"] = outcome.get("stderr") or outcome.get("message") or ""
                print(f"  {outcome['status'].upper()}: {rel} {entry['detail'][:200]}", flush=True)
            files[rel] = entry
            if done % 500 == 0:
                print(f"  ... {done}/{len(paths)} ({time.monotonic() - started:.1f}s)", flush=True)

    if args.lang:
        wanted = set(args.lang)
        files = {path: entry for path, entry in files.items() if entry.get("language") in wanted}
        issues = [issue for issue in issues
                  if files.get(str(Path(issue["file"]).relative_to(args.root)))]

    report = {"root": str(args.root), "elapsedSeconds": round(time.monotonic() - started, 1),
              "languages": aggregate(files), "files": files,
              "issueCounts": dict(Counter(issue["type"] for issue in issues))}
    print()
    print_table(report["languages"])
    print()
    print(f"elapsed {report['elapsedSeconds']}s; contract issues: {len(issues)} {report['issueCounts']}")
    if args.verbose_issues:
        for issue in issues:
            print("  ", json.dumps(issue)[:400])
    else:
        for issue in issues[:15]:
            print("  ", json.dumps(issue)[:300])

    slow = sorted(((entry["ms"], path) for path, entry in files.items()), reverse=True)[:5]
    print("slowest:", ", ".join(f"{path} {ms:.0f}ms" for ms, path in slow))

    if args.compare:
        previous = json.loads(args.compare.read_text())
        diff = compare(previous, report, args.drop_threshold)
        print()
        print(f"comparison against {args.compare}: {len(diff)} notable changes")
        for line in diff:
            print("  " + line)
    if args.save:
        args.save.write_text(json.dumps(report, indent=1))
        print(f"report saved to {args.save}")
    failures = sum(1 for entry in files.values() if entry["status"] != "ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

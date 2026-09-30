#!/usr/bin/env python3
"""Project-index metrics over the pinned corpus.

For each corpus project: builds the index cold (fresh cache) and warm (cache
reused), then reports

  - files, call sites, cross-file edges and the share of call sites resolved
  - edges by evidence (import / qualifier / package / unique-name)
  - cross-file coverage per language: share of files with at least one
    cross-file edge in or out
  - reciprocity: for a sample of edges, whether the live per-file view
    (--index-relations, what the GUI shows) lists the callee under the
    caller's Calls and the caller under the callee's Called By

Usage:
  tools/index_metrics.py [--corpus-root /path/to/corpus] [--projects a,b] [--sample 40]
                         [--save out.json]
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI = REPO_ROOT / "build" / "bin" / "lumencode-cli"
DEFAULT_CORPUS = Path(os.environ.get("LUMENCODE_CORPUS", "/home/user/corpus"))


def run_cli(cli: Path, args: list[str], env: dict) -> str:
    result = subprocess.run([str(cli), *args], capture_output=True, text=True, env=env, timeout=1800)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed: {result.stderr[-500:]}")
    return result.stdout


def relation_names(entries: list[dict], field: str) -> set[tuple[str, str]]:
    names = set()
    for entry in entries:
        for relation in entry.get(field, []):
            names.add((relation["name"], relation["path"]))
    return names


def measure_project(cli: Path, root: Path, sample: int, rng: random.Random) -> dict:
    cache_dir = Path(tempfile.mkdtemp(prefix="lumencode-index-"))
    env = dict(os.environ, XDG_CACHE_HOME=str(cache_dir))
    try:
        started = time.monotonic()
        stats = json.loads(run_cli(cli, ["--index-project", str(root)], env))
        cold_ms = int((time.monotonic() - started) * 1000)
        started = time.monotonic()
        warm = json.loads(run_cli(cli, ["--index-project", str(root)], env))
        warm_ms = int((time.monotonic() - started) * 1000)
        edges = [json.loads(line) for line in run_cli(cli, ["--index-project", str(root), "--index-edges"], env).splitlines() if line.strip()]

        facts_languages: dict[str, str] = {}
        cache_files = list((cache_dir / "lumencode" / "index").glob("*.json"))
        if cache_files:
            cache = json.loads(cache_files[0].read_text())
            for path, facts in cache.get("files", {}).items():
                facts_languages[os.path.relpath(path, root)] = facts.get("language", "")

        linked = set()
        for edge in edges:
            linked.add(edge["from"])
            linked.add(edge["to"])
        coverage: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
        for path, language in facts_languages.items():
            coverage[language][1] += 1
            if path in linked:
                coverage[language][0] += 1

        # Reciprocity through the live per-file view.
        checked = 0
        forward_ok = 0
        backward_ok = 0
        truncated = 0
        views: dict[str, list[dict]] = {}

        def view(path: str) -> list[dict]:
            if path not in views:
                views[path] = json.loads(run_cli(cli, ["--index-project", str(root), "--index-relations", str(root / path)], env))
            return views[path]

        named = [edge for edge in edges if edge["caller"] != "(top level)"]
        for edge in rng.sample(named, min(sample, len(named))):
            checked += 1
            callee_name = edge["callee"].split(".")[-1]
            caller_name = edge["caller"]
            if (callee_name, edge["to"]) in relation_names(view(edge["from"]), "calls"):
                forward_ok += 1
            callee_view = view(edge["to"])
            if (caller_name, edge["from"]) in relation_names(callee_view, "calledBy"):
                backward_ok += 1
            elif any(entry.get("calledByTotal") for entry in callee_view):
                truncated += 1

        return {
            "files": stats["files"],
            "failed": stats["failed"],
            "definitions": stats["definitions"],
            "callSites": stats["callSites"],
            "crossFileEdges": stats["crossFileEdges"],
            "resolvedShare": round(stats["crossFileEdges"] / stats["callSites"], 3) if stats["callSites"] else 0,
            "edgesByEvidence": stats["edgesByEvidence"],
            "coldMs": cold_ms,
            "warmMs": warm_ms,
            "warmReused": warm["reusedFromCache"],
            "coverageByLanguage": {language: {"linked": counts[0], "files": counts[1]} for language, counts in coverage.items()},
            "reciprocity": {"sampled": checked, "callsListed": forward_ok, "calledByListed": backward_ok,
                            "calledByTruncated": truncated},
        }
    finally:
        shutil.rmtree(cache_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--projects", default="")
    parser.add_argument("--sample", type=int, default=40)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--save", type=Path)
    args = parser.parse_args()

    projects = [name for name in args.projects.split(",") if name] or sorted(
        entry.name for entry in args.corpus_root.iterdir() if entry.is_dir())
    rng = random.Random(args.seed)
    results = {}
    totals_coverage: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    totals = collections.Counter()
    for name in projects:
        root = args.corpus_root / name
        print(f"[index] {name} ...", file=sys.stderr, flush=True)
        try:
            result = measure_project(args.cli, root, args.sample, rng)
        except Exception as error:  # noqa: BLE001 - report and continue
            print(f"[index] {name}: {error}", file=sys.stderr, flush=True)
            results[name] = {"error": str(error)}
            continue
        results[name] = result
        reciprocity = result["reciprocity"]
        print(f"[index] {name}: {result['files']} files, {result['crossFileEdges']} edges "
              f"({result['resolvedShare']:.0%} of call sites), cold {result['coldMs']} ms, warm {result['warmMs']} ms, "
              f"reciprocity {reciprocity['callsListed']}/{reciprocity['calledByListed']} of {reciprocity['sampled']}",
              file=sys.stderr, flush=True)
        for language, counts in result["coverageByLanguage"].items():
            totals_coverage[language][0] += counts["linked"]
            totals_coverage[language][1] += counts["files"]
        for key in ("files", "failed", "callSites", "crossFileEdges", "coldMs", "warmMs"):
            totals[key] += result[key]
        totals["sampled"] += reciprocity["sampled"]
        totals["callsListed"] += reciprocity["callsListed"]
        totals["calledByListed"] += reciprocity["calledByListed"]
        totals["calledByTruncated"] += reciprocity["calledByTruncated"]

    summary = {
        "totals": dict(totals),
        "coverageByLanguage": {
            language: {"linked": counts[0], "files": counts[1], "share": round(counts[0] / counts[1], 3) if counts[1] else 0}
            for language, counts in sorted(totals_coverage.items())
        },
        "projects": results,
    }
    text = json.dumps(summary, indent=2)
    if args.save:
        args.save.write_text(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

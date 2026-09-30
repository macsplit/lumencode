#!/usr/bin/env python3
"""Measure how far a local syntax error spreads through the analysis.

LumenCode's goal for broken-but-parseable code is that damage stays local: a
half-typed line inside one function should cost (at most) that function, not
the rest of the file. This probe checks that on real files:

1. Take a corpus file that analyses cleanly and has several top-level symbols.
2. Inject a syntax error into the *body* of one symbol (never its header).
3. Re-analyse and count how many of the *other* symbols (name + kind) survive.

Several mutation styles are applied, e.g. a stray garbage line, an unclosed
call ``broken(1,`` and an unclosed string. The report gives the retention rate
per language and mutation, and lists the worst files so they can be minimised
into fixtures.

Usage:
    python3 tools/damage_probe.py                      # $LUMENCODE_CORPUS
    python3 tools/damage_probe.py --lang python --files 40 --show-worst 10
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import corpus_scan  # noqa: E402

CLI_PATH = corpus_scan.CLI_PATH

MUTATIONS = {
    "garbage_line": "@@@ ### $$$ ;;; ~~~",
    "unclosed_call": "broken(1, ",
    "unclosed_string": "x = \"unterminated",
    # Not an insertion: the last line holding only a closing brace inside the
    # target is deleted (brace languages only; skipped where there is none).
    "missing_close": None,
}

# Languages where "garbage" differs a lot per grammar get the same mutations; the
# point is to see real parser behaviour, not to be clever per language.
# Languages without a Tree-sitter grammar but with a structural (block-aware)
# parser, which should be just as damage-tolerant.
STRUCTURAL_HEURISTIC_LANGUAGES = {"vbnet", "sql", "shell", "objc"}

SKIP_LANGUAGES = {"json", "html", "css", "unknown"}


def dump(path: Path) -> dict | None:
    try:
        proc = subprocess.run([str(CLI_PATH), "--dump-file", str(path)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=20, check=False)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None


def symbol_ids(symbols: list[dict]) -> set[tuple[str, str]]:
    return {(s.get("kind", ""), s.get("name", "")) for s in symbols if s.get("name")}


def _unused_member_ids(symbols: list[dict]) -> set[tuple[str, str, str]]:
    ids = set()
    for symbol in symbols:
        for member in symbol.get("members", []) or []:
            if member.get("name"):
                ids.add((symbol.get("name", ""), member.get("kind", ""), member.get("name", "")))
    return ids


def units_for(symbols: list[dict]) -> tuple[str, list[dict]]:
    """The level at which damage is measured: top-level symbols, or - for
    files that hold a single type (typical Java/C#/PHP) - that type's members."""
    if len(symbols) >= 3:
        return "top", symbols
    best = max(symbols, key=lambda s: len(s.get("members", []) or []), default=None)
    if best and len(best.get("members", []) or []) >= 3:
        return "member", best.get("members", []) or []
    return "none", []


def unit_ids(level: str, symbols: list[dict]) -> set[tuple[str, str]]:
    if level == "top":
        return symbol_ids(symbols)
    ids = set()
    for symbol in symbols:
        ids |= symbol_ids(symbol.get("members", []) or [])
    return ids


def pick_target(units: list[dict], line_count: int, rng: random.Random) -> tuple[dict, int] | None:
    """Pick a unit with a multi-line body and a line strictly inside it."""
    ordered = sorted((s for s in units if s.get("line", 0) > 0), key=lambda s: s["line"])
    candidates = []
    for index, symbol in enumerate(ordered):
        start = symbol["line"]
        next_start = ordered[index + 1]["line"] - 1 if index + 1 < len(ordered) else line_count
        end = min(symbol.get("endLine") or next_start, next_start)
        if end - start >= 3:
            candidates.append((symbol, start, end))
    if not candidates:
        return None
    # Prefer a unit in the first half so there is plenty of file after the damage.
    first_half = [c for c in candidates if c[1] <= line_count / 2] or candidates
    symbol, start, end = rng.choice(first_half)
    return symbol, rng.randint(start + 1, max(start + 1, end - 1)), start, end


def probe_file(path: Path, rng_seed: int) -> list[dict]:
    rng = random.Random(rng_seed)
    clean = dump(path)
    structural = clean and clean.get("language") in STRUCTURAL_HEURISTIC_LANGUAGES
    if not clean or clean.get("analysisHasAstErrors") or (clean.get("analysisSourceMode") != "ast" and not structural):
        return []
    language = clean.get("language", "unknown")
    symbols = clean.get("symbols", []) or []
    if language in SKIP_LANGUAGES:
        return []
    level, units = units_for(symbols)
    if level == "none":
        return []
    text = path.read_text(errors="replace")
    lines = text.split("\n")
    picked = pick_target(units, len(lines), rng)
    if not picked:
        return []
    target, line_no, target_start, target_end = picked
    target_id = (target.get("kind", ""), target.get("name", ""))
    others = unit_ids(level, symbols) - {target_id}
    if not others:
        return []

    results = []
    workdir = Path(tempfile.mkdtemp(prefix="lumencode-damage-"))
    try:
        for mutation, payload in MUTATIONS.items():
            mutated = list(lines)
            if payload is None:
                closers = [index for index in range(target_start, min(target_end, len(lines)))
                           if lines[index].strip() in ("}", "};", "},", "})", "});")]
                if not closers:
                    continue
                del mutated[closers[-1]]
            else:
                indent = mutated[line_no - 1][: len(mutated[line_no - 1]) - len(mutated[line_no - 1].lstrip())]
                mutated.insert(line_no - 1, indent + payload)
            copy = workdir / path.name
            copy.write_text("\n".join(mutated))
            damaged = dump(copy)
            if damaged is None:
                results.append({"file": str(path), "language": language, "mutation": mutation, "level": level,
                                "status": "failed", "retained": 0.0})
                continue
            damaged_symbols = damaged.get("symbols", []) or []
            kept = others & unit_ids(level, damaged_symbols)
            results.append({
                "file": str(path),
                "language": language,
                "mutation": mutation,
                "level": level,
                "status": "ok",
                "mode": damaged.get("analysisSourceMode", ""),
                "line": line_no,
                "target": target.get("name", ""),
                "retained": len(kept) / len(others),
                "lost": sorted(f"{kind}:{name}" for kind, name in others - kept)[:8],
                "extra": len(unit_ids(level, damaged_symbols) - unit_ids(level, symbols)),
            })
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=corpus_scan.default_root())
    parser.add_argument("--files", type=int, default=60, help="max files probed per language")
    parser.add_argument("--lang", action="append", default=None)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--show-worst", type=int, default=5)
    parser.add_argument("--save", type=Path, default=None)
    args = parser.parse_args()

    by_suffix: dict[str, list[Path]] = defaultdict(list)
    for path in corpus_scan.discover(args.root, None):
        by_suffix[path.suffix.lower()].append(path)
    rng = random.Random(args.seed)
    chosen: list[Path] = []
    for suffix, paths in sorted(by_suffix.items()):
        rng.shuffle(paths)
        chosen.extend(paths[: args.files * 3])  # many get filtered (not clean / too few symbols)
    print(f"probing up to {len(chosen)} candidate files", flush=True)

    results: list[dict] = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for batch in pool.map(lambda item: probe_file(item[1], args.seed + item[0]), enumerate(chosen)):
            results.extend(batch)

    per_lang_files: dict[str, set] = defaultdict(set)
    filtered = []
    for result in results:
        if args.lang and result["language"] not in args.lang:
            continue
        files = per_lang_files[result["language"]]
        if result["file"] not in files and len(files) >= args.files:
            continue
        files.add(result["file"])
        filtered.append(result)

    table: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for result in filtered:
        table[(result["language"], result["mutation"])].append(result)

    print(f"\n{'language':<10}{'mutation':<18}{'files':>6}{'kept':>8}{'intact':>8}{'extra':>7}  modes")
    for (language, mutation), rows in sorted(table.items()):
        kept = sum(r["retained"] for r in rows) / len(rows)
        extra = sum(r.get("extra", 0) for r in rows) / len(rows)
        intact = sum(1 for r in rows if r["retained"] == 1.0) / len(rows)
        modes = defaultdict(int)
        for row in rows:
            modes[row.get("mode", row["status"])] += 1
        print(f"{language:<10}{mutation:<18}{len(rows):>6}{kept * 100:>7.1f}%{intact * 100:>7.0f}%{extra:>7.1f}  "
              + ",".join(f"{m}:{c}" for m, c in sorted(modes.items())))

    overall = sum(r["retained"] for r in filtered) / max(1, len(filtered))
    print(f"\noverall symbol retention: {overall * 100:.1f}% over {len(filtered)} probes")
    if args.show_worst:
        print("\nworst probes:")
        for row in sorted(filtered, key=lambda r: r["retained"])[: args.show_worst]:
            print(f"  {row['retained'] * 100:5.1f}% {row['mutation']:<16} {row['file']}:{row.get('line')} "
                  f"(in {row.get('target')}) lost {row.get('lost')}")
    if args.save:
        args.save.write_text(json.dumps(filtered, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

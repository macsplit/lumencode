#!/usr/bin/env python3
"""Fetch the pinned public source corpus described in tools/corpus.json.

The corpus is what tools/corpus_scan.py and tools/regression_sweep.py --corpus-root
run against when a developer's own project tree is not available (for example in a
cloud container). Every repo is fetched shallowly at a pinned commit so runs are
reproducible across machines.

Usage:
    python3 tools/fetch_corpus.py                 # into $LUMENCODE_CORPUS or ~/.cache/lumencode-corpus
    python3 tools/fetch_corpus.py --dest /tmp/c   # explicit destination
    python3 tools/fetch_corpus.py --only flask gin
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST = REPO_ROOT / "tools" / "corpus.json"


def default_dest() -> Path:
    env = os.environ.get("LUMENCODE_CORPUS")
    if env:
        return Path(env)
    return Path.home() / ".cache" / "lumencode-corpus"


def git(*args: str, cwd: Path | None = None) -> str:
    env = dict(os.environ, GIT_LFS_SKIP_SMUDGE="1")
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()


def fetch(entry: dict, dest: Path) -> str:
    target = dest / entry["name"]
    commit = entry["commit"]
    if (target / ".git").exists():
        try:
            if git("rev-parse", "HEAD", cwd=target) == commit:
                return "present"
        except subprocess.CalledProcessError:
            pass
    else:
        target.mkdir(parents=True, exist_ok=True)
        git("init", "-q", cwd=target)
        git("remote", "add", "origin", entry["repo"], cwd=target)
    sparse = entry.get("sparse")
    if sparse:
        git("config", "remote.origin.promisor", "true", cwd=target)
        git("config", "remote.origin.partialclonefilter", "blob:none", cwd=target)
        git("sparse-checkout", "set", "--no-cone", *sparse, cwd=target)
        git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit, cwd=target)
    else:
        git("fetch", "-q", "--depth", "1", "origin", commit, cwd=target)
    git("checkout", "-q", "--force", "FETCH_HEAD", cwd=target)
    return "fetched"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", type=Path, default=default_dest())
    parser.add_argument("--only", nargs="*", default=None, help="corpus entry names to fetch")
    args = parser.parse_args()

    entries = json.loads(MANIFEST.read_text())["repos"]
    if args.only:
        entries = [entry for entry in entries if entry["name"] in set(args.only)]
    args.dest.mkdir(parents=True, exist_ok=True)
    failures = 0
    for index, entry in enumerate(entries, start=1):
        print(f"[{index}/{len(entries)}] {entry['name']} @ {entry['commit'][:10]} ...", end=" ", flush=True)
        try:
            print(fetch(entry, args.dest), flush=True)
        except subprocess.CalledProcessError as exc:
            failures += 1
            print(f"FAILED: {exc.stderr.strip()[:300]}", flush=True)
    print(f"corpus at {args.dest} ({len(entries) - failures}/{len(entries)} ok)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

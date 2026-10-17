#!/usr/bin/env python3
"""Import an explicit, clean upstream revision without modifying its sources.

Shared existing grammars stay in Sources/LumencodeNativeBridge/Vendor.
All inputs and hashes are recorded so subsequent updates are reviewable.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=Path("../lumencode"))
parser.add_argument("--revision", required=True)
args = parser.parse_args()
source = args.source.resolve()
repo = Path(__file__).resolve().parent.parent
revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", args.revision], text=True).strip()
head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
if revision != head or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True).strip():
    raise SystemExit("Source must be clean and checked out at the explicitly requested revision")
destination = repo / "Engine/Upstream"
records = []
paths = [Path("LICENSE"), Path("THIRD_PARTY_NOTICES.md")]
paths.extend(Path("tools") / name for name in (
    "regression_sweep.py", "corpus_scan.py", "damage_probe.py", "fetch_corpus.py", "corpus.json"
))
paths.extend(p.relative_to(source) for p in (source / "tests/fixtures/baseline").rglob("*") if p.is_file())
for name in ("symbolparser", "projectindex", "weblinks", "httpclients", "agentqueries"):
    paths.extend(Path("src") / (name + ext) for ext in (".cpp", ".h"))
for name in ("cpp", "go"):
    grammar = Path("third_party") / ("tree-sitter-" + name)
    paths.extend(p.relative_to(source) for p in (source / grammar / "src").rglob("*") if p.is_file() and p.suffix in (".c", ".h"))
    paths.extend(p.relative_to(source) for p in (source / grammar).glob("*LICENSE*") if p.is_file())
for relative in paths:
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / relative, target)
    records.append(dict(upstream=str(relative), local=str(target.relative_to(repo)), sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
shared = repo / "Sources/LumencodeNativeBridge/Vendor"
for grammar in shared.iterdir():
    original_name = "tree-sitter" if grammar.name == "tree-sitter-runtime" else grammar.name
    for file in grammar.rglob("*"):
        if file.is_file():
            original = source / "third_party" / original_name / file.relative_to(grammar)
            if original.is_file() and file.suffix in (".c", ".h", ".cc"):
                digest = hashlib.sha256(file.read_bytes()).hexdigest()
                if digest != hashlib.sha256(original.read_bytes()).hexdigest():
                    raise SystemExit(f"Shared vendor input differs from upstream: {file}")
                records.append(dict(upstream=str(original.relative_to(source)), local=str(file.relative_to(repo)), sha256=digest))
(repo / "Engine/upstream.json").write_text(json.dumps(dict(repository="https://github.com/macsplit/lumencode", revision=revision, files=records), indent=2) + "\n")
print(f"Pinned engine {revision}: {len(records)} verified inputs")

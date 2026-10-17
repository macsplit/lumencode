#!/usr/bin/env python3
"""Check the ordinary-checkout layout and optional one-time migration invariants."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--migration-baseline', action='store_true', help='also require unchanged pre-migration engine/Qt sources and preserved ancestry')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
issues = []
for path in ('engine/src/symbolparser.cpp', 'engine/helper/main.cpp', 'third_party/tree-sitter/lib/src/lib.c',
             'apps/qt/src/main.cpp', 'apps/macos/Package.swift', 'tests/fixtures/baseline/manifest.json'):
    if not (repo / path).is_file(): issues.append('Missing ' + path)
for path in ('apps/macos/Engine', 'apps/macos/Vendor', 'apps/macos/Sources/LumencodeNativeBridge/Vendor', '.gitmodules'):
    if (repo / path).exists(): issues.append('Obsolete copy/dependency ' + path)
vendor = json.loads((repo / 'third_party/provenance.json').read_text())
for entry in vendor['files']:
    path = repo / entry['path']
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
        issues.append('Vendored input differs: ' + entry['path'])
git_tree = subprocess.run(['git', '-C', str(repo), 'ls-files', '--stage'], capture_output=True, text=True)
if git_tree.returncode == 0:
    issues.extend('Git link: ' + line for line in git_tree.stdout.splitlines() if line.startswith('160000 '))
    replaces = subprocess.run(['git', '-C', str(repo), 'for-each-ref', 'refs/replace/'], capture_output=True, text=True, check=True)
    if replaces.stdout.strip(): issues.append('Unexpected replacement refs')
if args.migration_baseline:
    origin = json.loads((repo / 'engine/origin.json').read_text())
    for key in ('migration_source_sha256', 'qt_application_source_sha256'):
        for path, expected in origin.get(key, {}).items():
            if hashlib.sha256((repo / path).read_bytes()).hexdigest() != expected:
                issues.append('Migration changed source: ' + path)
    for tip in (origin['qt'], origin['imported_swift']):
        if subprocess.run(['git', '-C', str(repo), 'merge-base', '--is-ancestor', tip, 'HEAD']).returncode:
            issues.append('Missing imported ancestry: ' + tip)
result = dict(passed=not issues, issues=issues, vendored_inputs=len(vendor['files']), migration_baseline=args.migration_baseline)
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
raise SystemExit(bool(issues))

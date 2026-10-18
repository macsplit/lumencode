#!/usr/bin/env python3
"""Check Windows runtime/resources and shared-source provenance (no GUI launch)."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(package: Path, repo: Path) -> dict:
    required = [
        'LumenCode.Windows.exe', 'LumenCode.Windows.dll',
        'LumenCode.Windows.deps.json', 'LumenCode.Windows.runtimeconfig.json',
        'LumenCode.Windows.pri', 'App.xbf', 'MainWindow.xbf',
        'coreclr.dll', 'Microsoft.UI.Xaml.dll',
        'EngineRuntime/lumencode-engine.exe', 'EngineRuntime/lumencode-cli.exe',
        'EngineRuntime/Qt5Core.dll', 'EngineRuntime/Qt5Concurrent.dll',
        'EngineRuntime/Qt5Gui.dll', 'EngineRuntime/Qt5Widgets.dll',
        'EngineRuntime/vcruntime140.dll', 'EngineRuntime/platforms/qoffscreen.dll',
        'EngineRuntime/licenses/LumenCode-GPL-3.0.txt',
        'EngineRuntime/licenses/THIRD_PARTY_NOTICES.md',
        'EngineRuntime/source-provenance.json',
    ]
    issues = []
    for name in required:
        path = package / name
        if not path.is_file() or path.stat().st_size == 0:
            issues.append('Missing or empty package file: ' + name)
    native_commit = None
    try:
        provenance = json.loads((package / 'EngineRuntime/source-provenance.json').read_text(encoding='utf-8-sig'))
        if not isinstance(provenance, dict):
            raise ValueError('Source provenance must be an object')
        expected_sources = {
            p.relative_to(repo).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (repo / 'engine').rglob('*') if p.is_file() and p.suffix in ('.cpp', '.h')
        }
        if provenance.get('engine_sources') != expected_sources:
            issues.append('Packaged engine source hashes differ from this checkout')
        expected_vendor = json.loads((repo / 'third_party/provenance.json').read_text(encoding='utf-8'))
        if provenance.get('third_party') != expected_vendor:
            issues.append('Packaged grammar/runtime provenance differs from this checkout')
        native_commit = provenance.get('monorepo_commit')
        if not native_commit:
            issues.append('Packaged native build commit is missing')
    except (OSError, ValueError) as error:
        issues.append('Cannot read package provenance: ' + str(error))
    return dict(passed=not issues, issues=issues, checked_files=len(required),
                native_build_commit=native_commit,
                limitations='File/resource and source provenance gate; GUI startup, rendering and input are separate checks.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = verify(args.package.resolve(), Path(__file__).resolve().parent.parent)
    serialized = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding='utf-8')
    print(serialized, end='')
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())

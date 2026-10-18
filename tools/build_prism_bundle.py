#!/usr/bin/env python3
"""Rebuild the pinned offline Prism bundle from an upstream source checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
REVISION = '76dde18a575831c91491895193f56081ac08b0c5'
LANGUAGES = ['markup','css','javascript','typescript','jsx','tsx','php','python',
             'c','cpp','csharp','java','swift','objectivec','rust','go','ruby',
             'kotlin','bash','sql','visual-basic','qml','scss','less','json',
             'yaml','toml','ini','markdown','diff']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Prism v1.30.0 checkout')
    args = parser.parse_args()
    source = args.source.resolve()
    revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
    if revision != REVISION:
        raise SystemExit('Expected the pinned Prism v1.30.0 revision')
    if subprocess.check_output(['git','status','--porcelain'],cwd=source,text=True).strip():
        raise SystemExit('Prism source checkout must be clean')
    # Upstream resolves required/optional/modifying grammar order, including JSX/TSX.
    order = json.loads(subprocess.check_output(['node','-e',
        "const p=process.argv[1], wanted=JSON.parse(process.argv[2]); const out=[]; "
        "require(p+'/dependencies')(require(p+'/components'),wanted,[]).load(id=>out.push(id)); "
        "process.stdout.write(JSON.stringify(out));", str(source),json.dumps(LANGUAGES)],text=True))
    files = ['components/prism-core.js'] + [f'components/prism-{name}.js' for name in order]
    output = ROOT/'apps/web'
    parts = ['/* PrismJS 1.30.0 · MIT · See prism-LICENSE.txt and prism-provenance.json. */\n']
    for filename in files:
        parts.append(f'\n/* Upstream: {filename} */\n' + (source/filename).read_text() + '\n;\n')
    bundle = ''.join(parts)
    (output/'prism-vendor.js').write_text(bundle)
    (output/'prism-LICENSE.txt').write_bytes((source/'LICENSE').read_bytes())
    manifest = {'repository':'https://github.com/PrismJS/prism','version':'1.30.0',
        'revision':REVISION,'license':'MIT','requested_languages':LANGUAGES,'load_order':order,
        'source_sha256':{filename:hashlib.sha256((source/filename).read_bytes()).hexdigest() for filename in files},
        'bundle_sha256':hashlib.sha256(bundle.encode()).hexdigest()}
    (output/'prism-provenance.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'bytes':len(bundle.encode()),'grammars':len(order),'revision':REVISION}))


if __name__ == '__main__': main()

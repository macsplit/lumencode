#!/usr/bin/env python3
"""Prepare, sign and notarize a direct-download universal macOS application.

Each stage is explicit. Credentials stay in Keychain. Nothing is published.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parent.parent
MAGICS = {bytes.fromhex(v) for v in ('feedface', 'feedfacf', 'cefaedfe', 'cffaedfe', 'cafebabe', 'bebafeca')}


def run(command, **kwargs):
    try:
        return subprocess.run(list(map(str, command)), check=True, **kwargs)
    except subprocess.CalledProcessError as error:
        if kwargs.get('capture_output'):
            print(error.stdout or '', error.stderr or '', file=sys.stderr)
        raise


def write(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def mach_o_files(app):
    paths = []
    for path in app.rglob('*'):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as handle:
                if handle.read(4) in MAGICS:
                    paths.append(path)
    return paths


def runtime_check(app, output):
    for path in mach_o_files(app):
        architecture = subprocess.check_output(['lipo', '-archs', str(path)], text=True).strip()
        build = subprocess.check_output(['vtool', '-show-build', str(path)], text=True)
        minimums = re.findall(r'^\s*minos\s+(\d+(?:\.\d+)*)', build, re.MULTILINE)
        if set(architecture.split()) != {'arm64', 'x86_64'} or len(minimums) != 2 or any(
                tuple((list(map(int, minimum.split('.'))) + [0, 0])[:3]) > (14, 0, 0)
                for minimum in minimums):
            raise SystemExit(f'Architecture or macOS 14 compatibility check failed: {path}')
    run(['python3', REPO / 'tools/verify_runtime.py', '--runtime',
         app / 'Contents/Resources/EngineRuntime', '--target-macos', '14.0', '--output', output])


def prepare(args):
    output, scratch, qt = args.output.resolve(), args.scratch.resolve(), args.qt.resolve()
    if output.exists():
        raise SystemExit('Output already exists; choose a fresh staging directory')
    if not re.fullmatch(r'\d+\.\d+\.\d+', args.version) or not re.fullmatch(r'\d+', args.build_number):
        raise SystemExit('Use a three-part numeric version and numeric build number')
    output.mkdir(parents=True)
    environment = dict(os.environ, QT5_PREFIX=str(qt), LUMENCODE_MACOS_ARCHITECTURES='x86_64 arm64')
    run(['xcodebuild', '-project', REPO / 'apps/macos/lumencode-swift.xcodeproj',
         '-scheme', 'lumencode-swift', '-configuration', 'Release',
         '-destination', 'generic/platform=macOS', '-derivedDataPath', scratch,
         'CODE_SIGNING_ALLOWED=NO', 'ENABLE_HARDENED_RUNTIME=YES',
         'ARCHS=x86_64 arm64', 'ONLY_ACTIVE_ARCH=NO',
         'LUMENCODE_MACOS_ARCHITECTURES=x86_64 arm64',
         'MACOSX_DEPLOYMENT_TARGET=14.0', 'build'], env=environment)
    app = output / 'LumenCode.app'
    shutil.copytree(scratch / 'Build/Products/Release/LumenCode.app', app, symlinks=True)
    info_path = app / 'Contents/Info.plist'
    with info_path.open('rb') as handle:
        info = plistlib.load(handle)
    info.update(CFBundleShortVersionString=args.version, CFBundleVersion=args.build_number)
    with info_path.open('wb') as handle:
        plistlib.dump(info, handle)
    runtime_check(app, output / 'runtime.json')
    write(output / 'build.json', dict(version=args.version, build_number=args.build_number,
          source_commit=subprocess.check_output(['git', '-C', REPO, 'rev-parse', 'HEAD'], text=True).strip(),
          source_worktree_dirty=bool(subprocess.check_output(['git', '-C', REPO, 'status', '--porcelain'], text=True)),
          architectures=['x86_64', 'arm64'], target_macos='14.0', qt_prefix=str(qt),
          signed=False, notarized=False, published=False))
    print(f'Prepared application: {app}')


def signature(app):
    result = run(['codesign', '--display', '--verbose=4', app], capture_output=True, text=True)
    details = result.stderr
    flags = re.search(r'flags=0x([0-9a-fA-F]+)', details)
    team = re.search(r'^TeamIdentifier=(.+)$', details, re.MULTILINE)
    if 'Authority=Developer ID Application:' not in details or not flags or not int(flags[1], 16) & 0x10000 or not team:
        raise SystemExit('A Developer ID Application signature with hardened runtime is required')
    run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', app])
    for path in mach_o_files(app):
        run(['codesign', '--verify', '--strict', path])
        nested = run(['codesign', '--display', '--verbose=4', path], capture_output=True, text=True).stderr
        if f'TeamIdentifier={team[1]}\n' not in nested or 'Authority=Developer ID Application:' not in nested:
            raise SystemExit(f'Bundled code must use the application Developer ID team: {path}')
    return details


def sign(args):
    app = args.app.resolve()
    runtime_check(app, app.parent / 'runtime.json')
    paths = mach_o_files(app)
    # Explicitly sign inside out. --deep is used only for verification.
    for path in sorted(paths, key=lambda p: len(p.parts), reverse=True):
        run(['codesign', '--force', '--sign', args.identity, '--options', 'runtime', '--timestamp', path])
    for path in sorted((p for p in app.rglob('*') if p.is_dir() and not p.is_symlink()
                        and p.suffix in ('.framework', '.app', '.xpc', '.bundle')), key=lambda p: len(p.parts), reverse=True):
        run(['codesign', '--force', '--sign', args.identity, '--options', 'runtime', '--timestamp', path])
    run(['codesign', '--force', '--sign', args.identity, '--options', 'runtime', '--timestamp', app])
    details = signature(app)
    runtime_check(app, app.parent / 'signed-runtime.json')
    write(app.parent / 'signing.json', dict(passed=True, signed=True, notarized=False,
          identity=args.identity, mach_o_files=len(paths), details=details))
    print(f'Signed application: {app}')


def notarize(args):
    app, output = args.app.resolve(), args.output.resolve()
    if output.exists():
        raise SystemExit('Output archive already exists; choose a fresh path')
    signature(app)
    runtime_check(app, app.parent / 'signed-runtime.json')
    credential = ['--keychain-profile', args.profile]
    if args.keychain:
        credential += ['--keychain', str(args.keychain)]
    submission_id = args.submission_id
    if not submission_id:
        with tempfile.TemporaryDirectory(prefix='lumencode-notarization-') as temporary:
            archive = Path(temporary) / 'LumenCode.zip'
            run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, archive])
            print('Submitting the signed application to Apple...', flush=True)
            result = run(['xcrun', 'notarytool', 'submit', archive, *credential,
                          '--output-format', 'json'], capture_output=True, text=True)
            upload = json.loads(result.stdout)
            write(app.parent / 'notarization-submission.json', upload)
            submission_id = upload['id']
    print(f'Waiting for Apple submission {submission_id}...', flush=True)
    result = subprocess.run(['xcrun', 'notarytool', 'wait', submission_id, *credential,
                             '--output-format', 'json'], capture_output=True, text=True)
    if not result.stdout.strip():
        raise SystemExit(result.stderr or 'No notarization response; resume with --submission-id')
    submission = json.loads(result.stdout)
    write(app.parent / 'notarization.json', submission)
    if submission.get('status') != 'Accepted':
        if submission.get('status') == 'Invalid':
            run(['xcrun', 'notarytool', 'log', submission_id, *credential,
                 app.parent / 'notarization-log.json'])
        raise SystemExit('Apple has not accepted the submission; inspect notarization.json')
    run(['xcrun', 'stapler', 'staple', app])
    run(['xcrun', 'stapler', 'validate', app])
    signature(app)
    run(['spctl', '--assess', '--type', 'execute', '--verbose=4', app])
    output.parent.mkdir(parents=True, exist_ok=True)
    run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, output])
    write(app.parent / 'distribution.json', dict(passed=True, notarized=True,
          submission_id=submission_id, archive=str(output),
          sha256=hashlib.sha256(output.read_bytes()).hexdigest(), published=False))
    print(f'Notarized download ready: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='stage', required=True)
    build = sub.add_parser('prepare')
    build.add_argument('--qt', required=True, type=Path)
    build.add_argument('--scratch', required=True, type=Path)
    build.add_argument('--output', required=True, type=Path)
    build.add_argument('--version', required=True)
    build.add_argument('--build-number', required=True)
    signing = sub.add_parser('sign')
    signing.add_argument('--app', required=True, type=Path)
    signing.add_argument('--identity', required=True)
    notary = sub.add_parser('notarize')
    notary.add_argument('--app', required=True, type=Path)
    notary.add_argument('--profile', default='lumencode-notary')
    notary.add_argument('--keychain', type=Path)
    notary.add_argument('--submission-id', help="resume waiting/stapling for this application's existing submission")
    notary.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.stage != 'prepare' and (not args.app.is_dir() or args.app.suffix != '.app'):
        parser.error('--app must name an existing .app directory')
    {'prepare': prepare, 'sign': sign, 'notarize': notarize}[args.stage](args)


if __name__ == '__main__':
    main()

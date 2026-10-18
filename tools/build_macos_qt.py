#!/usr/bin/env python3
"""Build private universal Qt Core/Concurrent libraries for macOS 14 releases."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import tarfile

VERSION = '5.15.19'
SHA256 = '51e91c73abacab81e64efd01bf95794e79c0e605ad80947a024769f0dd620a32'
URL = f'https://download.qt.io/archive/qt/5.15/{VERSION}/submodules/qtbase-everywhere-opensource-src-{VERSION}.tar.xz'
OPTIONS = ['-opensource', '-confirm-license', '-release', '-no-framework',
           '-nomake', 'examples', '-nomake', 'tests', '-no-gui', '-no-widgets',
           '-no-glib', '-no-icu', '-qt-pcre', '-no-zstd', '-no-dbus', '-no-openssl',
           '-no-feature-network', '-no-feature-sql', '-no-feature-xml',
           '-no-feature-testlib', 'QMAKE_MACOSX_DEPLOYMENT_TARGET=14.0',
           'QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64']


def record_build(source, prefix):
    """Preserve source provenance and licence texts after successful install."""
    for name in ('Core', 'Concurrent'):
        library = prefix / 'lib' / f'libQt5{name}.{VERSION}.dylib'
        architectures = subprocess.check_output(['lipo', '-archs', str(library)], text=True).split()
        minimums = re.findall(r'^\s*minos\s+(\S+)', subprocess.check_output(
            ['vtool', '-show-build', str(library)], text=True), re.MULTILINE)
        if set(architectures) != {'x86_64', 'arm64'} or minimums != ['14.0', '14.0']:
            raise SystemExit(f'Qt kit failed universal/macOS 14 check: {library}')
    recipe = dict(version=VERSION, source_url=URL, source_sha256=SHA256,
                  architectures=['x86_64', 'arm64'], target_macos='14.0', configure_options=OPTIONS)
    notices = prefix / 'licenses'
    for path in source.rglob('*'):
        if path.is_file() and path.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT')):
            destination = notices / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
    for path in source.glob('LICENSE*'):
        if path.is_file():
            shutil.copy2(path, prefix / path.name)
    recipe['compiler'] = subprocess.check_output(['xcrun', 'clang', '--version'], text=True).strip()
    (prefix / 'lumencode-qt-build.json').write_text(json.dumps(recipe, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    args = parser.parse_args()
    if subprocess.check_output(['uname', '-sm'], text=True).strip() != 'Darwin arm64':
        parser.error('This release runtime recipe currently supports Apple Silicon macOS only')
    work, prefix = args.work.resolve(), args.prefix.resolve()
    if prefix.exists():
        parser.error('Use a new prefix; this tool never replaces an installed Qt kit')
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f'qtbase-{VERSION}.tar.xz'
    if not archive.exists():
        subprocess.run(['curl', '-fL', '--retry', '2', URL, '-o', str(archive)], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('Qt source archive checksum mismatch')
    source = work / f'qtbase-everywhere-src-{VERSION}'
    if source.exists():
        parser.error('Use a clean work directory so the verified source archive is authoritative')
    with tarfile.open(archive) as handle:
        handle.extractall(work, filter='data')
    build = work / 'build'
    build.mkdir()
    subprocess.run([str(source / 'configure'), '-prefix', str(prefix), *OPTIONS], cwd=build, check=True)
    subprocess.run(['make', '-j6'], cwd=build, check=True)
    subprocess.run(['make', 'install'], cwd=build, check=True)
    record_build(source, prefix)
    print(f'Private Qt kit ready: {prefix}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Build/stage the offline browser prototype using an isolated Qt/Emscripten kit."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ('index.html', 'app.js', 'worker.js', 'worker-client.js', 'storage.js', 'style.css', 'manifest.webmanifest',
          'icon.svg', 'engine.js', 'engine.wasm', 'engine-version.js')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / '.build/web')
    parser.add_argument('--qt', type=Path, help='Qt 6.8.3 wasm_singlethread prefix')
    parser.add_argument('--qt-host', type=Path, help='Matching Qt host prefix')
    parser.add_argument('--emsdk', type=Path, help='SDK with Emscripten 3.1.56 activated')
    parser.add_argument('--parallel', type=int, default=6)
    parser.add_argument('--stage-only', action='store_true', help='Refresh assets around an already built engine')
    args = parser.parse_args()
    build = args.build.resolve()
    if not args.stage_only:
        if not all((args.qt, args.qt_host, args.emsdk)):
            parser.error('--qt, --qt-host and --emsdk are required unless --stage-only')
        env = os.environ.copy()
        sdk = args.emsdk.resolve()
        env['EMSDK'] = str(sdk)
        env['PATH'] = str(sdk / 'upstream/emscripten') + os.pathsep + env['PATH']
        compiler = subprocess.check_output([str(sdk / 'upstream/emscripten/emcc'), '--version'], env=env, text=True)
        if ' 3.1.56 ' not in compiler:
            raise SystemExit('This prototype is validated with Emscripten 3.1.56; use the pinned SDK')
        if '6.8.3' not in (args.qt / 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake').read_text():
            raise SystemExit('This prototype requires the validated Qt 6.8.3 kit')
        print(compiler, end='')
        subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
                        '-DCMAKE_BUILD_TYPE=Release', '-DLUMENCODE_BUILD_WEB=ON',
                        '-DCMAKE_TOOLCHAIN_FILE=' + str(args.qt.resolve() / 'lib/cmake/Qt6/qt.toolchain.cmake'),
                        '-DQT_HOST_PATH=' + str(args.qt_host.resolve())], env=env, check=True)
        subprocess.run(['cmake', '--build', str(build), '--parallel', str(args.parallel)], env=env, check=True)
    web = build / 'web'
    engine_version = hashlib.sha256((web / 'engine.wasm').read_bytes() +
                                    (web / 'engine.js').read_bytes()).hexdigest()
    (web / 'engine-version.js').write_text('globalThis.LUMENCODE_ENGINE_VERSION = ' + json.dumps(engine_version) + ';\n')
    for name in ASSETS:
        if name.startswith('engine.') or name == 'engine-version.js':
            if not (web / name).is_file():
                raise SystemExit('Compile the engine before staging assets')
        else:
            shutil.copy2(ROOT / 'apps/web' / name, web / name)
    records = {name: {'bytes': (web / name).stat().st_size,
                      'sha256': hashlib.sha256((web / name).read_bytes()).hexdigest(),
                      'gzip_bytes': len(gzip.compress((web / name).read_bytes(), mtime=0))}
               for name in ASSETS}
    version = hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()[:20]
    (web / 'sw.js').write_text((ROOT / 'apps/web/sw.js').read_text().replace('__ASSET_VERSION__', version))
    source_paths = sorted((ROOT / 'engine/src').glob('*')) + sorted((ROOT / 'apps/web').glob('*'))
    source_paths += [ROOT / 'third_party/provenance.json', ROOT / 'CMakeLists.txt', ROOT / 'engine/CMakeLists.txt']
    source_hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in source_paths if path.is_file()}
    report = {'asset_version': version, 'assets': records, 'source_sha256': source_hashes,
              'qt': '6.8.3', 'emscripten': '3.1.56',
              'source_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True))}
    (web / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'web_root': str(web), 'asset_version': version,
                      'wasm': records['engine.wasm']}, indent=2))


if __name__ == '__main__':
    main()

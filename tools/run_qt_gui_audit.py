#!/usr/bin/env python3
"""Run Qt's rendered smoke with isolated settings seeded from a KDE palette.

Use in a graphical desktop session; the default palette is the user's KDE
configuration. This preserves dark-mode visual validation without modifying the
user's settings or imposing a theme on the application.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=repo / 'build')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--palette', type=Path, default=Path.home() / '.config/kdeglobals')
parser.add_argument('roots', nargs='*', type=Path)
args = parser.parse_args()
for executable in ('lumencode-gui-smoke', 'lumencode-cli'):
    if not (args.build.resolve() / 'bin' / executable).is_file():
        parser.error(f'Missing {executable}; build both lumencode-gui-smoke and lumencode-cli first')
if not args.palette.is_file():
    parser.error('No KDE palette found; supply --palette /path/to/kdeglobals')
output = args.output.resolve()
config = output / 'config'
config.mkdir(parents=True, exist_ok=True)
shutil.copy2(args.palette, config / 'kdeglobals')
env = dict(os.environ, XDG_CONFIG_HOME=str(config), XDG_CACHE_HOME=str(output / 'cache'),
           QT_QPA_PLATFORMTHEME='kde', QT_QUICK_BACKEND='software',
           LUMENCODE_SMOKE_SHOTS=str(output / 'screenshots'))
roots = args.roots or [repo / 'tests/fixtures/baseline/web_app']
with (output / 'smoke.log').open('w') as log:
    result = subprocess.run([str(args.build.resolve() / 'bin/lumencode-gui-smoke'),
                             *map(str, roots)], env=env, stdout=log, stderr=subprocess.STDOUT)
print(f'Qt audit exited {result.returncode}; screenshots and log: {output}')
raise SystemExit(result.returncode)

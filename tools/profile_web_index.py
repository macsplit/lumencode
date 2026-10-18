#!/usr/bin/env python3
"""Profile a real project in the browser worker and retain per-phase diagnostics."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
import time
from playwright.sync_api import sync_playwright
from check_web_prototype import HARNESS, Quiet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--chromium', default='/snap/bin/chromium')
    parser.add_argument('--browser', choices=('chromium', 'webkit'), default='chromium')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(Quiet, directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {'project': str(args.project.resolve()), 'browser': args.browser, 'completed': False}
    try:
        with sync_playwright() as p:
            browser = (p.chromium.launch(executable_path=args.chromium, headless=True, args=['--no-sandbox'])
                       if args.browser == 'chromium' else p.webkit.launch(headless=True))
            page = browser.new_page(viewport={'width': 1440, 'height': 900})
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('import').disabled", timeout=90000)
            page.evaluate("document.getElementById('folder-input').onchange = null")
            page.locator('#folder-input').set_input_files(str(args.project.resolve()))
            page.evaluate(HARNESS)
            report['initialization'] = page.evaluate('workerReady')
            report['mount'] = page.evaluate("""async () => {
              const skipped = new Set(['node_modules','dist','build','coverage','vendor',
                '__pycache__','venv','target','DerivedData','bower_components','bin','obj']);
              const files = [...document.getElementById('folder-input').files].map(file => ({
                path: file.webkitRelativePath.split('/').slice(1).join('/'), file
              })).filter(({path}) => !path.split('/').slice(0,-1).some(p => p.startsWith('.') || skipped.has(p)));
              const result = await testRequest('mount', {files});
              return {selectedFiles: files.length, candidates: result.result.paths.length, elapsedMs: result.elapsedMs};
            }""")
            page.evaluate("""() => {
              window.events = []; window.indexResult = null; window.indexError = null;
              window.heartbeat = {ticks: 0, maxGap: 0, last: performance.now()};
              window.heartbeatTimer = setInterval(() => {
                const now = performance.now(); heartbeat.maxGap = Math.max(heartbeat.maxGap, now-heartbeat.last);
                heartbeat.last = now; heartbeat.ticks++;
              }, 10);
              testWorker.addEventListener('message', ({data}) => {
                if (data.type === 'progress') events.push({...data, at: performance.now()});
              });
              testRequest('index').then(result => {indexResult = result; clearInterval(heartbeatTimer);})
                .catch(error => {indexError = String(error); clearInterval(heartbeatTimer);});
            }""")
            begin = time.monotonic()
            while time.monotonic() - begin < args.timeout:
                current = page.evaluate('({done: !!indexResult, error: indexError, progress: events.at(-1)})')
                print(json.dumps({'seconds': round(time.monotonic() - begin, 1), **current}), flush=True)
                if current['done'] or current['error']:
                    break
                page.wait_for_timeout(1000)
            report.update(page.evaluate('({result:indexResult,error:indexError,events,heartbeat})'))
            report['completed'] = bool(report['result']) and not report['error']
            report['wall_seconds'] = time.monotonic() - begin
            browser.close()
    finally:
        (args.output / 'profile.json').write_text(json.dumps(report, indent=2) + '\n')
        server.shutdown()
        server.server_close()
    print(json.dumps({key: value for key, value in report.items() if key != 'events'}, indent=2))
    if not report['completed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

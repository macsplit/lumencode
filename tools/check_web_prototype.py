#!/usr/bin/env python3
"""Exercise the real browser worker, native parity and offline lifecycle.

Requires Playwright and Chromium. Test directory handles are obtained through a
browser-private test fixture; this does not test the native permission dialog.
"""
import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def check_index_watchdog(browser, url):
    """Virtual time: continued progress beyond two minutes vs a stalled worker."""
    page = browser.new_page()
    page.add_init_script("""(() => {
      const NativeWorker = window.Worker;
      window.Worker = class extends NativeWorker {
        constructor(...args) { super(...args); window.lastPrototypeWorker = this; }
      };
    })()""")
    page.goto(url)
    page.wait_for_function("!document.getElementById('demo').disabled", timeout=90000)
    page.locator('#demo').click()
    page.wait_for_function("!document.getElementById('index').disabled")
    page.clock.install()
    page.evaluate("""() => {
      const instance = lastPrototypeWorker;
      const post = instance.postMessage.bind(instance);
      instance.postMessage = data => {
        if (data.operation !== 'index') post(data);
      };
      window.emitProgress = phase => instance.dispatchEvent(new MessageEvent('message', {
        data: {type: 'progress', phase, path: '/project/slow.js', done: 1, total: 10}
      }));
    }""")
    page.locator('#index').click()
    page.clock.fast_forward(90000)
    page.evaluate("emitProgress('analysing')")
    page.clock.fast_forward(90000)
    assert page.locator('#index-details').is_visible()
    assert page.locator('#cancel').is_enabled()
    assert 'slow.js' in page.locator('#status').inner_text()
    # No further progress: the watchdog must eventually terminate this request.
    page.clock.fast_forward(120001)
    page.wait_for_function("!document.getElementById('demo').disabled", timeout=30000)
    assert 'No engine progress for 120 seconds' in page.locator('#status').inner_text()
    assert not page.locator('#index-details').is_visible()
    page.close()
    return {'continued_progress_over_120s': True, 'stalled_worker_restarted': True,
            'clock': 'virtual; no performance claim'}


HARNESS = """() => {
  window.testWorker = new Worker('worker.js');
  let id = 0;
  window.progressEvents = [];
  const pending = new Map();
  window.workerReady = new Promise((resolve, reject) => {
    testWorker.onmessage = ({data}) => {
      if (data.type === 'progress') progressEvents.push(data);
      if (data.type === 'ready') resolve(data);
      if (data.type === 'fatal') reject(new Error(data.error));
      if (data.id && pending.has(data.id)) {
        const item = pending.get(data.id); pending.delete(data.id);
        data.error ? item.reject(new Error(data.error)) : item.resolve(data);
      }
    };
    testWorker.onerror = e => reject(new Error(e.message));
  });
  window.testRequest = (operation, extra = {}) => new Promise((resolve, reject) => {
    pending.set(++id, {resolve, reject});
    testWorker.postMessage({id, operation, ...extra});
  });
}"""


def normalized(value, native_root):
    # Normalize only the physical project-root prefix; preserve every field and
    # array order, including provenance, recovery and unresolved dependencies.
    if isinstance(value, str):
        return value.replace(str(native_root), '/project')
    if isinstance(value, list):
        return [normalized(item, native_root) for item in value]
    if isinstance(value, dict):
        return {key: normalized(item, native_root) for key, item in value.items()}
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--native-helper', type=Path, required=True)
    parser.add_argument('--chromium', default='/snap/bin/chromium')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(Quiet, directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/'
    report = {'passed': False, 'errors': [], 'differences': [], 'timings_ms': {}}
    try:
        with sync_playwright() as p, tempfile.TemporaryDirectory(prefix='lumencode-pwa-profile-') as profile:
            context = p.chromium.launch_persistent_context(profile, executable_path=args.chromium,
                headless=True, args=['--no-sandbox'], viewport={'width': 1440, 'height': 900})
            page = context.pages[0]
            page.on('pageerror', lambda error: report['errors'].append(str(error)))
            network = []
            page.on('request', lambda request: network.append((request.method, request.url)))
            page.goto(url)
            page.wait_for_function("!document.getElementById('demo').disabled", timeout=90000)
            page.locator('#demo').click()
            page.wait_for_function("document.getElementById('result').textContent.length > 2", timeout=30000)
            page.locator('#index').click()
            page.wait_for_function("!document.getElementById('search').disabled", timeout=30000)
            page.locator('#search').fill('greeting')
            page.locator('#search').press('Enter')
            page.wait_for_function("document.querySelectorAll('#search-results button').length > 0")
            page.screenshot(path=str(args.output / 'desktop.png'), full_page=True)
            assert not page.evaluate('document.documentElement.scrollWidth > innerWidth')
            report['ui_demo_index_search'] = True
            report['index_watchdog'] = check_index_watchdog(context, url)

            # Exercise the production folder-handle enumeration and refresh path
            # using a real OPFS directory handle supplied by a test-only picker.
            # This deliberately does not claim native-dialog/permission coverage.
            page.evaluate("""async () => {
              const root = await navigator.storage.getDirectory();
              const dir = await root.getDirectoryHandle('handle-test', {create:true});
              const file = await dir.getFileHandle('handle.js', {create:true});
              const stream = await file.createWritable();
              await stream.write('export function firstVersion() { return 1; }'); await stream.close();
              window.testDirectory = dir;
              window.showDirectoryPicker = async options => {
                if (options.mode !== 'read') throw new Error('Expected read-only access');
                return dir;
              };
            }""")
            page.locator('#open').click()
            page.wait_for_function("document.getElementById('result').textContent.includes('firstVersion') && !document.getElementById('refresh').disabled")
            page.evaluate("""async () => {
              const file = await testDirectory.getFileHandle('handle.js');
              const stream = await file.createWritable();
              await stream.write('export function nextVersionX() { return 2; }'); await stream.close();
            }""")
            page.locator('#refresh').click()
            page.wait_for_function("document.getElementById('result').textContent.includes('nextVersionX') && !document.getElementById('refresh').disabled")
            assert 'firstVersion' not in page.locator('#result').inner_text()
            report['directory_handle_enumeration_refresh'] = 'Real OPFS handle through test-supplied picker; OS permission dialog not tested'

            # Use the actual directory-import control and its File objects.
            fixture_root = ROOT / 'tests/fixtures/baseline'
            page.locator('#folder-input').set_input_files(str(fixture_root))
            page.wait_for_function("document.getElementById('project-name').textContent.includes('baseline') && !document.getElementById('index').disabled", timeout=30000)
            report['folder_import_files'] = page.locator('#files button').count()
            page.evaluate(HARNESS)
            ready = page.evaluate('workerReady')
            report['timings_ms']['worker_initialization'] = ready['initializationMs']
            # Reuse actual browser File objects, keeping binary contents and paths.
            # The import input is cleared by the UI; refill with no change handler.
            page.evaluate("document.getElementById('folder-input').onchange = null")
            page.locator('#folder-input').set_input_files(str(fixture_root))
            page.evaluate("""async () => {
              const files = [...document.getElementById('folder-input').files].map(file => ({
                path: file.webkitRelativePath.split('/').slice(1).join('/'), file
              }));
              await workerReady; return testRequest('mount', {files});
            }""")
            manifest = json.loads((fixture_root / 'manifest.json').read_text())
            files = sorted({str((ROOT / case['root'] / case['file']).relative_to(fixture_root)) for case in manifest['cases']})
            captures = {}
            allocated_memory = []
            for relative in files:
                native = json.loads(subprocess.check_output([str(args.native_helper), '--dump-file', str(fixture_root / relative)],
                    env={**os.environ, 'QT_HASH_SEED': '0'}, timeout=30))
                reply = page.evaluate("path => testRequest('analyse', {path})", relative)
                captures[relative] = reply['result']
                report['timings_ms'][relative] = reply['elapsedMs']
                allocated_memory.append(reply.get('linearMemoryBytes'))
                if normalized(native, fixture_root) != reply['result']:
                    report['differences'].append(relative)
                    (args.output / (relative.replace('/', '_') + '.json')).write_text(json.dumps(
                        {'native': normalized(native, fixture_root), 'browser': reply['result']}, indent=2))
            report['file_comparisons'] = len(files)
            (args.output / 'browser-fixture-results.json').write_text(json.dumps(captures, indent=2))
            rejected = page.evaluate("""async () => {
              try { await testRequest('analyse', {path: '../outside.js'}); return false; }
              catch (error) { return error.message.includes('Invalid project-relative path'); }
            }""")
            assert rejected
            report['path_traversal_rejected'] = True

            # Native and browser index the same complete fixture tree.
            indexed_run = page.evaluate("""async () => {
              const start = performance.now(); let last = start, ticks = 0, maxGap = 0;
              const timer = setInterval(() => {
                const now = performance.now(); maxGap = Math.max(maxGap, now - last); last = now; ticks++;
              }, 10);
              const reply = await testRequest('index'); clearInterval(timer);
              maxGap = Math.max(maxGap, performance.now() - last);
              return {reply, ticks, maxGap};
            }""")
            index = indexed_run['reply']
            report['browser_index'] = index['result']
            report['index_progress_events'] = page.evaluate('progressEvents.length')
            report['index_progress_phases'] = page.evaluate('[...new Set(progressEvents.map(event => event.phase))]')
            assert report['index_progress_events'] > 0
            assert {'analysing', 'linking', 'complete'} <= set(report['index_progress_phases'])
            report['timings_ms']['index'] = index['elapsedMs']
            report['ui_heartbeat_during_index'] = indexed_run['ticks']
            report['ui_max_timer_gap_ms_during_index'] = indexed_run['maxGap']
            assert indexed_run['ticks'] > 0
            allocated_memory.append(index.get('linearMemoryBytes'))
            report['maximum_observed_linear_memory_bytes'] = max((item for item in allocated_memory if item), default=None)
            indexed_files = ['js_commonjs/main.js', 'ts_module/main.ts', 'swift_try/Calls.swift']
            report['indexed_comparisons'] = []
            for relative in indexed_files:
                if not (fixture_root / relative).exists():
                    continue
                native = json.loads(subprocess.check_output([str(args.native_helper), '--root', str(fixture_root),
                    '--path', str(fixture_root / relative), '--cache', str(args.output / 'native-index-cache.json')],
                    env={**os.environ, 'QT_HASH_SEED': '0'}, timeout=120))
                browser = page.evaluate("path => testRequest('analyse', {path, indexed: true})", relative)['result']
                report['indexed_comparisons'].append(relative)
                if normalized(native, fixture_root) != browser:
                    report['differences'].append('indexed:' + relative)
                    (args.output / ('indexed-' + relative.replace('/', '_') + '.json')).write_text(json.dumps(
                        {'native': normalized(native, fixture_root), 'browser': browser}, indent=2))

            # Cancel immediately after dispatch: stale worker replies must not
            # restore the old project, and a new operation must still succeed.
            page.evaluate("document.getElementById('index').click(); document.getElementById('cancel').click()")
            page.wait_for_function("!document.getElementById('demo').disabled && document.querySelectorAll('#files button').length === 0")
            page.locator('#demo').click()
            page.wait_for_function("document.getElementById('project-name').textContent === 'Example project' && !document.getElementById('index').disabled")
            report['cancel_and_worker_recovery'] = True

            page.set_viewport_size({'width': 390, 'height': 844})
            assert not page.evaluate('document.documentElement.scrollWidth > innerWidth')
            page.screenshot(path=str(args.output / 'mobile.png'), full_page=True)
            page.wait_for_function("document.getElementById('offline').textContent.startsWith('Ready for offline use')", timeout=60000)
            context.close()

            # Close the browser, stop the HTTP origin, reopen the same profile
            # offline, and analyse a new source file that was never cached.
            server.shutdown()
            context = p.chromium.launch_persistent_context(profile, executable_path=args.chromium,
                headless=True, args=['--no-sandbox'])
            context.set_offline(True)
            page = context.pages[0]
            page.on('pageerror', lambda error: report['errors'].append(str(error)))
            page.goto(url)
            page.wait_for_function("!document.getElementById('demo').disabled", timeout=90000)
            page.evaluate(HARNESS)
            page.evaluate('workerReady')
            fresh_name = 'offline_' + str(time.time_ns())
            result = page.evaluate("""async name => {
              const file = new File([`export function ${name}() { return 42; }`], 'fresh.js');
              await testRequest('mount', {files: [{path: 'fresh.js', file}]});
              return (await testRequest('analyse', {path: 'fresh.js'})).result;
            }""", fresh_name)
            assert any(symbol['name'] == fresh_name for symbol in result['symbols'])
            report['offline_browser_restart_new_source'] = True
            report['network_get_only_same_origin'] = all(method == 'GET' and address.startswith(url) for method, address in network)
            assert report['network_get_only_same_origin']
            context.close()
            assert not report['errors'], report['errors']
            assert not report['differences'], report['differences']
            report['passed'] = True
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({key: value for key, value in report.items() if key != 'timings_ms'}, indent=2))
        server.server_close()


if __name__ == '__main__':
    main()

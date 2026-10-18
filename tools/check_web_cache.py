#!/usr/bin/env python3
"""Verify persistent facts reuse, invalidation and storage-denied fallback."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import HARNESS


class Assets(http.server.SimpleHTTPRequestHandler):
    engine_suffix = ''
    deny_storage = False

    def log_message(self, *args):
        pass

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def do_GET(self):
        if self.path == '/engine-version.js' and self.engine_suffix:
            text = Path(self.translate_path(self.path)).read_text()
            self.javascript(text + '\nLUMENCODE_ENGINE_VERSION += ' + json.dumps(self.engine_suffix) + ';')
        elif self.path == '/storage.js' and self.deny_storage:
            text = Path(self.translate_path(self.path)).read_text()
            self.javascript("Object.defineProperty(globalThis, 'indexedDB', {get() { throw new Error('Storage denied by test'); }});\n" + text)
        else:
            super().do_GET()

    def javascript(self, text):
        data = text.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/javascript')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def worker(page):
    page.evaluate('if (window.testWorker) testWorker.terminate()')
    page.evaluate(HARNESS)
    page.evaluate('workerReady')


def mount(page, sources, project='cache-test', verify=True):
    return page.evaluate("""async ({sources, project, verify}) => {
      const files = Object.entries(sources).map(([path, source]) => ({
        path, file: new File([source], path, {lastModified: 1700000000000})
      }));
      return testRequest('mount', {files, projectKey: project, verifyContents: verify});
    }""", {'sources': sources, 'project': project, 'verify': verify})


def index(page):
    return page.evaluate("testRequest('index')")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--chromium', default='/snap/bin/chromium')
    parser.add_argument('--browser', choices=('chromium', 'webkit'), default='chromium')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(Assets, directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {'passed': False, 'browser': args.browser}
    try:
        with sync_playwright() as p:
            browser = (p.chromium.launch(executable_path=args.chromium, headless=True, args=['--no-sandbox'])
                       if args.browser == 'chromium' else p.webkit.launch(headless=True))
            # No SW in this context: changing the served version/storage script
            # deliberately simulates an upgrade and denied storage separately.
            context = browser.new_context(service_workers='block')
            page = context.new_page()
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            worker(page)
            sources = {'main.js': 'import { greet } from "./helper.js";\nexport function run() { return greet(); }\n',
                       'helper.js': 'export function greet() { return 1; }\n'}
            first = mount(page, sources)
            assert not first['result']['cacheRestored']
            cold = index(page)
            assert cold['cacheStatus'] == 'saved' and cold['result']['analysed'] == 2
            expected = page.evaluate("testRequest('analyse', {path:'main.js', indexed:true})")['result']
            worker(page)
            assert mount(page, sources)['result']['cacheRestored']
            warm = index(page)
            assert warm['result']['reusedFromCache'] == 2 and warm['result']['analysed'] == 0
            assert page.evaluate("testRequest('analyse', {path:'main.js', indexed:true})")['result'] == expected
            report['worker_restart_reuses_complete_facts'] = True

            # Same size/timestamp, different neighbour contents: imports must
            # invalidate, even if the currently selected main.js did not change.
            changed = {**sources, 'helper.js': sources['helper.js'].replace('greet', 'other')}
            worker(page)
            assert not mount(page, changed)['result']['cacheRestored']
            assert index(page)['result']['analysed'] == 2
            result = page.evaluate("testRequest('analyse', {path:'helper.js', indexed:true})")['result']
            assert any(symbol['name'] == 'other' for symbol in result['symbols'])
            report['same_metadata_changed_contents_invalidates_import'] = True

            worker(page)
            added = {**changed, 'extra.js': 'export function extra() {}\n'}
            assert not mount(page, added)['result']['cacheRestored']
            assert index(page)['result']['files'] == 3
            worker(page)
            removed = {'main.js': sources['main.js']}
            assert not mount(page, removed)['result']['cacheRestored']
            assert index(page)['result']['files'] == 1
            report['added_deleted_files_invalidate'] = True

            # Save a stable snapshot to test a real page reload and key versioning.
            worker(page)
            mount(page, sources, project='stable')
            index(page)
            page.reload()
            worker(page)
            assert mount(page, sources, project='stable')['result']['cacheRestored']
            assert index(page)['result']['reusedFromCache'] == 2
            report['page_reload_reuses_facts'] = True
            Assets.engine_suffix = '-upgrade-test'
            worker(page)
            assert not mount(page, sources, project='stable')['result']['cacheRestored']
            assert index(page)['result']['analysed'] == 2
            report['simulated_engine_upgrade_invalidates'] = True
            count = page.evaluate("""() => new Promise((resolve, reject) => {
              const open = indexedDB.open('lumencode-local-projects', 1);
              open.onerror = () => reject(open.error);
              open.onsuccess = () => {
                const db = open.result, tx = db.transaction('indexes');
                const query = tx.objectStore('indexes').count();
                query.onsuccess = () => resolve(query.result);
                tx.oncomplete = () => db.close();
              };
            })""")
            assert count <= 3
            recent_count = None
            if args.browser == 'chromium':
                projects = page.evaluate("""async () => {
              const handle = await navigator.storage.getDirectory();
              for (let i=0; i<9; i++) await LumenCodeStorage.saveProject({
                id: `bounded-${i}`, name: `Project ${i}`, opened: i, handle
              });
              return LumenCodeStorage.projects();
                }""")
                assert len(projects) == 8 and not any(project['id'] == 'bounded-0' for project in projects)
                recent_count = len(projects)
            else:
                report['recent_folder_scope'] = 'Handle retention is tested in Chromium; WebKit coverage targets imported files and derived indexes'
            report['retention_bounds'] = {'index_snapshots': count, 'recent_folders': recent_count}

            Assets.deny_storage = True
            worker(page)
            assert mount(page, sources, project='storage-denied')['cacheStatus'] == 'unavailable'
            fallback = index(page)
            assert fallback['result']['files'] == 2 and fallback['cacheStatus'] == 'unavailable'
            report['storage_denied_still_analyses'] = True
            Assets.deny_storage = False
            worker(page)
            page.locator('#project-menu > summary').click()
            page.locator('#clear-saved').click()
            page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved indexes')")
            assert page.evaluate('LumenCodeStorage.projects()') == []
            assert not mount(page, sources, project='stable')['result']['cacheRestored']
            report['clear_saved_projects_removes_indexes'] = True
            report['cold_ms'] = cold['elapsedMs']
            report['warm_ms'] = warm['elapsedMs']
            report['passed'] = True
            browser.close()
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()

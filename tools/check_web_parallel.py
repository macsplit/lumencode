#!/usr/bin/env python3
"""Compare complete indexed results and exercise pool cancellation/failure."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import HARNESS, Quiet, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browser', choices=('chromium', 'webkit'), default='chromium')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(Quiet, directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {'browser': args.browser, 'passed': False}
    try:
        with sync_playwright() as p:
            browser = (p.chromium.launch(executable_path='/snap/bin/chromium', args=['--no-sandbox'])
                       if args.browser == 'chromium' else p.webkit.launch())
            context = browser.new_context(service_workers='block')
            page = context.new_page()
            page.add_init_script("""(() => {
              const Native = Worker;
              window.poolProbe = {live: new Set(), mode: ''};
              window.Worker = class extends Native {
                constructor(...args) { super(...args); poolProbe.live.add(this); }
                terminate() { poolProbe.live.delete(this); super.terminate(); }
                postMessage(data) {
                  if (data.operation === 'facts' && poolProbe.mode === 'hold') return;
                  if (data.operation === 'facts' && poolProbe.mode === 'fail') {
                    setTimeout(() => this.dispatchEvent(new MessageEvent('message', {
                      data: {id:data.id, error:'Injected parser failure'}
                    })), 0); return;
                  }
                  super.postMessage(data);
                }
              };
            })()""")
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('import').disabled", timeout=90000)
            page.evaluate("document.getElementById('folder-input').onchange = null")
            page.locator('#folder-input').set_input_files(str(ROOT / 'tests/fixtures/baseline'))
            page.evaluate("""() => { window.poolFiles = [...document.getElementById('folder-input').files]
              .map(file => ({path:file.webkitRelativePath.split('/').slice(1).join('/'),file})); }""")

            def fresh(project=None):
                page.evaluate('if (window.testWorker) testWorker.terminate()')
                page.evaluate(HARNESS)
                page.evaluate('workerReady')
                return page.evaluate("projectKey => testRequest('mount', {files:poolFiles, projectKey})", project)

            outputs = {}
            for count in (1, 2, 4):
                mounted = fresh('parallel-cache' if count == 4 else None)
                reply = page.evaluate("workers => testRequest('index', {workers})", count)
                assert reply['parserWorkers'] == count
                assert page.evaluate('testWorker.children.size') == 0
                captures = page.evaluate("""async paths => {
                  const results = {};
                  for (const absolute of paths) {
                    const path = absolute.slice('/project/'.length);
                    results[path] = (await testRequest('analyse', {path,indexed:true})).result;
                  }
                  return results;
                }""", mounted['result']['paths'])
                outputs[count] = captures
                (args.output / f'indexed-{count}.json').write_text(json.dumps(captures, indent=2)+'\n')
                assert captures == outputs[1], f'Complete indexed JSON differs with {count} parsers'
                warm = page.evaluate("testRequest('index', {workers:4})")
                assert warm['parserWorkers'] == 1
                assert warm['result']['analysed'] == 0
                assert warm['result']['reusedFromCache'] == len(captures)
                report[str(count)] = {'cold': reply, 'warm': warm, 'indexed_files_compared': len(captures)}

            restored = fresh('parallel-cache')
            assert restored['result']['cacheRestored']
            restored_index = page.evaluate("testRequest('index', {workers:4})")
            assert restored_index['result']['analysed'] == 0
            assert restored_index['parserWorkers'] == 1
            report['parallel_facts_reused_after_worker_restart'] = True

            fresh()
            page.evaluate("poolProbe.mode = 'fail'")
            failure = page.evaluate("""async () => {
              try { await testRequest('index', {workers:4}); return ''; }
              catch (error) { return String(error); }
            }""")
            assert 'Injected parser failure' in failure
            assert page.evaluate('testWorker.children.size') == 0
            assert page.evaluate("testRequest('index-plan')")['result']['paths']
            report['parser_failure_stops_pool_without_publishing_index'] = True

            fresh()
            page.evaluate("""() => {
              poolProbe.mode = 'hold'; window.liveBefore = poolProbe.live.size;
              testRequest('index', {workers:4}).catch(() => {});
            }""")
            page.wait_for_function('testWorker.children.size === 4')
            page.evaluate('testWorker.terminate()')
            assert page.evaluate('poolProbe.live.size === liveBefore - 1')
            report['cancel_terminates_all_workers'] = True
            page.evaluate("poolProbe.mode = ''")
            fresh()
            assert page.evaluate("testRequest('index', {workers:2})")['result']['failed'] == 0
            report['recovery_after_cancel'] = True
            report['passed'] = True
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
        (args.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({key: value if not key.isdigit() else {
        'cold_ms': value['cold']['elapsedMs'], 'indexed_files_compared': value['indexed_files_compared']
    } for key, value in report.items()}, indent=2))


if __name__ == '__main__':
    main()

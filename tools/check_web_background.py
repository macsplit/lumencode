#!/usr/bin/env python3
"""Exercise browsing, preserved selections and project replacement during indexing.

The test gates real parser-worker requests at the browser transport boundary;
production code contains no delay/test hook. All released requests use real WASM.
"""
import argparse
import functools
import http.server
import json
from pathlib import Path
import tempfile
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import HARNESS, Quiet


def check_single_parser(browser, url, project):
    page = browser.new_page(service_workers='block')
    page.goto(url)
    page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
    page.evaluate("document.getElementById('folder-input').onchange = null")
    page.locator('#folder-input').set_input_files(str(project.resolve()))
    page.evaluate(HARNESS); page.evaluate('workerReady')
    mounted = page.evaluate("""async () => {
      const skipped = new Set(['node_modules','dist','build','coverage','vendor','__pycache__',
        'venv','target','DerivedData','bower_components','bin','obj']);
      const files = [...document.getElementById('folder-input').files].map(file => ({
        path:file.webkitRelativePath.split('/').slice(1).join('/'),file
      })).filter(({path})=>!path.split('/').slice(0,-1).some(part=>part.startsWith('.') || skipped.has(part)));
      return testRequest('mount',{files});
    }""")
    page.evaluate("""() => {
      window.singleIndex = null; window.singleError = null;
      testRequest('index',{workers:1}).then(result=>singleIndex=result).catch(error=>singleError=String(error));
    }""")
    page.wait_for_function("progressEvents.some(event=>event.phase==='analysing')",timeout=90000)
    path = mounted['result']['paths'][0].removeprefix('/project/')
    inspection = page.evaluate("""async path => {
      const start=performance.now();
      const reply=await testRequest('analyse',{path});
      return {elapsedMs:performance.now()-start,completedBeforeIndex:!singleIndex,
        viewerCreated:!!testWorker.viewer,reply};
    }""",path)
    assert inspection['completedBeforeIndex'] and inspection['viewerCreated'],inspection
    page.wait_for_function('singleIndex || singleError',timeout=180000)
    assert not page.evaluate('singleError')
    index = page.evaluate('singleIndex')
    assert index['parserWorkers'] == 1 and index['result']['failed'] == 0
    page.wait_for_function('testWorker.viewer === null')
    assert page.evaluate("path=>testRequest('analyse',{path,indexed:true})",path)['result'].get('projectIndex')
    page.close()
    return {'files':index['result']['files'],'cold_index_ms':index['elapsedMs'],
        'inspection_ms':inspection['elapsedMs'],'inspection_completed_during_real_single_parser_index':True,
        'inspection_worker_released':True,'inspection_capacity_bytes':index['inspectionLinearMemoryCapacityBytes']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browser', choices=('chromium','webkit'), default='chromium')
    parser.add_argument('--real-project',type=Path,help='Also prove inspection while one coordinator is busy indexing this real project')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0), functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report = {'browser':args.browser,'passed':False}
    try:
        with sync_playwright() as p, tempfile.TemporaryDirectory(prefix='lumencode-background-',dir=Path.home()) as temporary:
            root = Path(temporary)
            for name in ('project-a','project-b'):
                source = root/name/'src'; source.mkdir(parents=True)
                (source/'helper.js').write_text('export function greet(name) { return name; }\n')
                (source/'main.js').write_text('import { greet } from "./helper.js";\nexport function run() { return greet("world"); }\n')
                (source/'encoding.js').write_bytes(b'export function encoding() { return 1; } // \xff\n')
                (source/'long.js').write_text('\n'.join(f'export function fn{i}() {{ return {i}; }}' for i in range(650)))
                for i in range(130): (source/f'extra{i:03}.js').write_text(f'export function extra{i}() {{ return {i}; }}\n')
                (source.parent/'README.md').write_text('# Read me\n<img src=x onerror=alert(1)>\nUseful project notes.\n')
                (source.parent/'package.json').write_text('{"name":"workspace-check","version":"1.2.3","scripts":{"start":"node src/main.js"}}')
                (source.parent/'style.css').write_text('.message { color: red; }\n')
                (source.parent/'settings.custom').write_text('answer = 42\n')
                (source.parent/'utf16.notes').write_bytes('Notes in UTF-16\n'.encode('utf-16'))
                (source.parent/'image.bin').write_bytes(b'\x89PNG\0\xff\x01')
                with (source.parent/'large.log').open('wb') as file: file.truncate(5*1024*1024+1)
            replacement = root/'replacement'; replacement.mkdir()
            (replacement/'done.js').write_text('export function done() {}\n')
            auxiliary = root/'notes'; auxiliary.mkdir()
            (auxiliary/'README.md').write_text('Only documentation.\n')
            browser = (p.chromium.launch(executable_path='/snap/bin/chromium',args=['--no-sandbox'])
                       if args.browser == 'chromium' else p.webkit.launch())
            page = browser.new_page(viewport={'width':1440,'height':950},service_workers='block')
            errors = []; page.on('pageerror',lambda error: errors.append(str(error)))
            page.add_init_script("""(() => {
              const Native = Worker;
              window.gate = {hold:true,waiting:[],live:new Set()};
              window.Worker = class extends Native {
                constructor(...args) { super(...args); gate.live.add(this); }
                terminate() { gate.live.delete(this); super.terminate(); }
                postMessage(data) {
                  if (gate.hold && data.operation === 'facts') { gate.waiting.push([this,data]); return; }
                  super.postMessage(data);
                }
                sendHeld(data) { super.postMessage(data); }
              };
              window.releaseFacts = () => {
                gate.hold=false;
                for (const [worker,data] of gate.waiting.splice(0)) worker.sendHeld(data);
              };
            })()""")
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)

            def opened(filename):
                page.wait_for_function("path => document.getElementById('selected-name').textContent === path && !busy",arg=filename)

            def choose(filename):
                page.locator('#search').fill(filename)
                page.wait_for_function("document.querySelectorAll('#search-results button').length > 0")
                page.locator('#search-results button').first.click(); opened(filename)
                page.locator('#search').press('Escape')

            def held():
                page.wait_for_function('indexing && !busy && gate.waiting.length > 0 && worker.children.size >= 2',timeout=90000)

            page.evaluate("window.indexReplies=[]; worker.addEventListener('message',({data})=>{if(data.parserWorkers)indexReplies.push(data)})")
            page.locator('#folder-input').set_input_files(str(root/'project-a')); held()
            assert page.locator('#cancel').is_visible()
            assert page.locator('#search').is_enabled()
            assert page.locator('#choose-project').is_enabled()
            choose('src/main.js')
            page.locator('.symbol-button',has_text='run').click()
            assert page.locator('#inspector h3').inner_text() == 'run'
            assert page.evaluate('!!worker.viewer')
            choose('src/encoding.js')
            assert not json.loads(page.locator('#result').text_content()).get('previewOnly')
            assert 'encoding' in page.locator('#symbols').inner_text()
            report['canonical_file_inspection_during_indexing'] = True
            report['candidate_encoding_does_not_bypass_shared_parser'] = True
            for filename, text in [('README.md','Useful project notes.'),('settings.custom','answer = 42'),('utf16.notes','Notes in UTF-16')]:
                choose(filename)
                assert text in page.locator('#source').inner_text()
                assert json.loads(page.locator('#result').text_content())['previewOnly']
                assert page.locator('#source img').count() == 0
            choose('package.json')
            package = json.loads(page.locator('#result').text_content())
            assert package['language'] == 'json' and package['packageSummary']['name'] == 'workspace-check'
            assert not package.get('previewOnly')
            choose('style.css')
            stylesheet = json.loads(page.locator('#result').text_content())
            assert stylesheet['language'] == 'css' and stylesheet['symbols']
            assert not stylesheet.get('previewOnly')
            report['nonindexed_css_and_package_metadata_use_shared_parser'] = True
            choose('image.bin'); assert 'Binary file' in page.locator('#summary').inner_text()
            assert page.locator('#copy-source').is_disabled()
            choose('large.log'); assert 'larger than 5 MiB' in page.locator('#summary').inner_text()
            report['auxiliary_utf8_utf16_binary_and_size_limits'] = True
            choose('src/long.js')
            page.locator('.symbol-button',has_text='fn600').click()
            page.locator('#source-previous').click()
            page.evaluate("Workspace.activatePane('preview'); document.getElementById('source').scrollTop=80")
            before = page.evaluate("({path:selected,title:document.querySelector('#inspector h3').textContent,page:document.getElementById('source-position').textContent,top:document.getElementById('source').scrollTop,pane:document.getElementById('workspace').dataset.pane})")
            page.screenshot(path=str(args.output/'browsing-during-index.png'))
            page.evaluate('releaseFacts()')
            page.wait_for_function("indexed && !indexing && document.getElementById('result').textContent.includes('projectIndex')",timeout=90000)
            after = page.evaluate("({path:selected,title:document.querySelector('#inspector h3').textContent,page:document.getElementById('source-position').textContent,top:document.getElementById('source').scrollTop,pane:document.getElementById('workspace').dataset.pane})")
            assert before == after,(before,after)
            page.wait_for_function('gate.live.size === 1')
            assert page.evaluate('indexReplies[0].inspectionLinearMemoryCapacityBytes') >= 64*1024*1024
            report['inspection_wasm_capacity_bytes'] = page.evaluate('indexReplies[0].inspectionLinearMemoryCapacityBytes')
            report['index_completion_preserves_view_and_releases_inspection_worker'] = True
            page.locator('#back').click()
            page.wait_for_function("document.querySelector('#inspector h3').textContent === 'long.js' && !busy")
            page.locator('#forward').click()
            page.wait_for_function("document.querySelector('#inspector h3').textContent === 'fn600' && !busy")
            report['background_refresh_does_not_add_history'] = True
            choose('src/main.js'); page.locator('.symbol-button',has_text='run').click()
            assert 'greet' in page.locator('#inspector .relation-button').first.inner_text()
            assert page.evaluate('JSON.parse(document.getElementById("result").textContent).projectIndex.files') == 134
            report['auxiliary_files_excluded_from_shared_index'] = True
            page.evaluate('gate.hold=true')
            page.locator('#folder-input').set_input_files(str(root/'project-b')); held()
            choose('src/main.js'); assert page.evaluate('!!worker.viewer')
            page.locator('#folder-input').set_input_files(str(replacement))
            page.wait_for_function("document.getElementById('project-name').textContent === 'replacement' && indexed && !indexing && !busy")
            page.evaluate('releaseFacts()')
            page.wait_for_function('gate.live.size === 1 && pending.size === 0')
            report['switching_project_terminates_old_pool_and_viewer'] = True
            page.evaluate('gate.hold=true')
            page.locator('#folder-input').set_input_files(str(root/'project-b')); held()
            choose('src/main.js')
            page.locator('#cancel').click()
            page.wait_for_function('!busy && !currentFiles.length && gate.live.size === 1 && pending.size === 0')
            page.evaluate('releaseFacts()')
            report['cancel_terminates_all_project_workers'] = True
            page.locator('#folder-input').set_input_files(str(auxiliary)); opened('README.md')
            assert not page.evaluate('indexing || indexed')
            assert 'Only documentation.' in page.locator('#source').inner_text()
            report['auxiliary_only_project_is_browsable'] = True
            assert not errors,errors
            report['page_errors']=errors
            if args.real_project:
                report['real_single_parser_index'] = check_single_parser(browser,
                    f'http://127.0.0.1:{server.server_port}/',args.real_project)
            report['passed']=True
            browser.close()
    finally:
        server.shutdown(); server.server_close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__': main()

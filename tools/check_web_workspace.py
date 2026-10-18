#!/usr/bin/env python3
"""Exercise the four-pane workspace, navigation, source paging and splitters."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import tempfile
import threading
import time
from playwright.sync_api import sync_playwright
from check_web_prototype import Quiet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browser', choices=('chromium', 'webkit'), default='chromium')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Quiet, directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {'browser': args.browser, 'passed': False}
    try:
        with sync_playwright() as p, tempfile.TemporaryDirectory(prefix='lumencode-workspace-', dir=Path.home()) as temporary:
            fixture = Path(temporary) / 'workspace-fixture' / 'src'
            fixture.mkdir(parents=True)
            (fixture/'helper.js').write_text('export function greet(name) { return `Hello ${name}`; }\n')
            (fixture/'main.js').write_text('import { greet } from "./helper.js";\nexport function run() { return greet("world"); }\n')
            (fixture/'long.js').write_text('const unsafe = "<img src=x onerror=alert(1)>";\n' + '\n'.join(f'export function fn{i}() {{ return {i}; }}' for i in range(650)))
            browser = (p.chromium.launch(executable_path='/snap/bin/chromium', args=['--no-sandbox'])
                       if args.browser == 'chromium' else p.webkit.launch())
            context = browser.new_context(viewport={'width':1440, 'height':950}, service_workers='block')
            page = context.new_page()
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('import').disabled", timeout=90000)
            page.locator('#folder-input').set_input_files(str(fixture.parent))
            page.wait_for_function("!document.getElementById('index').disabled")
            for pane in ('.project-pane','.overview-pane','.inspector-pane','.preview-pane'):
                assert page.locator(pane).is_visible(), pane
            report['four_panes_visible'] = True
            page.locator('#search').fill('main')
            page.wait_for_function("document.querySelectorAll('#search-results button').length === 1")
            page.locator('#search-results button').click()
            page.wait_for_function("document.getElementById('selected-name').textContent === 'src/main.js'")
            page.locator('#search').press('Escape')
            page.wait_for_function("!document.getElementById('files').hidden")
            report['file_search_before_index'] = True
            page.locator('#index').click()
            page.wait_for_function("document.getElementById('search').dataset.indexed === 'true' && !document.getElementById('search').disabled")
            page.locator('.symbol-button', has_text='run').click()
            assert page.locator('#inspector h3').inner_text() == 'run'
            assert page.locator('.source-line.selected').get_attribute('data-line') == '2'
            page.screenshot(path=str(args.output/'desktop.png'))
            page.locator('#inspector .relation-button', has_text='greet').click()
            page.wait_for_function("document.getElementById('selected-name').textContent === 'src/helper.js' && !document.getElementById('back').disabled")
            assert page.locator('#inspector h3').inner_text() == 'greet'
            assert 'name' in page.locator('#inspector').inner_text()
            assert page.locator('.source-line.selected').get_attribute('data-line') == '1'
            report['symbol_inspector_parameters_and_cross_file_navigation'] = True
            page.locator('#back').click()
            page.wait_for_function("document.getElementById('selected-name').textContent === 'src/main.js' && document.querySelector('#inspector h3').textContent === 'run' && !document.getElementById('forward').disabled")
            page.locator('#forward').click()
            page.wait_for_function("document.getElementById('selected-name').textContent === 'src/helper.js' && !document.getElementById('back').disabled")
            report['back_forward_restore_symbol_and_file'] = True
            # Mouse and keyboard resizing persist on reload.
            divider = page.locator('#project-divider')
            before = page.locator('.project-pane').bounding_box()['width']
            box = divider.bounding_box()
            page.mouse.move(box['x']+2, box['y']+50); page.mouse.down()
            page.mouse.move(box['x']+52, box['y']+50); page.mouse.up()
            divider.focus(); divider.press('ArrowRight')
            after = page.locator('.project-pane').bounding_box()['width']
            assert after >= before + 60
            overview_before = page.locator('.overview-pane').bounding_box()['width']
            page.locator('#overview-divider').focus(); page.locator('#overview-divider').press('ArrowRight')
            assert page.locator('.overview-pane').bounding_box()['width'] >= overview_before + 19
            source_before = page.locator('.preview-pane').bounding_box()['height']
            page.locator('#source-divider').focus(); page.locator('#source-divider').press('ArrowUp')
            assert page.locator('.preview-pane').bounding_box()['height'] >= source_before + 19
            page.set_viewport_size({'width':900,'height':950})
            assert not page.evaluate('document.documentElement.scrollWidth > innerWidth')
            page.set_viewport_size({'width':1440,'height':950})
            report['pointer_and_keyboard_resize'] = True
            page.locator('#files button[data-path="src/long.js"]').click()
            page.wait_for_function("document.getElementById('selected-name').textContent === 'src/long.js' && !document.getElementById('index').disabled")
            assert page.locator('#source .source-line').count() == 160
            assert page.locator('#source img').count() == 0
            page.locator('#source-next').click()
            assert page.locator('#source .source-line').first.get_attribute('data-line') == '161'
            page.locator('#source-previous').click()
            assert page.locator('#source .source-line').first.get_attribute('data-line') == '1'
            page.locator('.symbol-button', has_text='fn600').click()
            assert page.locator('.source-line.selected').get_attribute('data-line') == '602'
            assert page.locator('#source .source-line').count() <= 160
            report['bounded_source_paging_and_focused_line'] = True
            report['source_html_rendered_as_text'] = True
            # Narrow layouts retain all panes via explicit tabs, with no overflow.
            page.set_viewport_size({'width':390,'height':844})
            for name, selector in [('project','.project-pane'),('overview','.overview-pane'),('inspector','.inspector-pane'),('preview','.preview-pane')]:
                page.locator(f'[role=tab][data-pane={name}]').click()
                assert page.locator(selector).is_visible()
                assert not page.evaluate('document.documentElement.scrollWidth > innerWidth')
            page.locator('[role=tab][data-pane=preview]').focus()
            page.locator('[role=tab][data-pane=preview]').press('ArrowRight')
            assert page.locator('[role=tab][data-pane=project]').get_attribute('aria-selected') == 'true'
            page.locator('[role=tab][data-pane=preview]').click()
            page.screenshot(path=str(args.output/'mobile.png'))
            report['mobile_all_panes_accessible_without_overflow'] = True
            page.set_viewport_size({'width':1440,'height':950})
            page.reload()
            page.wait_for_function("!document.getElementById('import').disabled", timeout=90000)
            assert abs(page.locator('.project-pane').bounding_box()['width'] - after) < 2
            report['pane_size_survives_reload'] = True
            assert not errors, errors
            report['page_errors'] = errors
            report['passed'] = True
            browser.close()
    finally:
        server.shutdown(); server.server_close()
        (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

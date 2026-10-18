#!/usr/bin/env python3
"""Exercise browser workspace keyboard flows and focus across asynchronous updates."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import tempfile
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import Quiet


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--browser',choices=('chromium','webkit'),default='chromium')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report={'passed':False,'browser':args.browser,'errors':[]}
    try:
        with sync_playwright() as p,tempfile.TemporaryDirectory(prefix='lumencode-keyboard-',dir=Path.home()) as temporary:
            root=Path(temporary)/'project';root.mkdir()
            (root/'helper.js').write_text('export function greet(name) { return `Hello ${name}`; }\n')
            (root/'main.js').write_text('import { greet } from "./helper.js";\nexport function run() { return greet("world"); }\n')
            (root/'long.js').write_text('\n'.join(f'export function fn{i}() {{ return {i}; }}' for i in range(200)))
            browser=(p.chromium.launch(executable_path='/snap/bin/chromium',headless=True,args=['--no-sandbox']) if args.browser=='chromium' else p.webkit.launch(headless=True))
            context=browser.new_context(viewport={'width':1440,'height':950},service_workers='block')
            page=context.new_page();page.on('pageerror',lambda error:report['errors'].append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
            page.locator('#folder-input').set_input_files(str(root))
            page.wait_for_function('indexed && !busy && !indexing')
            def search(query):
                page.keyboard.press('Control+f');page.keyboard.type(query)
                page.keyboard.press('ArrowDown')
                page.wait_for_function("document.activeElement.parentElement?.id==='search-results'")
            search('main.js')
            page.keyboard.press('ArrowUp')
            assert page.locator('#search').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('ArrowDown');page.keyboard.press('Enter')
            page.wait_for_function("selected==='main.js' && !busy && document.activeElement.closest('#overview-pane')")
            assert 'run' in page.locator(':focus').inner_text()
            report['search_arrows_enter_focus_overview']=True
            page.keyboard.press('Enter')
            assert page.locator('#inspector h3').inner_text()=='run'
            assert page.locator('#inspector-pane').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Tab')
            assert 'greet' in page.locator(':focus').inner_text()
            # Exercise the real update renderer with the current real engine result.
            key=page.locator(':focus').get_attribute('data-focus-key')
            page.evaluate("Workspace.update(JSON.parse(document.getElementById('result').textContent))")
            assert page.locator(':focus').get_attribute('data-focus-key')==key
            page.keyboard.press('Enter')
            page.wait_for_function("selected==='helper.js' && !busy && document.querySelector('#inspector h3').textContent==='greet'")
            assert page.locator('#inspector-pane').evaluate('(e)=>e===document.activeElement')
            report['symbol_reference_follow_and_update_focus']=True
            page.keyboard.press('Alt+ArrowLeft')
            page.wait_for_function("selected==='main.js' && !busy && document.querySelector('#inspector h3').textContent==='run'")
            assert page.locator('#inspector-pane').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Alt+ArrowRight')
            page.wait_for_function("selected==='helper.js' && !busy && document.querySelector('#inspector h3').textContent==='greet'")
            report['keyboard_history_restores_details_and_focus']=True
            page.keyboard.press('F6')
            assert page.locator('#source').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('F6')
            assert page.locator('#search').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Shift+F6')
            assert page.locator('#source').evaluate('(e)=>e===document.activeElement')
            report['forward_reverse_pane_cycle']=True
            search('long.js');page.keyboard.press('Enter')
            page.wait_for_function("selected==='long.js' && !busy && document.activeElement.closest('#symbols')")
            page.keyboard.press('ArrowDown')
            assert 'fn1' in page.locator(':focus').inner_text()
            page.keyboard.press('End')
            assert 'fn199' in page.locator(':focus').inner_text()
            page.keyboard.press('Enter')
            assert page.locator('.source-line.selected').get_attribute('data-line')=='200'
            page.keyboard.press('F6')
            page.keyboard.press('Shift+Tab');assert page.locator('#copy-source').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Shift+Tab');assert page.locator('#source-previous').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Enter')
            assert page.locator('#source').evaluate('(e)=>e===document.activeElement')
            assert page.locator('.source-line').first.get_attribute('data-line')=='1'
            report['symbol_arrows_and_source_page_boundary_focus']=True
            # Returning to a selected symbol should reveal it after the pane was scrolled away.
            page.evaluate("document.querySelector('.overview-content').scrollTop=0")
            page.keyboard.press('F6');page.keyboard.press('F6')
            assert 'fn199' in page.locator(':focus').inner_text()
            assert page.locator(':focus').evaluate("e=>{const row=e.getBoundingClientRect(),pane=e.closest('.overview-content').getBoundingClientRect();return row.top>=pane.top && row.bottom<=pane.bottom;}")
            report['pane_cycle_reveals_selected_symbol']=True
            search('main')
            focused=page.locator(':focus').get_attribute('data-focus-key')
            page.evaluate('searchProject()')
            assert page.locator(':focus').get_attribute('data-focus-key')==focused
            page.keyboard.press('Escape')
            assert page.locator('#search').evaluate('(e)=>e===document.activeElement')
            assert page.locator('#search').input_value()==''
            assert page.locator('#search-results').is_hidden()
            report['refreshed_search_results_keep_focus_and_escape_clears']=True
            # Hold an actual old-query engine reply while typing a new query.
            page.evaluate("""()=>{
              window.originalKeyboardRpc=rpc;
              window.releaseOldSearch=null;window.oldSearchWaiting=false;window.oldSearchReleased=false;
              const gate=new Promise(resolve=>window.releaseOldSearch=resolve);
              rpc=async(operation,extra={})=>{
                const result=await originalKeyboardRpc(operation,extra);
                if(operation==='search' && extra.query==='helper'){
                  oldSearchWaiting=true;await gate;oldSearchReleased=true;
                }
                return result;
              };
            }""")
            page.keyboard.press('Control+f');page.keyboard.type('helper');page.keyboard.press('Enter')
            page.wait_for_function('oldSearchWaiting')
            page.keyboard.press('Control+a');page.keyboard.type('main.js');page.keyboard.press('ArrowDown')
            page.wait_for_function("document.activeElement.parentElement?.id==='search-results'")
            focused=page.locator(':focus').get_attribute('data-focus-key')
            page.evaluate('releaseOldSearch()');page.wait_for_function('oldSearchReleased')
            page.evaluate('() => { rpc=originalKeyboardRpc; }')
            assert page.locator(':focus').get_attribute('data-focus-key')==focused
            assert 'main.js' in page.locator(':focus').inner_text()
            assert 'helper.js' not in page.locator('#search-results').inner_text()
            page.keyboard.press('Escape')
            report['old_search_reply_cannot_replace_new_query_or_focus']=True
            page.locator('#project-menu > summary').focus();page.keyboard.press('Enter');page.keyboard.press('Tab')
            assert page.locator('#open').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('Escape')
            assert page.locator('#project-menu > summary').evaluate('(e)=>e===document.activeElement')
            assert not page.locator('#project-menu').evaluate('(e)=>e.open')
            report['menu_escape_restores_summary_focus']=True
            page.set_viewport_size({'width':390,'height':844})
            search('helper.js');page.keyboard.press('Enter')
            page.wait_for_function("selected==='helper.js' && !busy && document.activeElement.closest('#overview-pane')")
            assert page.locator('#overview-pane').is_visible()
            page.keyboard.press('Enter')
            assert page.locator('#inspector-pane').is_visible()
            assert page.locator('#inspector-pane').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('F6');assert page.locator('#source').is_visible()
            assert page.locator('#source').evaluate('(e)=>e===document.activeElement')
            page.locator('[role=tab][data-pane=preview]').focus();page.keyboard.press('Home')
            assert page.locator('[role=tab][data-pane=project]').evaluate('(e)=>e===document.activeElement')
            page.keyboard.press('ArrowRight');assert page.locator('#overview-pane').is_visible()
            assert page.locator('[role=tab][data-pane=overview]').get_attribute('aria-selected')=='true'
            assert not page.evaluate('document.documentElement.scrollWidth>innerWidth')
            page.screenshot(path=str(args.output/'keyboard-mobile.png'))
            report['compact_panes_keep_focus_visible_and_tabs_work']=True
            assert not report['errors'],report['errors']
            report['passed']=True;browser.close()
    finally:
        server.shutdown();server.server_close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()

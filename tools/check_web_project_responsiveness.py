#!/usr/bin/env python3
"""Measure real UI indexing/navigation on an imported project, headlessly."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import Quiet


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root',type=Path,required=True)
    parser.add_argument('--project',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report={'passed':False,'scope':'Headless Chromium, actual app folder-import and navigation; no permission prompts',
            'errors':[],'timing_scope':'Single diagnostic run on this machine, not a performance guarantee'}
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(executable_path='/snap/bin/chromium',headless=True,args=['--no-sandbox'])
            context=browser.new_context(viewport={'width':1440,'height':950},service_workers='block')
            page=context.new_page();page.on('pageerror',lambda error:report['errors'].append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
            page.evaluate("""()=>{
              window.measure={ticks:0,maxGap:0,index:null};let last=performance.now();measure.begin=last;
              window.heartbeat=setInterval(()=>{const now=performance.now();measure.maxGap=Math.max(measure.maxGap,now-last);last=now;measure.ticks++;},10);
              // App replaces its coordinator at subsequent mounts; first mount uses this instance.
              worker.addEventListener('message',({data})=>{if(data.parserWorkers){measure.index=data;measure.completed=performance.now();}});
            }""")
            page.locator('#folder-input').set_input_files(str(args.project.resolve()))
            page.wait_for_function('indexing && !busy',timeout=90000)
            target=page.evaluate(r"""()=>[...analysisFiles].map(path=>path.slice(9)).find(path=>path!==selected && sourceFiles.get(path)?.size<50000 && /\.(js|ts|py|java|swift|cpp)$/.test(path))""")
            assert target,'No source file suitable for navigation check'
            page.evaluate('window.navigationStarted=performance.now();window.indexingAtNavigationStart=indexing')
            page.locator('#search').fill(target)
            page.locator('#search').press('Enter')
            page.wait_for_function("document.querySelectorAll('#search-results button').length>0")
            page.locator('#search-results button').first.click()
            page.wait_for_function('path=>selected===path && !busy',arg=target,timeout=30000)
            report['navigation_during_index']=page.evaluate("""()=>({path:selected,elapsedMs:performance.now()-navigationStarted,
              indexingAtStart:indexingAtNavigationStart,completedBeforeIndex:indexing,
              sourceVisible:document.getElementById('source').textContent.length>0})""")
            assert report['navigation_during_index']['completedBeforeIndex'],report['navigation_during_index']
            page.locator('#search').press('Escape')
            page.wait_for_function('indexed && !indexing && !busy && measure.index',timeout=120000)
            page.wait_for_function("document.querySelector('#source [class^=syntax-]')",timeout=5000)
            report['cold']=page.evaluate("""()=>{clearInterval(heartbeat);return {wallIncludingImportMs:measure.completed-measure.begin,
              indexMs:measure.index.elapsedMs,workers:measure.index.parserWorkers,index:measure.index.result,
              maxTimerGapMs:measure.maxGap,heartbeatTicks:measure.ticks,selectedFiles:currentFiles.length,
              mountedBytes:[...sourceFiles.values()].reduce((sum,file)=>sum+file.size,0),
              aggregateWasmCapacityBytes:measure.index.aggregateLinearMemoryCapacityBytes};}""")
            assert report['cold']['index']['failed']==0
            assert page.evaluate('selected')==target
            page.screenshot(path=str(args.output/'real-project-desktop.png'))
            page.evaluate('window.warmBegin=performance.now()')
            page.locator('#folder-input').set_input_files(str(args.project.resolve()))
            page.wait_for_function('indexed && !indexing && !busy && document.getElementById("status").textContent.includes("reused")',timeout=90000)
            report['warm']=page.evaluate("""()=>({wallIncludingContentVerificationMs:performance.now()-warmBegin,status:document.getElementById('status').textContent})""")
            assert f"{report['cold']['index']['files']} reused" in report['warm']['status']
            assert not report['errors'],report['errors']
            report['passed']=True;browser.close()
    finally:
        server.shutdown();server.server_close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()

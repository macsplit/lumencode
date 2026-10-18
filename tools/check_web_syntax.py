#!/usr/bin/env python3
"""Check asynchronous source/excerpt colouring, safety, bounds and worker recovery."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright
from check_web_prototype import Quiet

GATE = """(() => {
  const Native = window.Worker;
  window.syntaxHeld = []; window.holdSyntax = false;
  window.Worker = class extends Native {
    constructor(url, options) {
      super(url, options);
      if (String(url).endsWith('syntax-worker.js')) {
        const send = this.postMessage.bind(this);
        this.postMessage = (...args) => window.holdSyntax ? syntaxHeld.push(()=>send(...args)) : send(...args);
      }
    }
  };
})();"""
SHOW = """({text,path='sample.js',language='script',line=1}) => {
  const result={path:'/project/'+path,fileName:path,language,summary:'Source',symbols:[
    {name:'sample',kind:'function',line,parameters:[{name:'value',type:'string'}],snippet:text.slice(0,500)}]};
  Workspace.show(result,text,path,{name:'sample',line,record:false});
}"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--browser', choices=('chromium','webkit'), default='chromium')
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report = {'browser':args.browser,'passed':False}
    try:
        with sync_playwright() as p:
            browser = (p.chromium.launch(executable_path='/snap/bin/chromium',args=['--no-sandbox'])
                       if args.browser=='chromium' else p.webkit.launch())
            context = browser.new_context(viewport={'width':1440,'height':950}, service_workers='block')
            page = context.new_page(); page.add_init_script(GATE)
            errors = []; page.on('pageerror',lambda error:errors.append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
            fixtures = [
                ('script','x.js','export function sample(value) { return "hello"; }'),
                ('script','crlf.js','// first\r\nexport const sample = `hello\r\nworld`;\r\n'),
                ('ts','x.ts','export function sample(value: string): string { return value; }'),
                ('tsx','x.tsx','export const Sample = () => <div title="hello">world</div>;'),
                ('python','x.py','def sample(value):\n    return "hello"'),
                ('swift','x.swift','func sample(value: String) -> String { return "hello" }'),
                ('cpp','x.cpp','int sample(int value) { return 42; }'),
                ('csharp','x.cs','public string Sample() { return "hello"; }'),
                ('php','x.php','<?php function sample($value) { return "hello"; }'),
                ('rust','x.rs','pub fn sample() -> i32 { return 42; }'),
                ('sql','x.sql',"SELECT name FROM items WHERE id = 42;"),
                ('html','x.html','<style>p { color: red; }</style><p title="hello">world</p>'),
                ('unknown','x.yaml','name: hello\nenabled: true'),
                ('vbnet','x.vb','Public Function Sample() As String\nReturn "hello"\nEnd Function'),
            ]
            for language,path,text in fixtures:
                page.evaluate(SHOW,{'text':text,'path':path,'language':language})
                page.wait_for_function("document.querySelector('#source [class^=syntax-]')")
                page.wait_for_function("document.querySelector('#inspector .signature [class^=syntax-]')")
                shown = page.locator('.source-line').evaluate_all("rows=>rows.map(row=>[...row.childNodes].slice(1).map(node=>node.textContent).join('')).join('')")
                assert shown == text.replace('\r\n','\n').replace('\r','\n')+'\n',(path,shown)
                assert page.locator('#inspector .signature').last.text_content()==text
            report['languages_with_exact_source_and_excerpt_text']=[item[1] for item in fixtures]
            copied='// original\r\nexport const sample = 42;\r\n'
            page.evaluate(SHOW,{'text':copied})
            page.evaluate("Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async text=>window.copiedSource=text}})")
            page.locator('#copy-source').click()
            assert page.evaluate('copiedSource')==copied
            report['copy_preserves_original_line_endings']=True
            text='const unsafe = "<img src=x onerror=alert(1)>";\n/* comment starts\n'+'still a comment\n'*170+'*/\nexport function sample() { return "done"; }'
            page.evaluate(SHOW,{'text':text,'line':174})
            page.wait_for_function("document.querySelector('#source .syntax-keyword')")
            assert page.locator('.source-line[data-line="161"] .syntax-comment').inner_text()=='still a comment'
            assert page.locator('#source img, #inspector img').count()==0
            assert page.locator('.source-line.selected').get_attribute('data-line')=='174'
            page.screenshot(path=str(args.output/'syntax-desktop.png'))
            page.locator('#source-previous').click()
            assert page.locator('.source-line[data-line="159"] .syntax-comment').inner_text()=='still a comment'
            report['multiline_page_context_and_focused_line']=True
            report['literal_html_safe']=True
            # Delay worker delivery, then navigate before the old result arrives.
            page.evaluate('Syntax.reset(); window.holdSyntax=true')
            page.evaluate(SHOW,{'text':'export const oldFile = "old";','path':'old.js'})
            page.evaluate(SHOW,{'text':'def newer():\n    return "new"','path':'new.py','language':'python'})
            page.evaluate('window.holdSyntax=false; syntaxHeld.splice(0).forEach(send=>send())')
            page.wait_for_function("document.querySelector('#source .syntax-keyword')?.textContent === 'def'")
            assert 'oldFile' not in page.locator('#source').text_content()
            report['stale_file_results_ignored']=True
            # Colouring arriving after a page/scroll change must preserve that state.
            page.evaluate('Syntax.reset(); window.holdSyntax=true')
            paged='\n'.join(f'export const entry{i} = "'+('x'*220)+'";' for i in range(350))
            page.evaluate(SHOW,{'text':paged,'path':'paged.js','line':321})
            page.evaluate("document.getElementById('source').scrollTop=100; document.getElementById('source').scrollLeft=140")
            before=page.locator('#source').evaluate('(e)=>[e.scrollTop,e.scrollLeft]')
            page.evaluate('window.holdSyntax=false; syntaxHeld.splice(0).forEach(send=>send())')
            page.wait_for_function("document.querySelector('#source .syntax-keyword')")
            assert page.locator('#source').evaluate('(e)=>[e.scrollTop,e.scrollLeft]')==before
            assert page.locator('.source-line').first.get_attribute('data-line')=='321'
            report['async_highlight_preserves_page_and_scroll']=True
            # Deliberately withhold work. Main-thread timers keep running; deadline
            # resolves to plain text and a fresh worker can highlight the next file.
            recovery=page.evaluate("""async () => {
              Syntax.reset(); holdSyntax=true;
              let ticks=0; const timer=setInterval(()=>ticks++,10), start=performance.now();
              const value=await Syntax.highlight('const blocked = 1;', 'script', 'blocked.js');
              clearInterval(timer); holdSyntax=false; syntaxHeld=[];
              const recovered=await Syntax.highlight('const recovered = 2;', 'script', 'recovered.js');
              return {elapsedMs:performance.now()-start,ticks,fallback:value===null,recovered:!!recovered};
            }""")
            assert recovery['fallback'] and recovery['recovered'] and recovery['ticks']>=20,recovery
            report['worker_timeout_recovery']=recovery
            fallback=page.evaluate("""async () => ({large:await Syntax.highlight('x'.repeat(524289),'script','large.js'),
              unknown:await Syntax.highlight('nothing here','unknown','notes.unknown'),
              dense:await Syntax.highlight('export function sample(value) { return \"hello\"; }\\n'.repeat(6000),'script','dense.js')})""")
            assert fallback=={'large':None,'unknown':None,'dense':None}
            # Bounded real tokenisation, with a main-thread heartbeat.
            report['bounded_highlight_sample']=page.evaluate("""async () => {
              Syntax.reset(); let ticks=0; const timer=setInterval(()=>ticks++,10), start=performance.now();
              const result=await Syntax.highlight('export function sample(value) { return "hello"; }\\n'.repeat(3000),'script','large.js');
              clearInterval(timer); return {elapsedMs:performance.now()-start,ticks,highlighted:!!result};
            }""")
            assert report['bounded_highlight_sample']['highlighted']
            report['size_and_unknown_language_plain_fallback']=True
            assert not errors,errors
            browser.close(); report['passed']=True
    finally:
        server.shutdown()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()

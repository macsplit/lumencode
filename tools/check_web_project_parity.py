#!/usr/bin/env python3
"""Compare complete native/browser JSON on real repositories, without opening a GUI.

Whole-project mode compares every indexed candidate before/after graph building.
Corpus mode samples up to three bounded files per focus language in corpus.json.
Only the physical root prefix is normalized; no fields or array order are ignored.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from playwright.sync_api import sync_playwright
from check_web_prototype import HARNESS, Quiet, ROOT, normalized

EXCLUDED = {'node_modules','dist','build','coverage','vendor','__pycache__','venv',
            'target','DerivedData','bower_components','bin','obj'}
FOCUS = {'web':{'.html','.htm','.css','.js'},'js':{'.js','.mjs','.cjs'},
         'ts':{'.ts','.tsx'},'python':{'.py'},'php':{'.php'},'java':{'.java'},
         'csharp':{'.cs'},'rust':{'.rs'},'swift':{'.swift'},'go':{'.go'},
         'c':{'.c','.h'},'cpp':{'.cpp','.cc','.hpp','.h'},'ruby':{'.rb'},
         'kotlin':{'.kt','.kts'},'bash':{'.sh','.bash'},'sql':{'.sql'},
         'qml':{'.qml'},'objc':{'.m','.mm','.h'},'vbnet':{'.vb'}}


def difference(a,b,path='$'):
    if type(a) is not type(b): return path+': type'
    if isinstance(a,dict):
        if a.keys()!=b.keys(): return path+': keys '+str(sorted(a.keys()^b.keys()))
        for key in a:
            if a[key]!=b[key]: return difference(a[key],b[key],path+'.'+key)
    if isinstance(a,list):
        if len(a)!=len(b): return path+': length '+str((len(a),len(b)))
        for i,(left,right) in enumerate(zip(a,b)):
            if left!=right:return difference(left,right,f'{path}[{i}]')
    return path+': value'


def diagnostic_value(value, root_name, root_labels=False, relation_order=False, field=''):
    """Secondary diagnosis only. The primary comparison remains exact JSON."""
    if isinstance(value, dict):
        result={key:diagnostic_value(item,root_name,root_labels,relation_order,key) for key,item in value.items()}
        if root_labels and value.get('path')=='/project' and value.get('label')==root_name:
            result['label']='project'
        return result
    if isinstance(value, list):
        result=[diagnostic_value(item,root_name,root_labels,relation_order) for item in value]
        return sorted(result,key=lambda item:json.dumps(item,sort_keys=True)) if relation_order and field in ('calls','calledBy') else result
    return value


def classify(expected, actual, root_name):
    for label,labels,order in [('root display label',True,False),('relation array order',False,True),
                               ('root display label and relation array order',True,True)]:
        if diagnostic_value(expected,root_name,labels,order)==diagnostic_value(actual,root_name,labels,order):return label
    return 'content'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root',type=Path,required=True)
    parser.add_argument('--native-helper',type=Path,required=True)
    parser.add_argument('--project',type=Path,action='append',default=[])
    parser.add_argument('--corpus-root',type=Path)
    parser.add_argument('--browser',choices=('chromium','webkit'),default='chromium')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not args.project and not args.corpus_root:parser.error('Provide --project or --corpus-root')
    args.output.mkdir(parents=True,exist_ok=True)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report={'passed':False,'browser':args.browser,'method':'Complete JSON; physical root prefix normalization only',
            'projects':[],'corpus_samples':[],'differences':[],'errors':[],
            'native_helper_sha256':hashlib.sha256(args.native_helper.read_bytes()).hexdigest(),
            'wasm_sha256':hashlib.sha256((args.web_root/'engine.wasm').read_bytes()).hexdigest(),
            'repository_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()}
    native_env={**os.environ,'QT_HASH_SEED':'0'}
    def native(root,path=None,indexed=False,cache=None):
        cmd=[str(args.native_helper.resolve())]
        cmd+=['--root',str(root),'--cache',str(cache)] if indexed else []
        cmd+=['--path' if indexed else '--dump-file',str(root/path)] if path else ['--build-index']
        return json.loads(subprocess.check_output(cmd,env=native_env,timeout=60))
    def compare(root,path,expected,actual,stage,folder):
        expected=normalized(expected,root)
        if expected!=actual:
            report['differences'].append({'project':root.name,'path':path,'stage':stage,'first_difference':difference(expected,actual),'difference_kind':classify(expected,actual,root.name)})
            out=folder/(stage+'-'+hashlib.sha256(path.encode()).hexdigest()[:12]+'.json')
            out.write_text(json.dumps({'path':path,'native':expected,'browser':actual},indent=2)+'\n')
    try:
        with sync_playwright() as p,ThreadPoolExecutor(max_workers=4) as executor:
            browser=(p.chromium.launch(executable_path='/snap/bin/chromium',headless=True,args=['--no-sandbox'])
                     if args.browser=='chromium' else p.webkit.launch(headless=True))
            report['browser_version']=browser.version
            context=browser.new_context(service_workers='block')
            page=context.new_page();page.on('pageerror',lambda error:report['errors'].append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
            page.evaluate("document.getElementById('folder-input').onchange=null")
            def mount(root):
                page.evaluate('if(window.testWorker)testWorker.terminate()')
                page.locator('#folder-input').set_input_files(str(root))
                page.evaluate(HARNESS);page.evaluate('workerReady')
                return page.evaluate("""async excluded=>{
                  const files=[...document.getElementById('folder-input').files].map(file=>({
                    path:file.webkitRelativePath.split('/').slice(1).join('/'),file
                  })).filter(({path})=>!path.split('/').slice(0,-1).some(part=>part.startsWith('.')||excluded.includes(part)));
                  return testRequest('mount',{files});
                }""",sorted(EXCLUDED))
            def request(operation,**extra):
                return page.evaluate("""async ({operation,extra})=>{
                  let timer;try{return await Promise.race([testRequest(operation,extra),new Promise((_,reject)=>{
                    timer=setTimeout(()=>reject(Error(operation+' exceeded 60 seconds')),60000);
                  })]);}finally{clearTimeout(timer);}
                }""",{'operation':operation,'extra':extra})
            for root in args.project:
                root=root.resolve();folder=args.output/root.name;folder.mkdir(exist_ok=True)
                print('Project:',root.name,flush=True)
                mounted=mount(root);paths=[path.removeprefix('/project/') for path in mounted['result']['paths']]
                if not paths:raise AssertionError('No project candidates: '+str(root))
                cache=folder/'native-index.json'
                # A fresh cache ensures native cold stats reflect this run.
                if cache.exists():raise RuntimeError('Use a fresh output directory: '+str(cache))
                native_index=executor.submit(native,root,None,True,cache)
                captures={}
                languages=Counter();modes=Counter()
                for path in paths:
                    future=executor.submit(native,root,path)
                    captures[path]=request('analyse',path=path)['result']
                    compare(root,path,future.result(),captures[path],'file',folder)
                    languages[captures[path]['language']]+=1
                    modes[captures[path].get('analysisSourceMode','unknown')]+=1
                measured=page.evaluate("""async()=>{
                  let last=performance.now(),ticks=0,maxGap=0;const begin=last;
                  const timer=setInterval(()=>{const now=performance.now();maxGap=Math.max(maxGap,now-last);last=now;ticks++;},10);
                  let deadline;
                  try{const reply=await Promise.race([testRequest('index',{workers:4}),new Promise((_,reject)=>{deadline=setTimeout(()=>reject(Error('Index exceeded 60 seconds')),60000);})]);return {reply,ticks,maxGap:Math.max(maxGap,performance.now()-last),wallMs:performance.now()-begin};}
                  finally{clearInterval(timer);clearTimeout(deadline);}
                }""")
                stats=native_index.result()
                if stats['files']!=len(paths):raise AssertionError(('Candidate count differs',root.name,stats['files'],len(paths)))
                indexed={}
                for path in paths:
                    future=executor.submit(native,root,path,True,cache)
                    indexed[path]=request('analyse',path=path,indexed=True)['result']
                    compare(root,path,future.result(),indexed[path],'indexed',folder)
                (folder/'browser-file.json').write_text(json.dumps(captures)+'\n')
                (folder/'browser-indexed.json').write_text(json.dumps(indexed)+'\n')
                report['projects'].append({'name':root.name,'files_compared':len(paths),'languages':dict(languages),
                    'modes':dict(modes),'native_index':stats,'browser_index':measured['reply']['result'],
                    'browser_index_ms':measured['wallMs'],'heartbeat_ticks':measured['ticks'],'max_timer_gap_ms':measured['maxGap'],
                    'parser_workers':measured['reply']['parserWorkers'],
                    'candidate_source_sha256':hashlib.sha256(json.dumps({path:hashlib.sha256((root/path).read_bytes()).hexdigest() for path in sorted(paths)},sort_keys=True).encode()).hexdigest()})
                print('Compared:',root.name,len(paths),'files and indexed analyses',flush=True)
            if args.corpus_root:
                for entry in json.loads((ROOT/'tools/corpus.json').read_text())['repos']:
                    root=(args.corpus_root/entry['name']).resolve()
                    # Deliberately bounded sample, not a claim about every corpus file.
                    groups={focus:[] for focus in entry['focus']}
                    for path in sorted(root.rglob('*')):
                        if not path.is_file() or path.stat().st_size>262144 or path.stat().st_size<100:continue
                        if any(part.startswith('.') or part in EXCLUDED for part in path.relative_to(root).parts[:-1]):continue
                        for focus in groups:
                            if path.suffix.lower() in FOCUS[focus]:groups[focus].append(path)
                    chosen=set()
                    for paths in groups.values():
                        if paths:chosen.update([paths[0],paths[len(paths)//2],paths[-1]])
                    if not chosen:raise AssertionError('No corpus sample: '+entry['name'])
                    mount(root);folder=args.output/('sample-'+root.name);folder.mkdir(exist_ok=True)
                    samples=[]
                    for file in sorted(chosen):
                        path=str(file.relative_to(root));future=executor.submit(native,root,path)
                        reply=request('analyse',path=path);actual=reply['result']
                        compare(root,path,future.result(),actual,'sample',folder)
                        samples.append({'path':path,'sha256':hashlib.sha256(file.read_bytes()).hexdigest(),
                            'language':actual['language'],'mode':actual.get('analysisSourceMode'),
                            'ast_errors':actual.get('analysisHasAstErrors',False),'browser_ms':reply['elapsedMs']})
                    report['corpus_samples'].append({'name':root.name,'manifest_revision':entry['commit'],'files':samples})
                    print('Sampled:',root.name,len(samples),flush=True)
            browser.close()
            report['passed']=not report['differences'] and not report['errors']
            report['content_equivalent']=not report['errors'] and all(item['difference_kind']!='content' for item in report['differences'])
    except Exception as error:
        report['errors'].append(str(error));raise
    finally:
        server.shutdown();server.server_close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':report['passed'],'project_files':sum(p['files_compared'] for p in report['projects']),
        'sample_files':sum(len(p['files']) for p in report['corpus_samples']),'strict_differences':len(report['differences']),'difference_kinds':dict(Counter(d['difference_kind'] for d in report['differences'])),'errors':report['errors']},indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())

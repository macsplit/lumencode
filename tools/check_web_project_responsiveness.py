#!/usr/bin/env python3
"""Measure real UI indexing/navigation on an imported project, headlessly."""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
import time
from playwright.sync_api import sync_playwright
from check_web_prototype import Quiet


PROFILE = r"""(() => {
  const profile = window.startupProfile = {clock:performance.now(),workers:[],spans:[],longTasks:[],gaps:[]};
  performance.mark('lumencode-profile-clock');
  let last=performance.now();
  profile.timer=setInterval(()=>{const now=performance.now();if(now-last>50)profile.gaps.push({start:last,end:now,duration:now-last});last=now;},10);
  new PerformanceObserver(list=>{for(const entry of list.getEntries())profile.longTasks.push({start:entry.startTime,duration:entry.duration});}).observe({type:'longtask',buffered:true});
  const Native=Worker, ids=new WeakMap();
  const span=(label,start,extra={})=>profile.spans.push({label,start,end:performance.now(),...extra});
  const post=Native.prototype.postMessage;
  Native.prototype.postMessage=function(data,...args){
    if(window.firstCloneWorker && data?.operation==='mount' && !profile.firstCloneControl){
      const begin=performance.now();post.call(firstCloneWorker,data);
      profile.firstCloneControl={files:data.files.length,start:begin,end:performance.now(),synchronousMs:performance.now()-begin,wasm:false};
    }
    const start=performance.now();
    try{return post.call(this,data,...args);}
    finally{span('postMessage '+(data?.operation || 'syntax'),start,{worker:ids.get(this),files:data?.files?.length,facts:data?.facts?Object.keys(data.facts).length:undefined});}
  };
  window.Worker=class extends Native {
    constructor(url,options){
      const start=performance.now();super(url,options);
      const id=profile.workers.length;ids.set(this,id);
      const info={id,url:String(url),created:start};profile.workers.push(info);
      span('Worker constructor',start,{worker:id});
      this.addEventListener('message',({data})=>{if(data.type==='ready')info.ready=performance.now();});
    }
  };
  profile.wrap=(object,key)=>{
    const original=object[key];object[key]=function(...args){
      const start=performance.now();let result;
      try{result=original.apply(this,args);}catch(error){span(key,start);throw error;}
      if(result?.then)return result.then(value=>{span(key,start);return value;},error=>{span(key,start);throw error;});
      span(key,start);return result;
    };
  };
})();"""


def save_profile(page, client, output):
    page_data=page.evaluate("""()=>{
      clearInterval(startupProfile.timer);
      const {wrap,timer,...data}=startupProfile;return data;
    }""")
    complete=[];client.on('Tracing.tracingComplete',lambda event:complete.append(event))
    client.send('Tracing.end')
    deadline=time.monotonic()+30
    while not complete:
        if time.monotonic()>deadline:raise TimeoutError('Chrome did not finish tracing within 30 seconds')
        page.wait_for_timeout(50)
    handle=complete[0]['stream'];trace_path=output/'chromium-trace.json'
    with trace_path.open('w') as file:
        while True:
            chunk=client.send('IO.read',{'handle':handle});file.write(chunk['data'])
            if chunk.get('eof'):break
    client.send('IO.close',{'handle':handle})
    events=json.loads(trace_path.read_text())['traceEvents']
    clock=next(event for event in events if event['name']=='lumencode-profile-clock')
    main=[event for event in events if (event.get('pid'),event.get('tid'))==(clock['pid'],clock['tid']) and event.get('ph')=='X']
    tasks=[event for event in main if 'RunTask' in event['name'] and event.get('dur',0)>=50000]
    summaries=[]
    for task in sorted(tasks,key=lambda e:e['dur'],reverse=True)[:10]:
        start=(task['ts']-clock['ts'])/1000+page_data['clock'];end=start+task['dur']/1000
        spans=[{**span,'durationMs':span['end']-span['start']} for span in page_data['spans'] if span['start']<end and span['end']>start]
        inner=[event for event in main if task['ts']<=event['ts'] and event['ts']+event.get('dur',0)<=task['ts']+task['dur'] and event is not task]
        children=[{'name':event['name'],'durationMs':event.get('dur',0)/1000,'data':event.get('args',{}).get('data',{})}
                  for event in sorted(inner,key=lambda e:e.get('dur',0),reverse=True)[:8]]
        summaries.append({'startMs':start,'durationMs':task['dur']/1000,'overlappingSpans':spans,'traceChildren':children})
    result={**page_data,'mainThreadTasksOver50Ms':summaries,'traceBytes':trace_path.stat().st_size,
            'trace_scope':'CDP timeline; includes profiling overhead; raw trace retained outside repository'}
    (output/'profile.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-root',type=Path,required=True)
    parser.add_argument('--project',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--first-clone-control',action='store_true',help='With --profile, send the fresh import to a plain worker before the engine')
    parser.add_argument('--profile',action='store_true',help='Capture startup/long tasks and a Chrome timeline trace')
    args=parser.parse_args()
    if args.first_clone_control and not args.profile:parser.error('--first-clone-control requires --profile')
    args.output.mkdir(parents=True,exist_ok=True)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(args.web_root.resolve())))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    report={'passed':False,'scope':'Headless Chromium, actual app folder-import and navigation; no permission prompts',
            'errors':[],'timing_scope':'Single diagnostic run on this machine, not a performance guarantee'}
    try:
        with sync_playwright() as p:
            browser=p.chromium.launch(executable_path='/snap/bin/chromium',headless=True,args=['--no-sandbox'])
            context=browser.new_context(viewport={'width':1440,'height':950},service_workers='block')
            page=context.new_page()
            client=None
            if args.profile:
                page.add_init_script(PROFILE)
                client=context.new_cdp_session(page)
                client.send('Tracing.start',{'categories':'toplevel,devtools.timeline,blink.user_timing,v8,disabled-by-default-devtools.timeline.stack','transferMode':'ReturnAsStream'})
            page.on('pageerror',lambda error:report['errors'].append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}/')
            page.wait_for_function("!document.getElementById('choose-project').disabled",timeout=90000)
            if args.profile:
                page.evaluate("""()=>{
                  startupProfile.initialEngineReady=performance.now();
                  for(const name of ['files','show','update'])startupProfile.wrap(Workspace,name);
                  for(const name of ['mount','analyse','buildRelationships'])startupProfile.wrap(window,name);
                }""")
            if args.first_clone_control:
                page.evaluate("""async()=>{
                  const url=URL.createObjectURL(new Blob(['postMessage({type:"ready"});onmessage=({data})=>postMessage({files:data.files.length});'],{type:'text/javascript'}));
                  window.firstCloneWorker=new Worker(url);
                  await new Promise((resolve,reject)=>{firstCloneWorker.onmessage=resolve;firstCloneWorker.onerror=reject;});
                  URL.revokeObjectURL(url);
                }""")
            page.evaluate("""()=>{
              if(window.startupProfile)startupProfile.importStart=performance.now();
              window.measure={ticks:0,maxGap:0,index:null};let last=performance.now();measure.begin=last;
              window.heartbeat=setInterval(()=>{const now=performance.now();measure.maxGap=Math.max(measure.maxGap,now-last);last=now;measure.ticks++;},10);
              // App replaces its coordinator at subsequent mounts; first mount uses this instance.
              worker.addEventListener('message',({data})=>{if(data.parserWorkers){measure.index=data;measure.completed=performance.now();}});
            }""")
            page.locator('#folder-input').set_input_files(str(args.project.resolve()))
            page.wait_for_function('indexing && !busy',timeout=90000)
            if args.profile:page.evaluate('startupProfile.projectBrowsable=performance.now()')
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
            if args.profile:page.evaluate('startupProfile.coldComplete=performance.now()')
            assert report['cold']['index']['failed']==0
            assert page.evaluate('selected')==target
            page.screenshot(path=str(args.output/'real-project-desktop.png'))
            page.evaluate('window.warmBegin=performance.now();if(window.startupProfile)startupProfile.warmStart=warmBegin')
            page.locator('#folder-input').set_input_files(str(args.project.resolve()))
            page.wait_for_function('indexed && !indexing && !busy && document.getElementById("status").textContent.includes("reused")',timeout=90000)
            report['warm']=page.evaluate("""()=>({wallIncludingContentVerificationMs:performance.now()-warmBegin,status:document.getElementById('status').textContent})""")
            assert f"{report['cold']['index']['files']} reused" in report['warm']['status']
            assert not report['errors'],report['errors']
            if args.first_clone_control:page.evaluate('firstCloneWorker.terminate()')
            if args.profile:
                report['file_clone_control']=page.evaluate("""async()=>{
                  const url=URL.createObjectURL(new Blob(['postMessage({type:"ready"}); onmessage=({data})=>postMessage({files:data.files.length});'],{type:'text/javascript'}));
                  const echo=new Worker(url);
                  try{
                    await new Promise((resolve,reject)=>{echo.onmessage=resolve;echo.onerror=reject;});
                    const files=[...sourceFiles].map(([path,file])=>({path,file}));
                    const response=new Promise((resolve,reject)=>{echo.onmessage=resolve;echo.onerror=reject;});
                    const start=performance.now();echo.postMessage({operation:'file-clone-control',files});
                    const synchronousMs=performance.now()-start;const result=(await response).data;
                    return {files:result.files,synchronousMs,roundTripMs:performance.now()-start,wasm:false};
                  }finally{echo.terminate();URL.revokeObjectURL(url);}
                }""")
                report['profile']=save_profile(page,client,args.output)
            report['passed']=True;browser.close()
    finally:
        server.shutdown();server.server_close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({key:value for key,value in report.items() if key!='profile'},indent=2))
    if args.profile:print(json.dumps({'profile_tasks':[{key:task[key] for key in ('startMs','durationMs')} for task in report['profile']['mainThreadTasksOver50Ms']]},indent=2))


if __name__=='__main__':main()

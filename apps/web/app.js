'use strict';
const $ = id => document.getElementById(id);
const skipped = new Set(['node_modules', 'dist', 'build', 'coverage', 'vendor',
  '__pycache__', 'venv', 'target', 'DerivedData', 'bower_components', 'bin', 'obj']);
const MAX_FILES = 20000;
let worker, sequence = 0, revision = 0, busy = false, indexed = false, directory, selected, currentFiles = [];
let pending = new Map();
let indexing = false, projectGeneration = 0, indexRefreshPending = false, refreshing;
let analysisFiles = new Set(), sourceFiles = new Map();
let indexActivity, indexTimer, readyNotice = '';
let recentProjects = [], currentProjectKey;

async function loadRecentProjects() {
  try { recentProjects = (await LumenCodeStorage.projects()).sort((a, b) => b.opened - a.opened); }
  catch (_) { recentProjects = []; }
  const select = $('recent');
  select.replaceChildren(new Option('Recent folders…', ''));
  for (const project of recentProjects) select.append(new Option(project.name, project.id));
  select.hidden = !recentProjects.length;
  controls();
}
async function rememberDirectory(handle) {
  let id;
  for (const project of recentProjects) {
    try { if (await handle.isSameEntry(project.handle)) { id = project.id; break; } }
    catch (_) { /* Moved or inaccessible recent entry. */ }
  }
  id ||= crypto.randomUUID();
  try {
    await LumenCodeStorage.saveProject({id, name: handle.name, handle, opened: Date.now()});
    await loadRecentProjects();
  } catch (_) { /* File access still works when browser storage is unavailable. */ }
  return `folder:${id}`;
}

function stopIndexProgress() {
  clearInterval(indexTimer); indexTimer = undefined; indexActivity = undefined;
  $('index-details').hidden = true;
}
function renderIndexProgress() {
  if (!indexActivity) return;
  const {phase, path, done, total, started, fileStarted} = indexActivity;
  const labels = {discovering: 'Listing project files', analysing: 'Analysing files',
    saving: 'Saving analysis facts', definitions: 'Collecting definitions',
    declarations: 'Matching declarations', linking: 'Resolving calls',
    routes: 'Matching HTTP routes', complete: 'Finishing relationships'};
  $('index-message').textContent = `${labels[phase] || 'Building relationships'}${total ? ` · ${done} / ${total}` : ''}${path ? ` · ${path.replace(/^\/project\//, '')}` : ''}`;
  const progress = $('index-progress');
  if (total) { progress.max = total; progress.value = done; }
  else progress.removeAttribute('value');
  const now = performance.now();
  const seconds = Math.floor((now - started) / 1000);
  const fileSeconds = Math.floor((now - fileStarted) / 1000);
  $('index-time').textContent = `${seconds}s elapsed${path && fileSeconds >= 5 ? ` · current file ${fileSeconds}s` : ''} · Cancel stops the build`;
}
function noteIndexProgress(data) {
  // Progress messages belong to the one index request running in this worker.
  for (const request of pending.values()) {
    if (request.operation === (data.operation || 'index')) request.touch();
  }
  if (data.operation === 'mount') status(`Checking saved project · ${data.done} / ${data.total}`);
  if (!indexActivity) return;
  const changed = data.path !== indexActivity.path || data.phase !== indexActivity.phase;
  Object.assign(indexActivity, data);
  if (changed) indexActivity.fileStarted = performance.now();
}

function status(text) { if ($('status').textContent !== text) $('status').textContent = text; }
function controls() {
  for (const id of ['open', 'import', 'choose-project']) $(id).disabled = busy;
  $('import').hidden = !('showDirectoryPicker' in window);
  $('project-empty').hidden = Boolean(currentFiles.length);
  $('workspace').setAttribute('aria-busy', String(busy));
  $('recent').disabled = busy;
  $('clear-saved').disabled = busy || indexing;
  $('refresh').disabled = busy || (!directory && !currentFiles.length);
  $('refresh').textContent = directory ? 'Refresh project' : 'Import folder again…';
  $('cancel').disabled = !busy && !indexing;
  $('cancel').hidden = !busy && !indexing;
  $('search').disabled = busy || !currentFiles.length;
  $('search').dataset.indexed = String(indexed);
  Workspace.controls();
  $('search').placeholder = indexed ? 'Search files and symbols' : 'Search files';
  for (const button of $('files').querySelectorAll('button')) button.disabled = busy;
}
function rpc(operation, extra = {}) {
  return new Promise((resolve, reject) => {
    const id = ++sequence;
    const request = {resolve, reject, operation, timeout: undefined, touch() {
      clearTimeout(this.timeout);
      this.timeout = setTimeout(() => {
        const activity = indexActivity;
        const location = activity?.path ? ` at ${activity.path.replace(/^\/project\//, '')}` : '';
        restart(`No engine progress for 120 seconds${location}. Choose or refresh the project to retry.`);
      }, 120000);
    }};
    request.touch();
    pending.set(id, request);
    worker.postMessage({id, operation, ...extra});
  });
}
function startWorker() {
  const instance = worker = new LumenCodeWorker();
  worker.onmessage = ({data}) => {
    if (instance !== worker) return;
    if (data.type === 'ready') {
      if (!pending.size) {
        busy = false;
        status(readyNotice || 'Open a folder to explore your code.');
        readyNotice = '';
        controls();
      }
    } else if (data.type === 'progress') {
      noteIndexProgress(data);
    } else if (data.type === 'fatal') {
      status(`Engine unavailable: ${data.error}`);
      $('cancel').disabled = true;
    } else if (data.id && pending.has(data.id)) {
      const task = pending.get(data.id);
      clearTimeout(task.timeout);
      pending.delete(data.id);
      if (data.error) task.reject(new Error(data.error));
      else task.resolve(data);
    } else if (data.type === 'diagnostic') console.warn(data.message);
  };
  worker.onerror = event => {
    if (instance !== worker) return;
    status(`Engine worker failed: ${event.message}. Reload the app to retry.`);
    for (const task of pending.values()) { clearTimeout(task.timeout); task.reject(new Error(event.message)); }
    pending.clear();
    busy = true;
    controls();
    $('cancel').disabled = false;
  };
}
function restart(message) {
  stopIndexProgress();
  readyNotice = message;
  revision++; projectGeneration++; indexing = false; indexRefreshPending = false;
  analysisFiles.clear(); sourceFiles.clear();
  worker.terminate();
  for (const task of pending.values()) { clearTimeout(task.timeout); task.reject(new Error('Operation cancelled')); }
  pending.clear();
  currentFiles = []; selected = undefined; indexed = false;
  Workspace.reset();
  clearSearch();
  $('files').replaceChildren(); $('file-count').textContent = '';
  $('symbols').replaceChildren(); $('search-results').replaceChildren();
  $('source').textContent = ''; $('result').textContent = '';
  $('selected-name').textContent = 'Your project, locally';
  $('summary').textContent = 'Choose or refresh a project to continue.';
  busy = true; controls(); startWorker(); status(message);
}
async function task(action) {
  if (busy) return;
  const token = revision;
  busy = true; controls();
  try { return await action(); }
  catch (error) { if (token === revision && error.name !== 'AbortError') status(error.message); return false; }
  finally { if (token === revision) { busy = false; controls(); refreshIndexedView(); } }
}
function accepted(path) {
  return !path.split('/').slice(0, -1).some(part => skipped.has(part) || part.startsWith('.'));
}
async function enumerate(handle, prefix = '', output = [], token = revision) {
  for await (const [name, entry] of handle.entries()) {
    if (token !== revision) throw new DOMException('Operation cancelled', 'AbortError');
    const path = prefix + name;
    if (entry.kind === 'directory') {
      if (accepted(`${path}/file`)) await enumerate(entry, `${path}/`, output, token);
    } else if (accepted(path)) {
      if (output.length >= MAX_FILES) throw new Error(`Project exceeds the current limit of ${MAX_FILES} files.`);
      const file = await entry.getFile();
      if (token !== revision) throw new DOMException('Operation cancelled', 'AbortError');
      // WORKERFS retains File handles; it does not eagerly read their bytes.
      output.push({path, file});
    }
  }
  return output;
}
async function mount(files, name, {projectKey, verifyContents = false} = {}) {
  const project = ++projectGeneration;
  indexing = false; indexRefreshPending = false; stopIndexProgress();
  status(`Opening ${files.length} local files…`);
  // New worker clears path/mtime caches as well as the previous graph.
  // Recreate it only for subsequent mounts; no cached source survives a refresh.
  if (currentFiles.length) {
    worker.terminate();
    for (const request of pending.values()) { clearTimeout(request.timeout); request.reject(new Error('Project changed')); }
    pending.clear();
    startWorker();
    busy = true;
  }
  indexed = false; selected = undefined;
  Workspace.reset();
  clearSearch();
  $('search-results').replaceChildren(); $('symbols').replaceChildren();
  $('result').textContent = ''; $('source').textContent = '';
  const reply = await rpc('mount', {files, projectKey, verifyContents});
  analysisFiles = new Set(reply.result.paths);
  sourceFiles = new Map(files.map(({path,file}) => [path,file]));
  currentFiles = [...sourceFiles.keys()].sort().map(path => `/project/${path}`);
  $('project-name').textContent = name;
  $('file-count').textContent = `(${currentFiles.length})`;
  Workspace.files(currentFiles);
  if (currentFiles.length) await analyse((reply.result.paths[0] || currentFiles[0]).slice('/project/'.length));
  if (analysisFiles.size) void buildRelationships(project);
  else status(`${currentFiles.length} files · Ready`);
}
async function readPreview(file, indexedCandidate = false) {
  if (!file) throw new Error('This file is no longer available. Refresh the project.');
  if (file.size > 5 * 1024 * 1024) return {text:'',unavailable:true,message:'Preview unavailable for files larger than 5 MiB.'};
  const sample = new Uint8Array(await file.slice(0,8192).arrayBuffer());
  const utf16 = sample[0] === 255 && sample[1] === 254 ? 'utf-16le' : sample[0] === 254 && sample[1] === 255 ? 'utf-16be' : null;
  if (!indexedCandidate && !utf16 && sample.includes(0)) return {text:'',unavailable:true,message:'Binary file · No text preview.'};
  try {
    return {text:new TextDecoder(utf16 || 'utf-8', {fatal:!indexedCandidate}).decode(await file.arrayBuffer())};
  } catch (_) { return {text:'',unavailable:true,message:'Text encoding is not supported by this preview.'}; }
}
async function analyse(path, focus = {}) {
  const project = projectGeneration, withIndex = indexed;
  status(`Opening ${path}…`);
  const source = await readPreview(sourceFiles.get(path),analysisFiles.has(`/project/${path}`));
  let result, elapsedMs = 0;
  if (!source.unavailable) {
    const reply = await rpc('analyse', {path, indexed:withIndex});
    result = reply.result; elapsedMs = reply.elapsedMs;
    if (!analysisFiles.has(`/project/${path}`) && result.language === 'text') {
      result = {...result,previewOnly:true,summary:'Text preview · Not part of the relationship index'};
    }
  } else {
    result = {path:`/project/${path}`,fileName:path.split('/').at(-1),language:'Text',symbols:[],
      previewOnly:true,previewUnavailable:Boolean(source.unavailable),
      summary:source.message || 'Text preview · Not part of the relationship index'};
  }
  if (project !== projectGeneration) return false;
  selected = path;
  Workspace.show(result, source.text, path, focus);
  if (!withIndex && indexed && !result.previewOnly) indexRefreshPending = true;
  status(result.previewOnly ? result.summary : `${result.language || 'Source'} · ${Math.round(elapsedMs)} ms analysis${withIndex ? ' · relationships included' : ''}`);
  return result;
}
async function refreshIndexedView() {
  if (!indexRefreshPending || busy || !selected || refreshing?.project === projectGeneration) return;
  if (Workspace.previewOnly()) { indexRefreshPending = false; return; }
  indexRefreshPending = false;
  const job = refreshing = {project:projectGeneration,path:selected};
  try {
    const {result} = await rpc('analyse', {path:job.path,indexed:true});
    if (job.project !== projectGeneration) return;
    if (!busy && selected === job.path) Workspace.update(result);
    else indexRefreshPending = true;
  } catch (error) { if (job.project === projectGeneration) status(error.message); }
  finally {
    if (refreshing === job) refreshing = null;
    if (indexRefreshPending && !busy) queueMicrotask(refreshIndexedView);
  }
}
$('open').onclick = () => {
  if (!('showDirectoryPicker' in window)) { $('folder-input').click(); return; }
  return task(async () => {
    // Invoke the picker directly from the user's action to retain activation.
    const handle = await window.showDirectoryPicker({mode: 'read'});
    directory = handle;
    currentProjectKey = await rememberDirectory(handle);
    status('Reading folder entries…');
    await mount(await enumerate(handle), handle.name, {projectKey: currentProjectKey});
  });
};
$('choose-project').onclick = () => $('open').click();
$('recent').onchange = () => task(async () => {
  const project = recentProjects.find(item => item.id === $('recent').value);
  $('recent').value = '';
  if (!project) return;
  const handle = project.handle;
  if (await handle.queryPermission({mode: 'read'}) !== 'granted' &&
      await handle.requestPermission({mode: 'read'}) !== 'granted') throw new Error('Read access was not granted.');
  directory = handle;
  currentProjectKey = await rememberDirectory(handle);
  status('Reading folder entries…');
  await mount(await enumerate(handle), handle.name, {projectKey: currentProjectKey});
});
$('import').onclick = () => $('folder-input').click();
$('folder-input').onchange = () => task(async () => {
  const files = [...$('folder-input').files];
  if (!files.length) return;
  directory = undefined;
  const root = files[0].webkitRelativePath.split('/')[0];
  const entries = files.map(file => ({path: file.webkitRelativePath.slice(root.length + 1), file})).filter(item => accepted(item.path));
  if (entries.length > MAX_FILES) throw new Error(`Project exceeds the current limit of ${MAX_FILES} files.`);
  currentProjectKey = `import:${root}`;
  await mount(entries, root, {projectKey: currentProjectKey, verifyContents: true});
  $('folder-input').value = '';
});
$('refresh').onclick = () => {
  if (!directory) { $('folder-input').click(); return; }
  return task(async () => {
    if (await directory.queryPermission({mode: 'read'}) !== 'granted' &&
        await directory.requestPermission({mode: 'read'}) !== 'granted') throw new Error('Read access was not granted.');
    status('Refreshing folder entries…');
    await mount(await enumerate(directory), directory.name, {projectKey: currentProjectKey});
  });
};
async function buildRelationships(project) {
  indexing = true; controls();
  const now = performance.now();
  indexActivity = {phase: 'discovering', path: '', done: 0, total: 0, started: now, fileStarted: now};
  $('index-details').hidden = false;
  renderIndexProgress(); indexTimer = setInterval(renderIndexProgress, 250);
  try {
    const reply = await rpc('index');
    if (project !== projectGeneration) return;
    indexed = true; indexRefreshPending = true;
    status(`${reply.result.files} indexed files · ${reply.result.reusedFromCache} reused · ${reply.result.crossFileEdges} relationships${reply.cacheStatus === 'unavailable' ? ' · browser could not save the index' : ''}`);
  } catch (error) {
    if (project === projectGeneration) status(`Relationships unavailable: ${error.message}. Refresh the project to retry.`);
  } finally {
    if (project === projectGeneration) {
      indexing = false; stopIndexProgress(); controls(); refreshIndexedView();
    }
  }
}
$('clear-saved').onclick = () => task(async () => {
  await LumenCodeStorage.clear();
  await rpc('forget-cache');
  currentProjectKey = undefined;
  await loadRecentProjects();
  status('Saved indexes and recent folders cleared. The current project remains open.');
});
let searchGeneration = 0, searchDelay;
function clearSearch() {
  searchGeneration++; clearTimeout(searchDelay);
  $('search').value = ''; $('files').hidden = false; $('search-results').hidden = true;
}
async function searchProject() {
  const generation = ++searchGeneration;
  const query = $('search').value.trim();
  const container = $('search-results');
  container.hidden = !query; $('files').hidden = Boolean(query);
  if (!query) { container.replaceChildren(); return; }
  const fileResults = currentFiles.filter(path => path.toLowerCase().includes(query.toLowerCase())).slice(0, 50)
    .map(path => ({name:path.split('/').at(-1), path, kind:'file'}));
  const render = results => {
    if (generation !== searchGeneration) return;
    const focusKey = container.contains(document.activeElement) ? document.activeElement.dataset.focusKey : undefined;
    container.replaceChildren();
    for (const item of results) {
      const path = item.path?.replace(/^\/project\//, '');
      const button = document.createElement('button');
      const title = document.createElement('span'); title.textContent = item.name || item.label || 'Symbol';
      const detail = document.createElement('small'); detail.textContent = `${item.kind || 'symbol'} · ${path || ''}${item.line ? ':' + item.line : ''}`;
      button.append(title, detail);
      button.dataset.focusKey = JSON.stringify([path,item.line,item.kind,item.name,item.label]);
      if (path) button.onclick = event => Workspace.navigate(path, item.line, item.name, true, event.detail === 0 ? (item.kind === 'file' ? 'overview' : 'inspector') : '');
      container.append(button);
    }
    if (focusKey) {
      const replacement = [...container.querySelectorAll('button')].find(button => button.dataset.focusKey === focusKey);
      (replacement || $('search')).focus({preventScroll:true});
    }
    if (!results.length) { const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = 'No matches'; container.append(empty); }
  };
  render(fileResults);
  if (indexed && !busy) {
    try {
      const reply = await rpc('search', {query});
      if (generation === searchGeneration) render([...fileResults, ...(reply.result.results || [])]);
    } catch (error) { if (generation === searchGeneration) status(error.message); }
  }
}
$('search').oninput = () => { searchGeneration++; $('search-results').replaceChildren(); clearTimeout(searchDelay); searchDelay = setTimeout(searchProject, 180); };
$('search').onkeydown = async event => {
  if (event.key === 'Escape') { $('search').value = ''; searchProject(); }
  if (event.key === 'Enter') { clearTimeout(searchDelay); searchProject(); }
  if (event.key === 'ArrowDown') {
    if ($('search').value.trim() && !$('search-results').querySelector('button')) {
      event.preventDefault(); clearTimeout(searchDelay); await searchProject();
      if (document.activeElement !== $('search')) return;
    }
    const target = $('search').value.trim() ? $('search-results').querySelector('button') : $('files').querySelector('[aria-current=true]');
    if (target) { event.preventDefault(); target.focus(); }
  }
};
$('search-results').onkeydown = event => {
  const buttons = [...$('search-results').querySelectorAll('button')];
  const index = buttons.indexOf(event.target.closest('button'));
  if (event.key === 'Escape') { event.preventDefault(); $('search').value = ''; searchProject(); $('search').focus(); }
  else if (index >= 0 && ['ArrowDown','ArrowUp','Home','End'].includes(event.key)) {
    event.preventDefault();
    if (event.key === 'ArrowUp' && index === 0) $('search').focus();
    else buttons[event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length-1 : Math.max(0,Math.min(buttons.length-1,index+(event.key==='ArrowDown'?1:-1)))].focus();
  }
};
Workspace.setup({available: () => !busy, status,
  open: (path, focus) => task(() => analyse(path, focus))});
$('cancel').onclick = () => restart('Cancelled. Open or refresh a project to retry.');
const projectMenu = $('project-menu');
projectMenu.addEventListener('click', event => {
  if (event.target.closest('button')) projectMenu.open = false;
});
$('recent').addEventListener('change', () => { projectMenu.open = false; });
document.addEventListener('pointerdown', event => {
  if (!projectMenu.contains(event.target)) projectMenu.open = false;
});
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && projectMenu.open) {
    projectMenu.open = false; projectMenu.querySelector('summary').focus();
  }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'o') {
    event.preventDefault(); if (!busy) $('open').click();
  }
});

async function prepareOffline() {
  if (!('serviceWorker' in navigator)) { $('offline').textContent = 'Offline access unavailable'; return; }
  try {
    await navigator.serviceWorker.register('sw.js', {scope: './'});
    await navigator.serviceWorker.ready;
    const persistent = await navigator.storage.persist();
    $('offline').textContent = 'Available offline';
    $('offline').title = persistent ? 'Offline files saved with persistent storage.' : 'Offline files saved. This browser may clear storage.';
  } catch (error) { $('offline').textContent = `Offline setup incomplete: ${error.message}`; }
}
busy = true; controls(); startWorker(); prepareOffline(); loadRecentProjects();

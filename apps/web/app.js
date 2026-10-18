'use strict';
const $ = id => document.getElementById(id);
const skipped = new Set(['node_modules', 'dist', 'build', 'coverage', 'vendor',
  '__pycache__', 'venv', 'target', 'DerivedData', 'bower_components', 'bin', 'obj']);
const MAX_FILES = 20000;
let worker, sequence = 0, revision = 0, busy = false, indexed = false, directory, selected, currentFiles = [];
let pending = new Map();
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
  status(`${labels[phase] || 'Building relationships'}${total ? ` · ${done} / ${total}` : ''}${path ? ` · ${path.replace(/^\/project\//, '')}` : ''}`);
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
  for (const id of ['open', 'import', 'demo']) $(id).disabled = busy;
  $('open').hidden = !('showDirectoryPicker' in window);
  $('recent').disabled = busy;
  $('clear-saved').disabled = busy;
  $('refresh').disabled = busy || !directory;
  $('index').disabled = busy || !currentFiles.length;
  $('cancel').disabled = !busy;
  $('search').disabled = busy || !indexed;
  $('search').placeholder = indexed ? 'Name or initial letters' : 'Build relationships first';
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
  const instance = worker = new Worker('worker.js');
  worker.onmessage = ({data}) => {
    if (instance !== worker) return;
    if (data.type === 'ready') {
      if (!pending.size) {
        busy = false;
        status(readyNotice || `Engine ready · ${Math.round(data.initializationMs)} ms initialization`);
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
  revision++;
  worker.terminate();
  for (const task of pending.values()) { clearTimeout(task.timeout); task.reject(new Error('Operation cancelled')); }
  pending.clear();
  currentFiles = []; selected = undefined; indexed = false;
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
  try { await action(); }
  catch (error) { if (token === revision && error.name !== 'AbortError') status(error.message); }
  finally { if (token === revision) { busy = false; controls(); } }
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
      if (output.length >= MAX_FILES) throw new Error(`Project exceeds the prototype limit of ${MAX_FILES} files.`);
      const file = await entry.getFile();
      if (token !== revision) throw new DOMException('Operation cancelled', 'AbortError');
      // WORKERFS retains File handles; it does not eagerly read their bytes.
      output.push({path, file});
    }
  }
  return output;
}
async function mount(files, name, {projectKey, verifyContents = false} = {}) {
  status(`Opening ${files.length} local files…`);
  // New worker clears path/mtime caches as well as the previous graph.
  // Recreate it only for subsequent mounts; no cached source survives a refresh.
  if (currentFiles.length) {
    worker.terminate();
    startWorker();
    busy = true;
  }
  indexed = false; selected = undefined;
  $('search-results').replaceChildren(); $('symbols').replaceChildren();
  $('result').textContent = ''; $('source').textContent = '';
  const reply = await rpc('mount', {files, projectKey, verifyContents});
  currentFiles = reply.result.paths;
  $('project-name').textContent = name;
  $('file-count').textContent = `(${currentFiles.length})`;
  const fragment = document.createDocumentFragment();
  for (const path of currentFiles) {
    const button = document.createElement('button');
    const relative = path.slice('/project/'.length);
    button.textContent = relative;
    button.dataset.path = relative;
    button.onclick = () => task(() => analyse(relative));
    fragment.append(button);
  }
  $('files').replaceChildren(fragment);
  status(`${currentFiles.length} analysable files · ${Math.round(reply.elapsedMs)} ms to mount and list`);
  if (currentFiles.length) await analyse(currentFiles[0].slice('/project/'.length));
  if (reply.result.cacheRestored) await buildRelationships();
}
function renderSymbols(symbols, parent) {
  for (const symbol of symbols || []) {
    const row = document.createElement('div'); row.className = 'symbol';
    const head = document.createElement('div'); head.className = 'symbol-head';
    for (const [name, text] of [['symbol-kind', symbol.kind], ['', symbol.name], ['symbol-line', `L${symbol.line || '?'}`]]) {
      const span = document.createElement('span'); span.className = name; span.textContent = text; head.append(span);
    }
    row.append(head);
    if (symbol.detail) { const detail = document.createElement('div'); detail.className = 'symbol-detail'; detail.textContent = symbol.detail; row.append(detail); }
    for (const [field, label] of [['calls', 'Calls'], ['calledBy', 'Called by']]) {
      if (symbol[field]?.length) {
        const relation = document.createElement('div'); relation.className = 'relations';
        relation.textContent = `${label}: ${symbol[field].map(item => item.name || item.target || item.detail || JSON.stringify(item)).join(', ')}`;
        row.append(relation);
      }
    }
    if (symbol.members?.length) { const members = document.createElement('div'); members.className = 'members'; renderSymbols(symbol.members, members); row.append(members); }
    parent.append(row);
  }
}
async function analyse(path) {
  status(`Analysing ${path}…`);
  const {result, elapsedMs} = await rpc('analyse', {path, indexed});
  const source = await rpc('read', {path});
  selected = path;
  $('selected-name').textContent = path;
  $('summary').textContent = result.summary || `${result.language} analysis`;
  $('result').textContent = JSON.stringify(result, null, 2);
  $('source').textContent = source.result.text;
  $('symbols').replaceChildren(); renderSymbols(result.symbols, $('symbols'));
  for (const button of $('files').querySelectorAll('button')) button.setAttribute('aria-current', String(button.dataset.path === path));
  status(`${result.language || 'Source'} · ${Math.round(elapsedMs)} ms analysis${indexed ? ' · relationships included' : ''}`);
  return result;
}
$('open').onclick = () => task(async () => {
  // Must be called directly from this click to retain user activation.
  directory = await window.showDirectoryPicker({mode: 'read'});
  currentProjectKey = await rememberDirectory(directory);
  status('Reading folder entries…');
  await mount(await enumerate(directory), directory.name, {projectKey: currentProjectKey});
});
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
  if (entries.length > MAX_FILES) throw new Error(`Project exceeds the prototype limit of ${MAX_FILES} files.`);
  currentProjectKey = `import:${root}`;
  await mount(entries, `${root} (imported snapshot)`, {projectKey: currentProjectKey, verifyContents: true});
  $('folder-input').value = '';
});
$('refresh').onclick = () => task(async () => {
  if (await directory.queryPermission({mode: 'read'}) !== 'granted' &&
      await directory.requestPermission({mode: 'read'}) !== 'granted') throw new Error('Read access was not granted.');
  status('Refreshing folder entries…');
  await mount(await enumerate(directory), directory.name, {projectKey: currentProjectKey});
});
$('demo').onclick = () => task(async () => {
  directory = undefined;
  const sources = {
    'greeting.js': 'export function greeting(name) { return `Hello, ${name}`; }\n',
    'main.js': 'import { greeting } from "./greeting.js";\nexport function run() { return greeting("world"); }\n',
    'index.html': '<!doctype html>\n<html><body><button id="greet" onclick="run()">Greet</button><script src="main.js"></script></body></html>\n',
  };
  await mount(Object.entries(sources).map(([path, text]) => ({path, file: new File([text], path, {lastModified: 1700000000000})})), 'Example project');
});
async function buildRelationships() {
  const now = performance.now();
  indexActivity = {phase: 'discovering', path: '', done: 0, total: 0, started: now, fileStarted: now};
  $('index-details').hidden = false;
  renderIndexProgress(); indexTimer = setInterval(renderIndexProgress, 250);
  try {
    const reply = await rpc('index'); indexed = true;
    stopIndexProgress();
    if (selected) await analyse(selected);
    status(`${reply.result.files} files · ${reply.result.reusedFromCache} reused · ${reply.result.crossFileEdges} relationships · ${(reply.elapsedMs / 1000).toFixed(1)}s${reply.cacheStatus === 'unavailable' ? ' · browser could not save the index' : ''}`);
  } finally { stopIndexProgress(); }
}
$('index').onclick = () => task(buildRelationships);
$('clear-saved').onclick = () => task(async () => {
  await LumenCodeStorage.clear();
  await rpc('forget-cache');
  currentProjectKey = undefined;
  await loadRecentProjects();
  status('Saved indexes and recent folders cleared. The current project remains open.');
});
$('search').onkeydown = event => {
  if (event.key === 'Enter') task(async () => {
    const reply = await rpc('search', {query: $('search').value});
    $('search-results').replaceChildren();
    for (const item of reply.result.results || []) {
      const button = document.createElement('button');
      const path = item.path?.replace(/^\/project\//, '');
      button.textContent = `${item.name || item.label || 'Symbol'} · ${path || ''}:${item.line || ''}`;
      if (path) button.onclick = () => task(() => analyse(path));
      $('search-results').append(button);
    }
    status(`${reply.result.results?.length || 0} results`);
  });
};
$('cancel').onclick = () => restart('Cancelled. Restarting engine…');

async function prepareOffline() {
  if (!('serviceWorker' in navigator)) { $('offline').textContent = 'Offline caching unavailable in this browser'; return; }
  try {
    await navigator.serviceWorker.register('sw.js', {scope: './'});
    await navigator.serviceWorker.ready;
    const persistent = await navigator.storage.persist();
    $('offline').textContent = persistent ? 'Ready for offline use · persistent storage granted' : 'Ready for offline use · browser storage may be evicted';
  } catch (error) { $('offline').textContent = `Offline setup incomplete: ${error.message}`; }
}
busy = true; controls(); startWorker(); prepareOffline(); loadRecentProjects();

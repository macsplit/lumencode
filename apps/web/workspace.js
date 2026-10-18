/* Presentation only: all symbols, dependencies and relationships come from the
 * shared engine. Source rendering is paged to bound DOM work on generated files. */
globalThis.Workspace = (() => {
  const get = id => document.getElementById(id);
  const node = (tag, text, className) => {
    const element = document.createElement(tag);
    if (text != null) element.textContent = text;
    if (className) element.className = className;
    return element;
  };
  const relative = path => typeof path === 'string' && path.startsWith('/project/') ? path.slice(9) : '';
  const label = item => item.name || item.label || item.target || item.method && `${item.method} ${item.path}` || item.path || item.text || 'Item';
  let actions, analysis, path = '', sourceLines = [], page = 0, focusLine = 0;
  let selection, paths = new Set(), history = [], historyPosition = -1, sourceText = '';
  const pageSize = 160;
  function activatePane(name) {
    get('workspace').dataset.pane = name;
    for (const tab of document.querySelectorAll('[data-pane][role=tab]')) {
      const active = tab.dataset.pane === name;
      tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1;
    }
  }
  function historyControls() {
    get('back').disabled = !actions?.available() || historyPosition <= 0;
    get('forward').disabled = !actions?.available() || historyPosition >= history.length - 1;
  }
  function remember(item) {
    if (!item) return;
    if (JSON.stringify(history[historyPosition]) === JSON.stringify(item)) return;
    history = history.slice(0, historyPosition + 1); history.push(item);
    if (history.length > 100) history.shift();
    historyPosition = history.length - 1; historyControls();
  }
  function findSymbol(items, line, name) {
    if (!line && !name) return undefined;
    for (const item of items || []) {
      if ((name && item.name === name && (!line || item.line === line)) || (!name && item.line === line)) return item;
      const match = findSymbol(item.members, line, name);
      if (match) return match;
    }
  }
  async function navigate(targetPath, line, name, record = true) {
    if (!actions.available()) return;
    if (!paths.has(targetPath)) { actions.status('This target is outside the selected project.'); return; }
    await actions.open(targetPath, {line, name, record});
  }
  function showSource(line = 0) {
    focusLine = Math.max(0, Math.min(sourceLines.length, Number(line) || 0));
    page = Math.floor(Math.max(0, focusLine - 1) / pageSize);
    renderSource();
    const active = get('source').querySelector('.selected');
    if (active) get('source').scrollTop = Math.max(0, active.offsetTop - get('source').offsetTop - 60);
    else get('source').scrollTop = 0;
  }
  function renderSource() {
    const start = page * pageSize, end = Math.min(sourceLines.length, start + pageSize);
    const fragment = document.createDocumentFragment();
    for (let index = start; index < end; index++) {
      const row = node('span', null, `source-line${index + 1 === focusLine ? ' selected' : ''}`);
      row.dataset.line = index + 1;
      row.append(node('span', String(index + 1), 'line-number'), document.createTextNode(sourceLines[index] + '\n'));
      fragment.append(row);
    }
    get('source').replaceChildren(fragment);
    get('preview-title').textContent = path ? `${path}${focusLine ? ` : ${focusLine}` : ''}` : 'Source preview';
    get('source-position').textContent = sourceLines.length ? `${start + 1}–${end} / ${sourceLines.length}` : '';
    get('source-previous').disabled = page === 0;
    get('source-next').disabled = end >= sourceLines.length;
    get('copy-source').disabled = !path || Boolean(analysis?.previewUnavailable);
  }
  function section(title, parent) {
    const container = node('section', null, 'detail-section');
    container.append(node('h4', title)); parent.append(container); return container;
  }
  function describe(value) {
    if (value == null) return '';
    if (typeof value !== 'object') return String(value);
    if (Array.isArray(value)) return value.map(describe).filter(Boolean).join(', ');
    return Object.entries(value).filter(([,entry]) => entry !== '' && entry != null)
      .map(([key, entry]) => `${key}: ${describe(entry)}`).join(' · ');
  }
  function links(title, items, parent) {
    if (!items?.length) return;
    const container = section(title, parent);
    for (const item of items) {
      const button = node('button', null, 'relation-button');
      button.append(node('span', label(item)));
      const target = relative(item.path) || (item.kind ? relative(item.sourcePath) : '');
      button.append(node('small', [target || item.detail || item.target, item.line ? `L${item.line}` : '', item.confidence].filter(Boolean).join(' · ')));
      button.onclick = () => {
        if (!actions.available()) return;
        if (target && paths.has(target)) navigate(target, item.line, item.name);
        else {
          // Unresolved/external targets remain inspectable, never opened as URLs.
          inspect(item, title, true); activatePane('inspector');
        }
      };
      container.append(button);
    }
  }
  function inspect(item, kind = 'Symbol', record = true) {
    if (!analysis) return;
    selection = {item,kind};
    const parent = get('inspector'); parent.replaceChildren();
    parent.append(node('div', kind === 'File' ? analysis.language || 'File' : item.kind || kind, 'inspector-kind'));
    parent.append(node('h3', kind === 'File' ? analysis.fileName || path : label(item)));
    parent.append(node('p', [item.detail, item.line ? `${path} · line ${item.line}` : path].filter(Boolean).join(' · '), 'inspector-subtitle'));
    if (kind === 'File') {
      const info = section(analysis.previewOnly ? 'Preview' : 'Analysis', parent);
      info.append(node('p', analysis.previewOnly ? analysis.summary : `${analysis.analysisSourceMode || 'analysis'} · ${analysis.analysisConfidence || 'unknown'} confidence`));
      for (const notice of analysis.analysisNotices || []) info.append(node('p', describe(notice)));
      links('Dependencies', analysis.dependencies, parent);
      links('Routes', analysis.routes, parent);
      links('Quick links', analysis.quickLinks, parent);
      links('Related files', analysis.relatedFiles, parent);
      for (const [field, title] of [['cssSummary', 'CSS'], ['packageSummary', 'Package']]) {
        if (Object.keys(analysis[field] || {}).length) {
          const dl = node('dl'); section(title, parent).append(dl);
          for (const [key, value] of Object.entries(analysis[field])) {
            if (value === '' || Array.isArray(value) && !value.length) continue;
            dl.append(node('dt', key.replace(/([A-Z])/g, ' $1')), node('dd', describe(value)));
          }
        }
      }
    } else {
      const parameters = item.parameters || item.signature?.parameters || [];
      const returns = item.returns || (item.signature?.returnType ? [{text:item.signature.returnType}] : []);
      if (parameters.length || returns.length) {
        const signature = `${item.name || label(item)}(${parameters.map(p => p.name + (p.type ? `: ${p.type}` : '')).join(', ')})`;
        parent.append(node('pre', signature, 'signature'));
      }
      if (parameters.length) {
        const dl = node('dl'); section('Parameters', parent).append(dl);
        for (const parameter of parameters) dl.append(node('dt', parameter.name), node('dd', parameter.type || parameter.detail || 'Type not specified'));
      }
      if (returns.length) section('Returns', parent).append(node('p', returns.map(value => value.text || describe(value)).join(', ')));
      links('Members', item.members, parent);
      links('Calls', item.calls, parent);
      links('Called by', item.calledBy, parent);
      links('Declared in', item.declaredIn, parent);
      if (item.definition) links('Definition', [item.definition], parent);
      links('HTTP routes', item.routes, parent);
      if (item.target) section('Target', parent).append(node('p', item.target));
      const destination = relative(item.path);
      if (destination && paths.has(destination)) {
        const open = node('button', 'Open file');
        open.onclick = () => navigate(destination, item.line, item.name);
        section('Location', parent).append(open);
      }
      if (item.snippet) section('Excerpt', parent).append(node('pre', item.snippet, 'signature'));
    }
    for (const button of get('symbols').querySelectorAll('.symbol-button')) button.setAttribute('aria-current', String(button._item === item));
    showSource(item.line || 0);
    if (record) remember({path, line:item.line || 0, name:item.name || '', file:kind === 'File'});
  }
  function renderSymbols(items, parent) {
    for (const item of items || []) {
      const row = node('div', null, 'symbol');
      const button = node('button', null, 'symbol-button'); button._item = item;
      const heading = node('span', null, 'symbol-head');
      heading.append(node('span', item.kind, 'symbol-kind'), node('span', item.name, 'symbol-name'), node('span', `L${item.line || '?'}`, 'symbol-line'));
      button.append(heading);
      if (item.detail) button.append(node('div', item.detail, 'symbol-detail'));
      button.onclick = () => { if (actions.available()) { inspect(item); activatePane('inspector'); } };
      row.append(button);
      if (item.members?.length) { const members = node('div', null, 'members'); renderSymbols(item.members, members); row.append(members); }
      parent.append(row);
    }
  }
  function files(candidates) {
    paths = new Set(candidates.map(relative));
    const root = {folders:new Map(), files:[]};
    for (const filename of paths) {
      const parts = filename.split('/'); let directory = root;
      for (const name of parts.slice(0,-1)) {
        if (!directory.folders.has(name)) directory.folders.set(name, {folders:new Map(), files:[]});
        directory = directory.folders.get(name);
      }
      directory.files.push({path:filename, name:parts.at(-1)});
    }
    const render = (tree, parent) => {
      for (const [name, child] of [...tree.folders].sort(([a],[b]) => a.localeCompare(b))) {
        const details = node('details', null, 'folder'); details.append(node('summary', name));
        const children = node('div', null, 'folder-children'); render(child, children); details.append(children); parent.append(details);
      }
      for (const file of tree.files.sort((a,b) => a.name.localeCompare(b.name))) {
        const button = node('button', file.name, 'file-button');
        button.dataset.path = file.path; button.title = file.path;
        button.onclick = () => navigate(file.path);
        parent.append(button);
      }
    };
    get('files').replaceChildren(); render(root, get('files'));
  }
  function show(result, text, filename, focus = {}) {
    analysis = result; path = filename; sourceText = text; sourceLines = result.previewUnavailable ? [] : text.split(/\r\n|\n|\r/);
    get('selected-name').textContent = filename;
    get('summary').textContent = `${result.summary || result.language || 'Source'} · ${result.language || ''}`;
    get('result').textContent = JSON.stringify(result, null, 2);
    get('symbols').replaceChildren(); renderSymbols(result.symbols, get('symbols'));
    if (!result.symbols?.length) get('symbols').append(node('p', result.previewOnly ? 'This file is available as a text preview; it is not indexed.' : 'No symbols found in this file.', 'empty'));
    const sections = get('file-sections'); sections.replaceChildren();
    for (const [field, title] of [['dependencies','Dependencies'],['routes','Routes'],['quickLinks','Quick links'],['relatedFiles','Related files']]) {
      if (!result[field]?.length) continue;
      const block = node('section', null, 'overview-section'); block.append(node('h4', `${title} · ${result[field].length}`));
      for (const item of result[field]) {
        const button = node('button', label(item), 'overview-item');
        button.append(node('small', item.detail || item.target || `Line ${item.line || '?'}`));
        button.onclick = () => { if (actions.available()) { inspect(item, title); activatePane('inspector'); } };
        block.append(button);
      }
      sections.append(block);
    }
    for (const button of get('files').querySelectorAll('button')) {
      const active = button.dataset.path === filename; button.setAttribute('aria-current', String(active));
      if (active) { let parent = button.parentElement; while (parent && parent !== get('files')) { if (parent.tagName === 'DETAILS') parent.open = true; parent = parent.parentElement; } }
    }
    const notices = result.previewUnavailable ? [result.summary] : result.analysisNotices || [];
    get('source-notice').hidden = !notices.length;
    get('source-notice').textContent = notices.map(describe).join(' · ');
    const symbol = findSymbol(result.symbols, focus.line, focus.name);
    inspect(symbol || (focus.line ? {name:focus.name || 'Source location',line:focus.line} : result), symbol || focus.line ? 'Symbol' : 'File', focus.record !== false);
    get('inspect-file').disabled = false;
    activatePane('overview');
  }
  function update(result) {
    if (!analysis || analysis.previewOnly || result.path !== analysis.path) return;
    const previous = selection;
    const saved = {page,focusLine,pane:get('workspace').dataset.pane,
      source:get('source').scrollTop,sourceLeft:get('source').scrollLeft,
      overview:document.querySelector('.overview-content').scrollTop,
      inspector:document.querySelector('.inspector-content').scrollTop};
    const identity = item => JSON.stringify([item.name,item.line,item.kind,item.target,item.path]);
    const find = items => {
      for (const item of items || []) {
        if (identity(item) === identity(previous?.item || {})) return item;
        const member = find(item.members); if (member) return member;
      }
    };
    const current = previous?.kind === 'File' ? result : find(result.symbols) ||
      find(result.dependencies) || find(result.routes) || find(result.quickLinks) || find(result.relatedFiles) || previous?.item;
    show(result,sourceText,path,{record:false});
    if (current) inspect(current,previous.kind,false);
    page = saved.page; focusLine = saved.focusLine; renderSource();
    get('source').scrollTop = saved.source; get('source').scrollLeft = saved.sourceLeft;
    document.querySelector('.overview-content').scrollTop = saved.overview;
    document.querySelector('.inspector-content').scrollTop = saved.inspector;
    activatePane(saved.pane);
  }
  function reset() {
    analysis = undefined; selection = undefined; path = ''; sourceText = ''; sourceLines = []; page = 0;
    history = []; historyPosition = -1; paths.clear();
    get('inspector').replaceChildren(node('p','Select a file or symbol to see its details.','empty'));
    get('file-sections').replaceChildren(); get('source-notice').hidden = true;
    get('inspect-file').disabled = true; renderSource(); historyControls();
  }
  async function travel(offset) {
    const position = historyPosition + offset;
    if (!actions.available() || position < 0 || position >= history.length) return;
    const destination = history[position];
    // Commit history position only after the destination loads successfully.
    const opened = await actions.open(destination.path, {...destination, record:false});
    if (opened === false) return;
    historyPosition = position; historyControls();
  }
  function setup(configuration) {
    actions = configuration;
    const tabs = [...document.querySelectorAll('[data-pane][role=tab]')];
    for (const [index, tab] of tabs.entries()) {
      tab.onclick = () => activatePane(tab.dataset.pane);
      tab.onkeydown = event => {
        const destination = event.key === 'ArrowRight' ? (index + 1) % tabs.length :
          event.key === 'ArrowLeft' ? (index + tabs.length - 1) % tabs.length :
          event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1;
        if (destination >= 0) { event.preventDefault(); tabs[destination].focus(); activatePane(tabs[destination].dataset.pane); }
      };
    }
    get('inspect-file').onclick = () => { if (analysis && actions.available()) { inspect(analysis,'File'); activatePane('inspector'); } };
    get('back').onclick = () => travel(-1); get('forward').onclick = () => travel(1);
    get('source-previous').onclick = () => { page--; renderSource(); get('source').scrollTop=0; };
    get('source-next').onclick = () => { page++; renderSource(); get('source').scrollTop=0; };
    get('copy-source').onclick = async () => {
      try { await navigator.clipboard.writeText(sourceText); actions.status('Source copied.'); }
      catch (_) { actions.status('Clipboard access was not granted. You can select and copy source text.'); }
    };
    document.addEventListener('keydown', event => {
      if (event.altKey && ['ArrowLeft','ArrowRight'].includes(event.key)) { event.preventDefault(); travel(event.key === 'ArrowLeft' ? -1 : 1); }
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f') { event.preventDefault(); activatePane('project'); get('search').focus(); }
    });
    const saved = (() => { try { return JSON.parse(localStorage.getItem('lumencode-pane-sizes')) || {}; } catch (_) { return {}; } })();
    const dimensions = {};
    for (const [id, variable, min, max, fallback, axis] of [
      ['project-divider','--project-width',160,480,260,'x'],
      ['overview-divider','--overview-width',220,700,360,'x'],
      ['source-divider','--preview-height',100,600,250,'y']]) {
      const divider = get(id);
      const update = value => {
        const limit = axis === 'y' ? Math.min(max, get('workspace').clientHeight - 120) :
          Math.min(max, window.innerWidth - (id === 'project-divider' ? 550 : (dimensions['project-divider'] || 260) + 290));
        const size = Math.round(Math.max(min, Math.min(Math.max(min,limit),value)));
        dimensions[id] = size; document.documentElement.style.setProperty(variable, `${size}px`);
        divider.setAttribute('aria-valuemin',min); divider.setAttribute('aria-valuemax',Math.max(min,limit)); divider.setAttribute('aria-valuenow',size);
      };
      // Let the overview share remaining space until deliberately resized.
      if (Number.isFinite(saved[id])) update(saved[id]);
      else if (id !== 'overview-divider') update(fallback);
      else { divider.setAttribute('aria-valuemin',min); divider.setAttribute('aria-valuemax',max); divider.setAttribute('aria-valuenow',fallback); }
      window.addEventListener('resize', () => {
        if (window.innerWidth > 800 && dimensions[id]) update(dimensions[id]);
      });
      const persist = () => { try { localStorage.setItem('lumencode-pane-sizes',JSON.stringify(dimensions)); } catch (_) { /* Session layout still works. */ } };
      divider.onkeydown = event => {
        const direction = event.key === (axis === 'x' ? 'ArrowRight' : 'ArrowUp') ? 1 : event.key === (axis === 'x' ? 'ArrowLeft' : 'ArrowDown') ? -1 : 0;
        if (direction) {
          event.preventDefault();
          const current = id === 'overview-divider' ? document.querySelector('.overview-pane').clientWidth : dimensions[id] || fallback;
          update(current + direction * 20); persist();
        }
      };
      divider.onpointerdown = event => {
        if (event.button !== 0) return;
        event.preventDefault(); divider.setPointerCapture(event.pointerId);
        const start = axis === 'x' ? event.clientX : event.clientY;
        const initial = id === 'overview-divider' ? document.querySelector('.overview-pane').clientWidth : dimensions[id] || fallback;
        divider.classList.add('dragging'); document.body.classList.add('resizing');
        divider.onpointermove = move => update(initial + (axis === 'x' ? move.clientX-start : start-move.clientY));
        const finish = () => { divider.onpointermove=null; divider.classList.remove('dragging'); document.body.classList.remove('resizing'); persist(); };
        divider.onpointerup = finish; divider.onpointercancel = finish; divider.onlostpointercapture = finish;
      };
    }
  }
  return {setup,files,show,update,reset,previewOnly:() => Boolean(analysis?.previewOnly),controls:historyControls,navigate,activatePane};
})();

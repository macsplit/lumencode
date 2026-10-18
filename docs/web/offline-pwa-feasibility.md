# Offline browser application: feasibility and prototype direction

Assessment date: 18 October 2026.

Status: original feasibility assessment and future direction. Work began on
18 October 2026; see [prototype status and evidence](offline-pwa-status.md) for
implemented functionality and remaining gates. The assessment below records the
proposed approach; it does not commit to a release date.

## Intended experience

Load LumenCode once over HTTPS, download everything needed for offline use, and
optionally install it as a progressive web application (PWA). The user grants
read access to a project folder. Source scanning, analysis, search and navigation
then run entirely on the user's machine, without uploading project contents or
requiring a local server, native helper or subsequent network connection.

The app should show that offline preparation succeeded before promising offline
availability. After preparation, it must be possible to reopen the app offline
and analyse a different, previously unopened folder. Displaying an old index alone
does not satisfy this requirement.

## Feasibility verdict

Feasible enough to justify a bounded technical prototype. Desktop Chrome and Edge
are the recommended initial targets. There is no identified architectural blocker,
but compilation, filesystem integration, parity and resource usage remain unproven.

The existing separation between native interfaces and `LumenCode::Analysis` is a
strong starting point. Recompile the canonical C++ engine to WebAssembly rather
than reproduce language behaviour in JavaScript. Preserve the existing grammars,
AST processing, branch-scoped recovery, heuristic fallbacks, dependency resolution
and cross-file matching rules in shared source.

## Browser capabilities and limits

| Area | Proposed behaviour / limitation |
| --- | --- |
| Local folders | Use `showDirectoryPicker()` with read permission on desktop Chrome/Edge. |
| Other browsers | Firefox/Safari currently lack that picker API. A folder-import fallback may be possible, with reduced reopening and refresh capabilities. |
| Remembering projects | Store directory handles in IndexedDB; check permissions on reopening and offer a user-triggered reconnect action if needed. |
| Offline startup | A service worker caches the app shell, scripts, styles, worker code, WebAssembly and every language asset. |
| Cache survival | Request persistent storage and check the result. Best-effort storage can be evicted under pressure even without an explicit cache wipe. |
| External dependencies | Only granted folders are accessible. Dependencies outside the selected tree need additional explicit access or remain unresolved. |
| File changes | Start with manual refresh and rescanning on return to the app. Do not depend on experimental filesystem observers. |
| Background work | Do not promise continuous indexing after the app is closed or suspended. |

Installed Chrome apps support persistent file permissions after the user grants
access, but handles and permissions must still be treated separately. Permission
revocation, moved folders and removed files need clear recovery paths. Local
permission prompts do not inherently require a network connection.

Browser-private storage (including OPFS) is distinct from access to the user's
ordinary project folder. It is suitable for indexes and settings, not a substitute
for the explicit folder grant. Source snapshots could be optional; the selected
folder should remain authoritative.

## Engine work required

### WebAssembly build

Use Emscripten to compile the shared engine and vendored Tree-sitter grammars.
Retain Qt Core initially to minimize changes to analysis behaviour. The current
build explicitly requires Qt 5.15 Core/Concurrent; select and prove a compatible
Qt/Emscripten toolchain. If Qt 6 is chosen for the web target, make the necessary
compatibility changes in shared source and verify existing native consumers.
Current Qt WebAssembly documentation is not proof that the existing build works
unchanged.

Expose a small callable API for file analysis, facts and project-index operations.
Reuse existing result schemas where practical. The native CLI transport itself
does not need to run in the browser.

### Filesystem adapter

`engine/src/symbolparser.cpp` uses `QFile`, `QFileInfo` and `QDir`, including reads
of neighbouring files, package metadata and directory listings. Supplying only
the selected file's text would lose existing behaviour.

Map the granted project into a virtual filesystem with stable project-relative
paths and the relevant hierarchy. Evaluate Emscripten MEMFS and WORKERFS, or a
small shared filesystem abstraction if necessary. WORKERFS can read supplied
`File`/`Blob` objects in a worker without copying their entire contents into memory;
the directory mapping and refresh semantics still need proving.

Do not eagerly copy every byte of an arbitrary repository into WebAssembly
memory. Use bounded enumeration and reads, retain metadata needed for resolution,
and preserve the engine's selection and exclusion rules. Measure large-project
behaviour before choosing the final storage strategy.

Explicitly test Unicode names, case sensitivity, symlinks, deleted files,
modification times, relative paths and dependencies outside the granted root.
Document genuine browser/host differences rather than hiding them in comparisons.

### Index scheduling and persistence

`engine/src/projectindex.cpp` currently launches helper processes using `QProcess`
and schedules batches through Qt Concurrent. Browsers cannot use that native
process model. Introduce a browser execution adapter while retaining shared fact
extraction, graph construction and matching logic.

Start with one dedicated Web Worker running a single-threaded WebAssembly engine.
Keep filesystem enumeration and processing off the interface thread where
possible. Make progress, cancellation, timeouts and worker restart explicit;
browser workers replace the execution boundary, not the analysis algorithms.

Persist indexes/settings using IndexedDB or browser-private storage, with engine
version/provenance and invalidation metadata. Preserve native cache semantics
where applicable and test changes, deletions and interrupted scans.

Defer shared-memory threading until measurement justifies it. Threaded
WebAssembly requires cross-origin isolation and suitable response headers. A
single-threaded module in a dedicated worker avoids that initial requirement.

## Interface and hosting direction

Build a web-native TypeScript/HTML/CSS interface around the worker API. Preserve
familiar project navigation, symbols, source previews and relationships while
using browser-appropriate controls and accessibility. Framework choice is open;
it should follow the engine prototype rather than drive it.

Host static build artifacts alongside the existing website, with a separately
scoped service worker so the application does not unexpectedly control the
marketing/download pages. No analysis service or source upload endpoint is needed.
Bundle fonts and dependencies locally; do not rely on CDNs or runtime network
fetches. Treat source snippets as untrusted text when rendering them.

Precache every required language module, including assets normally loaded lazily.
Use versioned, coherent cache updates so an interrupted update cannot mix an old
interface with a new engine. Retain a usable offline version until its replacement
is ready. Exact MIME, caching and any future threading-header requirements should
be checked during deployment; no hosting changes are authorized by this proposal.

## Prioritized prototype and decision gates

1. **Prove compilation.** Build the canonical parser and all existing grammars
   into a callable WebAssembly module. Record toolchain versions, compressed and
   uncompressed artifact sizes, initialization time and build reproducibility.
2. **Prove real-folder analysis.** Grant read access to a project and expose its
   hierarchy to the engine. Analyse representative languages and verify imports,
   related files and package metadata, not just isolated symbol extraction.
3. **Prove shared indexing and parity.** Adapt the execution boundary and compare
   complete native/browser results using the existing baseline fixtures, contract
   comparisons, corpus and damaged-source probes. Normalize only legitimate root
   path differences; report filesystem limitations explicitly. Include complete
   project-index and reciprocal relationship comparisons.
4. **Measure responsiveness and resources.** Test small and large projects, cold
   and warm scans, slow/damaged files, memory peaks, cancellation, worker recovery,
   incremental refresh and interaction latency during scanning. Set acceptance
   budgets from measurements rather than assuming native-equivalent speed.
5. **Prove offline lifecycle.** Prepare the app, close the browser, disable the
   network, reopen it, reconnect permission if necessary, and scan a previously
   unopened project. Test missing permission, denied persistence, incomplete
   initial caching and interrupted updates. Distinguish unavoidable storage loss
   from failures of the offline implementation.
6. **Decide whether to build the full interface.** Proceed when parity,
   responsiveness, memory and the Chrome/Edge permission workflow are acceptable.
   Then select UI tooling, implement the main views, add visual/accessibility
   checks and investigate optional Firefox/Safari folder-import support.

The first milestone is a functional technical demonstrator with measured results,
not a fourth production interface. Keep changes in the monorepo and avoid a forked
browser engine or a separate language-support maintenance stream.

## References

Browser support is time-sensitive; recheck these before implementation.

- [Shared engine](../../engine/README.md) and [build target](../../engine/CMakeLists.txt)
- [Native portability evidence](../windows/engine-portability.md)
- [File System Access API](https://developer.chrome.com/docs/capabilities/web-apis/file-system-access)
- [Persistent file permissions](https://developer.chrome.com/blog/persistent-permissions-for-the-file-system-access-api)
- [Directory picker and compatibility](https://developer.mozilla.org/en-US/docs/Web/API/Window/showDirectoryPicker)
- [Storage persistence and eviction](https://developer.mozilla.org/en-US/docs/Web/API/Storage_API/Storage_quotas_and_eviction_criteria)
- [Service workers and offline caching](https://developer.mozilla.org/en-US/docs/Web/API/Service_Worker_API/Using_Service_Workers)
- [Qt for WebAssembly](https://doc.qt.io/qt-6/wasm.html)
- [Emscripten filesystems](https://emscripten.org/docs/api_reference/Filesystem-API.html)
- [Experimental filesystem observation](https://developer.mozilla.org/en-US/docs/Web/API/FileSystemObserver)

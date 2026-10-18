# Offline browser prototype status

18 October 2026. Initial working technical demonstrator; not publicly deployed.

## Implemented

`apps/web/` builds a callable, headless WebAssembly module from the canonical
`engine/src/` and all existing vendored grammars. Qt Core 6.8.3 and Emscripten
3.1.56 are isolated from the native Qt 5 build. Native options remain the default;
`LUMENCODE_BUILD_WEB` explicitly selects the browser target.

The browser worker maps granted/imported `File` objects into read-only WORKERFS,
retains paths and modification times, and invokes the shared parser and project
index. The web-specific adaptation replaces native helper subprocesses and Qt
Concurrent batching with independently scheduled worker analysis; facts, recovery and graph matching
remain canonical. A new worker on refresh clears static path/mtime caches.

The dark interface now follows the desktop four-pane layout: project tree, file
overview, inspector and bottom source preview. It supports folder selection/import,
an example project, symbols and source, project relationships and search. Workspace
presentation remains separate from the engine and worker scheduler. Source text is rendered as text rather than HTML.
Cancellation/timeouts terminate the coordinator and every parser worker. All essential assets are precached,
with a cache version derived from asset hashes and a storage-persistence result
shown to the user.

Derived index facts now persist in IndexedDB, keyed by the compiled engine version,
project identity and a whole-project manifest. Recent granted folder handles also
persist, with permission checked on reopening. Imports require reselection and
verify contents before cache reuse; folder handles use the native-style size/mtime
metadata check. An unchanged project automatically rebuilds its relationships from
saved facts on reopening. Changed project/engine inputs invalidate the snapshot
conservatively, including changed neighbouring files and package metadata. At most
three index snapshots and eight recent folders are retained. Storage failure falls
back to session analysis; a clear-saved-state control is available.

## Evidence and scope

Chromium/Playwright checks run against the actual WebAssembly engine on Kubuntu
([recorded results](evidence/offline-pwa-fixtures.json)):

- Complete JSON parity across all **102 unique baseline fixture files** after
  normalizing only the physical project-root prefix.
- Complete parity for **three selected indexed analyses**, covering JavaScript,
  TypeScript and Swift, using the same fixture tree for native/browser indexing.
- Demo navigation, relationship indexing, search and folder-import control.
- Production folder-handle enumeration and refresh using a real OPFS directory
  handle supplied by a test-only picker. This verifies handle processing, not the
  operating-system picker or its permission prompt.
- Desktop/mobile layout screenshots and no horizontal overflow at tested sizes.
- A main-thread timer continues during worker indexing. Recorded timer gaps and
  per-operation timings are diagnostic fixture measurements, not latency promises.
- Full browser shutdown, origin-server shutdown, same-profile restart offline,
  and analysis of newly generated source that was never cached.
- Network requests observed during the online test are same-origin GET requests;
  analysis requires no source upload endpoint.

Two initial differences were QSet/QHash output ordering across Qt versions in
snippet call relationships and CSS script-applied class lists. Both were corrected
in shared source using deterministic ordering. No browser-only language logic or
comparison allowlist was introduced.

Native Qt 5 helpers compile on Linux and macOS, with **102 analysis contracts and
zero contract issues** on each. These checks do not replace complete native GUI,
Windows or broad corpus acceptance testing.

The engine artifact is approximately **20.4 MB uncompressed / 3.4 MB gzipped**.
`build-report.json` records the exact size, hash and source provenance for each
build. Actual transfer compression depends on hosting configuration.

One Linux Chromium fixture run initialized the worker in **91 ms** and indexed
**122 files in 250 ms**, with zero failed analyses. The main-thread 10 ms timer
had a maximum observed gap of **10.1 ms** during that indexing operation. The
observed WebAssembly linear-memory capacity remained **64 MiB**; this is neither
total browser memory nor a general peak-memory guarantee. Treat these as a
small-fixture diagnostic sample, not a production benchmark.

Durable development artifacts on Kubuntu:

- Build: `/home/user/Code/Coding/lumencode-wasm-build/web/`
- Toolchains: `/home/user/Code/Coding/lumencode-wasm-tools/`
- Native helper: `/home/user/Code/Coding/lumencode-wasm-native-check/bin/lumencode-engine`
- Browser reports/screenshots: `/home/user/Code/Coding/lumencode-validation-20261018-pwa/`
- macOS native contract report: `native-macos-contract.json` in the browser reports directory

## Resume here

### Relationship build follow-up

A user reported an apparently endless relationship build. The exact folder and
browser were not established during this follow-up, so its original cause is
not claimed as reproduced. Inspection identified two concrete problems: no live
progress display, and an unconditional two-minute operation timeout that could
discard an otherwise progressing large scan.

The browser now uses single-file analysis batches and an optional shared-engine
activity callback. The interface shows phase, file/count and elapsed time through
analysis, declaration matching, call resolution and route matching. The watchdog
is reset by actual engine progress; 120 seconds without progress triggers a
visible stalled-worker message. Cancellation also preserves its message after
worker restart. No parsing or relationship heuristics changed.

[Regression evidence](evidence/offline-pwa-progress-fixtures.json) retains full
102-file and three indexed comparisons, offline restart and worker recovery.
Virtual-clock checks verify both progress beyond two minutes and restart after
silence; these checks make no performance claim.

[Larger-project measurements](evidence/offline-pwa-project-progress.json) cover
590 indexed source files from the monorepo, including vendored parser sources:
Chromium completed in 15.1 seconds and Playwright WebKit on Linux in 14.9 seconds,
with zero failed files. Observed WebAssembly memory capacity was about 92 MiB.
The snapshots changed slightly between runs, so this is not a cross-browser
complete-result parity comparison. A separate visible-interface test completed
the same-sized scan in 14.6 seconds and captured the live progress display.
Raw traces/screenshots are under `monorepo-progress/`, `monorepo-webkit/`,
`progress-ui/` and `progress-fixtures/` in the durable browser evidence directory.

The generated Brotli-data recovery case is now tracked in
[GitHub issue #13](https://github.com/macsplit/lumencode/issues/13) and deferred at
the user's request. Longer scans expose the current file/phase instead of
remaining opaque. No Brotli-specific recovery change or exclusion was introduced.

### Persistent-cache milestone

The canonical engine still produces and consumes the facts cache; browser storage
adds durability without a separate language or relationship implementation.
The 102-file complete JSON comparisons and three selected indexed comparisons
continue to pass. The full lifecycle test now also reopens a remembered OPFS folder
and restores its relationships after browser/server shutdown and offline restart.
The native operating-system folder permission dialog remains an interactive check.

Cache-specific checks pass in Chromium and Playwright WebKit: reuse after worker
and page restarts, edits with identical size/mtime on imports, changed neighbours,
added/deleted files, simulated engine-version changes, denied storage, index retention
limits and clearing saved state. Recent-folder handle retention is tested in
Chromium; WebKit coverage targets imports and derived facts. See [Chromium evidence](evidence/offline-pwa-cache-chromium.json),
[WebKit evidence](evidence/offline-pwa-cache-webkit.json) and
[full lifecycle/parity evidence](evidence/offline-pwa-persistence-fixtures.json).

A real 590-file project took **14.9 seconds** for its first relationship build.
After a full browser restart offline, reimport, content verification and automatic
relationship restoration took **2.25 seconds**, with all **590 files reused** and
**1,988 relationships**. The cached index step itself took about **0.3 seconds**.
See [project restoration evidence](evidence/offline-pwa-project-restoration.json).

The earlier comparison on one frozen 590-file snapshot measured native parallel
cold indexing at 5.3 seconds, sequential native facts at 12.1 seconds, and one-worker
browser cold indexing at 14.5 seconds. Both cached rebuilds were about 0.23 seconds.
Preloading browser files into memory changed cold indexing to 14.2 seconds but
added 0.92 seconds of preparation, versus 0.07 seconds for file-backed mounting.
These diagnostic samples support prioritizing scheduling, parallelism and durable
caches; they are not universal WebAssembly overhead or speedup guarantees. Native
and browser runs also use different Qt versions and execution/transport paths.
See the [recorded comparison](evidence/offline-pwa-performance-comparison.json).

### Parallel indexing milestone

Cold file extraction now runs across a bounded pool of independent workers. Every
worker uses the same compiled engine and mounts the complete read-only project so
neighbour/import resolution retains its context. JavaScript schedules files; the
canonical C++ parser produces facts, and the canonical project index consumes them,
constructs relationships and saves the cache. Native helper execution remains the
default when supplied facts are absent. A shared planning function applies exactly
the same cache validity rules before extraction and during the final build.

The automatic pool uses at most four parsers, leaves one reported CPU available,
and starts only for at least 128 stale candidates. Smaller projects and fully
cached builds stay on the coordinator. Larger files start first to reduce the long
tail. The client owns every worker directly: cancellation, restart and parser
failure terminate the entire pool; failed extraction does not publish a partial
index. No shared memory, pthread build or isolation headers are required.

[Complete indexed comparisons](evidence/offline-pwa-parallel-validation.json) pass
for all **122 candidate fixture files** with one, two and four parsers in Chromium
and Playwright WebKit on Linux. Comparisons retain complete arrays and values,
without sorting in the test. They exposed hash-dependent incoming-call and tied
HTTP-route ordering; shared graph traversal and tied route results now use stable
ordering on every platform. Cache reuse after a worker restart, injected parser
failure, whole-pool cancellation and recovery also pass in both engines.
[Lifecycle regression evidence](evidence/offline-pwa-parallel-lifecycle.json)
retains the 102-file native/browser comparisons, three selected native indexed
comparisons, offline restart, saved-folder reopening and progress watchdog checks.
The rebuilt Linux native helper passes all 102 analysis contracts. A visible
590-file interface check selected four workers automatically, finished in about
five seconds, saved its facts and had no horizontal overflow at 1440 × 900.

Sequential measurements on the same frozen 590-file project and host:

| Parser workers | Cold index | Cached rebuild | Aggregate WASM capacity |
| --- | ---: | ---: | ---: |
| 1 | 14.83 s | 0.37 s | 92 MiB |
| 2 | 8.64 s | 0.43 s | 233 MiB |
| 4 | 5.11 s | 0.42 s | 361 MiB |

All runs reported zero failed files, 6,708 definitions, 27,293 call sites and
1,988 cross-file relationships. These single-run Chromium diagnostics show about
**2.9× faster cold indexing**; they are not universal performance promises.
Playwright WebKit on Linux completed the same four-parser scan in **5.79 seconds**
with a **0.62-second** cached rebuild. The main-thread 10 ms heartbeat's largest
observed gap in Chromium was 56 ms in the four-parser
run. Capacity sums include each parser's largest observed heap plus the
coordinator heap; they are not sampled peak RSS or total browser memory. Children
are terminated before final graph construction. See the
[recorded performance comparison](evidence/offline-pwa-parallel-performance.json).

### Desktop-style workspace milestone

The browser now presents the same four main panes as the Qt, SwiftUI and WinUI
applications, with independently scrolling content and draggable dividers. Divider
sizes persist locally; arrow keys resize focused dividers. At narrow widths,
Project / Overview / Inspector / Source tabs keep every pane reachable.

The project tree groups candidate files by folder and unfolds the current file.
File search works before indexing; symbol results still come from the shared engine.
Overview symbols and members select inspector details and focus their source line.
The inspector presents parameters, returns, members, calls/callers, dependencies,
routes, related files, quick links and available CSS/package metadata. Local relation
links navigate between files. Bounded Back/Forward history restores file/symbol
selection; Alt+Left/Right and Ctrl/Cmd+F follow desktop navigation conventions.

The source pane renders at most 160 numbered lines per page, jumps to a selected
line, supports page navigation and copies the full original source. Text nodes
preserve source safely; engine notices remain visible. Raw engine JSON is tucked
under a diagnostic disclosure. These changes do not modify parser/graph behaviour
or the worker-pool policy.

[Workspace interaction evidence](evidence/offline-pwa-workspace.json) covers real
Chromium and Playwright WebKit on Linux: all four panes, pre-index file search,
symbol parameters, cross-file relationships, Back/Forward selection, pointer and
keyboard resizing, reload persistence, source paging/focused lines and literal
HTML rendering, mobile pane access and no horizontal page overflow. Desktop and
mobile screenshots are retained in `workspace-chromium/` and `workspace-webkit/`
in the durable evidence directory. [Lifecycle checks](evidence/offline-pwa-workspace-lifecycle.json)
retain native/browser JSON parity, indexed comparisons, offline restart, saved
folders and cancellation/watchdog recovery. Native Safari and the actual OS folder
permission prompts remain separate acceptance work.

### Automatic project workflow

The prototype toolbar and manual relationship-build action have been removed.
Opening/importing, refreshing and reopening recent projects automatically build
relationships, reusing the same cached facts and worker pool. Progress and Cancel
remain visible during work. Folder opening lives in the header's Project menu and
in the empty project pane, with Ctrl/Cmd+O as a shortcut. The same Open action uses
the import picker where direct folder access is unavailable. Refreshing an imported
snapshot prompts for reimport. Back/Forward now sit in the project-pane heading;
recent folders, example, saved-state controls and diagnostics live in the Project
menu. The installed app name is simply LumenCode.

[Automatic-workflow evidence](evidence/offline-pwa-automatic-workflow.json) covers
Chromium/WebKit workspace interactions, empty-state folder import, menu/shortcut
opening and automatic indexing. [Lifecycle regression evidence](evidence/offline-pwa-automatic-lifecycle.json)
retains native/browser JSON parity, real OPFS-handle open/refresh, saved-folder
reopening offline, progress-aware timeout and cancellation/recovery. OS picker
permission prompts remain a separate interactive check.

### Background browsing and auxiliary files

File navigation, source inspection, project switching and folder refresh remain
available while the relationship index builds. Progress has its own row, separate
from the currently inspected file's status. A lazy inspection worker runs the same
WASM parser independently of the coordinator, including when the coordinator is
occupied by a single-parser build. The inspection module has no graph; completed
relationships come from the coordinator and update the current view in place.
Symbol selection, source page and horizontal/vertical scroll, pane scroll and the
active compact-screen pane are preserved without adding a history entry.

Project generations prevent old background completions from restoring a replaced
project. Switching projects/cancelling clears pending requests and terminates the
coordinator, parser pool and optional inspection worker. Inspection workers are
also released after indexing and outstanding inspections finish. Clearing saved
state remains disabled during indexing to avoid racing a cache write.

The project tree now includes all selected files after the existing directory
exclusions, while canonical candidates alone contribute to relationships. Other
files support read-only UTF-8 and BOM-marked UTF-16 previews up to 5 MiB. Text
inspection still invokes the canonical parser, preserving CSS symbols and
JSON/package metadata even though those files are not index candidates. Unknown
text is presented as a plain preview. Binary,
oversized and unsupported-encoding files show explicit messages. Projects containing
only auxiliary files are browsable. No language extraction, recovery, graph rules
or C++ engine source changed for this milestone.

[Background validation](evidence/offline-pwa-background-browsing.json) covers
Chromium and Playwright WebKit on Linux: inspection while real facts requests are
held at the transport boundary, auxiliary previews, unchanged indexed candidate
count, preserved selection/source/pane position and history, worker release,
project replacement, cancellation and auxiliary-only projects. The test contains
no production timing hooks. The Chromium run also indexed the frozen **590-file**
project with one parser: inspection completed in **168 ms** while the real index
continued for **14.5 seconds**. The optional inspection module's observed capacity
was **64 MiB**, released afterward. These are diagnostic samples; aggregate WASM
capacity includes the optional inspection module and is not peak RSS/browser memory.

The complete indexed 122-file one/two/four-parser comparison still passes, as do
102-file native/browser parity, selected indexed comparisons, offline restart,
remembered folders, progress watchdog and existing pane interactions. See
[lifecycle evidence](evidence/offline-pwa-background-lifecycle.json). Browser test
tooling now lives in the persistent
`/home/user/Code/Coding/lumencode-wasm-tools/browser-venv/` directory on Kubuntu,
so it survives `/tmp` cleanup.

### Next steps

1. Try the actual Open folder permission flow interactively in desktop Chrome
   and Edge; check denial/revocation and refresh after external edits/deletions.
2. Extend complete parity comparisons to representative public repositories,
   damaged source and project-wide indexed results. Investigate path case,
   symlinks and dependencies outside the granted root explicitly.
3. Broaden scheduling measurements on lower-memory hardware and representative
   projects before tuning the pool limit/cutoff further. Preserve shared facts and
   graph logic. Observed WebAssembly capacity is not total browser memory. The
   generated-data issue remains deferred separately.
4. Extend cache validation to native permission reconnection, interrupted updates,
   quota pressure and cache loss. Consider fine-grained invalidation and broader
   native-browser/platform checks. Public-hosted app update UX remains future work.
5. Extend the four-pane workspace with syntax colouring, richer search filters
   and broader keyboard/accessibility acceptance. Packaging,
   public preview and distribution remain separate work; native releases and the
   public website have not been changed.

[Build/run/test instructions](../../apps/web/README.md) ·
[Original feasibility and decision gates](offline-pwa-feasibility.md)

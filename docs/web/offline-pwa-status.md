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
Concurrent batching with in-worker analysis; facts, recovery and graph matching
remain canonical. A new worker on refresh clears static path/mtime caches.

The minimal dark interface supports folder selection/import, an example project,
symbols and source, project relationships and search. No framework was needed to
prove the engine boundary. Source text is rendered as text rather than HTML.
Cancellation/timeouts restart the worker. All essential assets are precached,
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

### Next steps

1. Try the actual Open folder permission flow interactively in desktop Chrome
   and Edge; check denial/revocation and refresh after external edits/deletions.
2. Extend complete parity comparisons to representative public repositories,
   damaged source and project-wide indexed results. Investigate path case,
   symlinks and dependencies outside the granted root explicitly.
3. Improve cold-index scheduling and parallelism while preserving shared facts and
   graph logic. Retain responsiveness and memory measurements; observed WebAssembly
   linear-memory capacity is not peak total browser memory. The generated-data
   issue remains deferred separately.
4. Extend cache validation to native permission reconnection, interrupted updates,
   quota pressure and cache loss. Consider fine-grained invalidation and broader
   native-browser/platform checks. Public-hosted app update UX remains future work.
5. Decide the scope and UI tooling for a fuller browser application once those
   gates are satisfied. Packaging/public preview and distribution remain separate
   work; native releases and the public website have not been changed.

[Build/run/test instructions](../../apps/web/README.md) ·
[Original feasibility and decision gates](offline-pwa-feasibility.md)

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

1. Try the actual Open folder permission flow interactively in desktop Chrome
   and Edge; check denial/revocation and refresh after external edits/deletions.
2. Extend complete parity comparisons to representative public repositories,
   damaged source and project-wide indexed results. Investigate path case,
   symlinks and dependencies outside the granted root explicitly.
3. Measure larger-project enumeration, cold/warm index time, interaction latency
   and memory. The test's observed WebAssembly linear-memory capacity is not peak
   total browser memory. Improve progress and scheduling from those measurements.
4. Add persisted recent-folder handles and indexes with versioned invalidation;
   test permission reconnection, interrupted updates, denied persistence and cache
   loss. Add native-browser/platform checks beyond Linux Chromium.
5. Decide the scope and UI tooling for a fuller browser application once those
   gates are satisfied. Packaging/public preview and distribution remain separate
   work; native releases and the public website have not been changed.

[Build/run/test instructions](../../apps/web/README.md) ·
[Original feasibility and decision gates](offline-pwa-feasibility.md)

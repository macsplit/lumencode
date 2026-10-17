# Engine integration and verification

The [17 October follow-up](follow-up-2026-10-17.md) records the subsequent native GUI fixes, wider parity sweeps and updated priorities. The measurements below are the 16 October integration baseline.

The Swift app and CLI now use the original LumenCode analysis engine for language extraction, AST repair, fallbacks, signatures, project relations and search. All 101 shared fixture cases pass, and single-file output matches the live original CLI on all 99 fixture files. GUI loading and rapid switching are substantially faster than the 12 October baseline.

Implementation date: 16 October 2026. Engine revision: `48cf4db8042d51cfe67c2344e4d7a3824576517f`. The sibling repository was not modified. The [12 October assessment](current-assessment.md) remains the historical baseline.

## Implemented changes

The parser, index, web-link, HTTP-link and query modules are pinned without source edits under `Engine/Upstream`. Existing grammars are reused, with C++ and Go added from the same revision. The provenance manifest verifies 251 source, fixture and validator inputs. A small wrapper dispatches JSON requests; it does not implement language heuristics.

The helper builds with Qt Core and Concurrent, without Widgets or Kirigami. The Xcode build packages its private libraries in `LumenCode.app/Contents/Resources/EngineRuntime`. A relocated copy works from a path with spaces without Homebrew load paths. SwiftPM and the internal CLI use the same runtime built by `tools/build_engine.py`.

The Swift adapter retains the original JSON for CLI output, including signature defaults, return descriptions, symbol provenance, unresolved call facts and web/HTTP details. It adds presentation defaults and stable UI IDs without changing the emitted analysis contract. Package dependency objects and quick-link source/target fields are adapted for the existing views.

The GUI requests local file results immediately while the original project index builds separately. It no longer reparses every neighbouring script on each uncached selection. Subsequent indexed selections use the original binding and ambiguity rules. Active symbol inspection is preserved when indexing finishes; the next selection or Command-R uses refreshed indexed data.

Request cancellation terminates foreground helper work, with a forced-kill fallback and deadlines. Project generation and request identity guard publication. The selected-result cache checks modification time, size and inode. Opening a project clears old inspector/snippet content, and edited files no longer reuse the measured stale result.

The interface now has ranked project search, All/Files/Functions/Types filters, Command-F focus and Enter navigation. Search ranking comes from `AgentQueries`. Symbol inspection shows structured parameters, defaults and returns. Damaged analysis displays the original engine's recovery/confidence notice. Related symbol navigation reopens the file and declaration line. Stylesheet links open their target while inspection snippets retain their source context.

Lazy symbol/member rendering reduces the large-result presentation pause. The code view avoids re-highlighting unchanged text and resetting its scroll position on unrelated updates. File previews use bounded reads and file selection shows a file preview instead of silently choosing the first symbol.

The old C++ bridge is no longer compiled by SwiftPM or Xcode. Earlier Swift resolver/scaffolding files remain outside the GUI/CLI execution path and should not receive new language algorithms.

## Backend verification

| Gate | Result |
| --- | --- |
| Fresh Swift package and real Xcode app builds | Passed |
| Swift integration tests | 8 passed |
| Shared manifest cases | 101 of 101 passed, previously 10 of 101 |
| Evaluated manifest expectations | 625 of 625 passed |
| Expected declarations with matching kind/name | 226 of 226 present, previously 84 of 226 |
| Original analysis-contract validator | No issues across 99 files |
| Live original CLI comparison | No differing single-file payloads across 99 files |
| Relocated runtime | Passed; eight private libraries and no external developer load paths |
| Production app launch | Remained running for the five-second smoke interval; no startup diagnostics |

The live comparison fixes `QT_HASH_SEED=0` for both processes because some upstream relation and web-link arrays iterate QHash data. It compares complete JSON values without dropping fields. Manifest cases additionally exercise indexed cross-file behavior. This establishes parity on the shared fixtures; the pinned public corpus and damage mutation sweep remain broader gates.

Integration tests cover original Swift parameters/defaults/returns, raw payload preservation, stable IDs, reciprocal Swift calls, oversized-file skipping, cancellation/timeouts, malformed helper output, language scanning, ranked search/filtering and quick-link source/target semantics.

## GUI measurements

The audit renders the real app views and view model in an AppKit window. It measures data readiness and samples a 10 ms main-runloop heartbeat, including a further 150 ms presentation observation interval. Screenshots are outside timed intervals.

| Operation | 12 October | 16 October |
| --- | ---: | ---: |
| Open original repository and finish initial selection | 25.53 s | 146 ms |
| Open 500-file JS project | 1.02 s | 140 ms |
| Uncached selection in that project, five samples | 903–908 ms | 76–97 ms, median 95 ms |
| Cached selected-data update, five samples | 0.10–0.43 ms | About 1 ms |
| Sixteen rapid selections, final result | 2.55 s | 118 ms |
| Largest observed heartbeat gap across the final run | 189 ms | 59 ms |

The corrected original-repository entrypoint is `src/main.cpp`; the earlier app selected a JS test fixture and performed a neighbour sweep. These figures compare the actual opening experience, not identical parser workloads. The app's Swift source is now the default entrypoint in its own repository, so the later “select app source” action is a cache/no-op check rather than an independent parse benchmark.

The final GUI run passes all recorded selection, timeout and error checks. It also verifies changed-file freshness, opening a stylesheet link, search-to-signature navigation, file-only search and clearing signature context when inspecting a dependency. The tests use pinned fixtures; the sibling checkout is needed only for its optional repository-opening benchmark.

The additional 16,095-line original parser C++ file was ready in 1.21 seconds, with a 39 ms maximum heartbeat gap. The 59 ms overall gap means the proposed 50 ms responsiveness budget is not yet consistently met.

These are Debug measurements on this Mac with a mixture of warm project caches and background indexing. They do not measure OS mouse/keyboard delivery, input-to-frame latency or release p95 performance.

Visual review covers 16 captures, including [search and signatures](audit-2026-10-16/15-search-signature.png), [recovery notices](audit-2026-10-16/08-broken-swift.png) and [the minimum window](audit-2026-10-16/07-minimum-window.png). Old content is cleared during project loading. HTML now displays the original engine's page structure rather than an empty symbol panel. The existing three-column layout is retained.

## Remaining priorities

1. **Compatible release runtime.** The engine executable targets macOS 14, but the Homebrew dependencies installed on this machine include libraries requiring macOS 27. Minimum versions are recorded, and Swift rejects an incompatible runtime explicitly. Build or obtain compatible dependencies before claiming macOS 14 support; then test the relocated app on that OS. Developer ID signing, notarization, complete release dependency notices and clean-machine validation remain release work. Qt 5's deprecation also needs a planned shared-engine migration, without changing language behavior incidentally.
2. **Wider parity and resilience.** Run the original pinned corpus and damage probe against this integration, including false declarations, ambiguous relations, Unicode, deleted/unreadable files and large mixed projects. Keep the manifest and original contract validator as fast gates for engine updates. Retire the unused parser/resolver scaffolding after those wider checks.
3. **Native GUI regression coverage.** The stress run still emits `Application performed a reentrant operation in its NSTableView delegate`. Deferred selection and resetting table identity did not eliminate it. Investigate the underlying AppKit/SwiftUI update sequence and add native input/accessibility tests. Current model-driven tests do not verify keyboard event delivery, split dragging, native menus or multiple windows.
4. **Presentation completeness.** Add type-link interactions and damaged-line gutter markers, improve large-project search/inspector density, and verify very large individual symbol snippets. Cache freshness is checked on selection or refresh; there is no recursive filesystem watcher or complete dependent-result invalidation yet. Indexed enrichment of an already active symbol currently waits for its next selection/refresh.

## Reproduce

Build prerequisites and source update policy are in [Engine/README.md](../Engine/README.md).

```sh
python3 tools/build_engine.py
swift test --scratch-path /tmp/lumencode-shared-build --disable-sandbox
python3 tools/parity_audit.py --cli /tmp/lumencode-shared-build/debug/lumencode-swift-cli --original Engine/Upstream --output /tmp/parity.json
python3 tools/verify_engine_contract.py --cli /tmp/lumencode-shared-build/debug/lumencode-swift-cli --output /tmp/contracts.json
python3 tools/verify_runtime.py --runtime .build/EngineRuntime --output /tmp/runtime.json
python3 tools/run_gui_audit.py --build /tmp/lumencode-shared-build --output /tmp/gui-audit
/tmp/lumencode-gui-audit/gui-audit /tmp/gui-audit "$PWD"
```

Pass `--reference-cli /path/to/original/lumencode-cli` to the contract tool for a live comparison. The GUI audit exits nonzero for missing/failed functional checks, analysis errors, stale final selections or timeouts. Runtime warnings and performance budgets still need an explicit CI gate.

Raw [fixture results](audit-2026-10-16/parity.json), [contract comparison](audit-2026-10-16/contracts.json), [GUI measurements](audit-2026-10-16/gui.json), [CLI timings](audit-2026-10-16/cli-timings.json), [runtime verification](audit-2026-10-16/runtime.json) and [app launch result](audit-2026-10-16/app-launch.json) retain the evidence.

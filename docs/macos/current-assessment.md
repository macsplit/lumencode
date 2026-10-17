# Swift port assessment and priorities

This is the 12 October baseline. The shared-engine implementation and current measurements are recorded in [Engine integration and verification](engine-integration.md), dated 16 October 2026.

The Swift app has a usable macOS shell and background file analysis, but its understanding of code is substantially behind `../lumencode`. The best route to parity is to reuse the original C++ analysis engine and project index behind a narrow JSON boundary. Continuing to develop the separate Swift-port parser would duplicate work already completed in the original.

Assessment date: 12 October 2026. Swift baseline: `b619b82`. Original baseline: `48cf4db`. Both working trees were clean at the start. This assessment adds audit tools, results and documentation; it does not change application behavior.

## Measured baseline

| Check | Result |
| --- | --- |
| Fresh Swift package Debug build | Passed, including GUI and CLI; compiler conversion warnings remain |
| Real Xcode macOS app Debug build | Passed with code signing disabled |
| Original baseline fixture cases | 10 of 101 pass the audited expectations |
| Expected declarations present with the expected kind and name | 84 of 226 |
| Evaluated manifest assertions | 143 of 521 pass |
| CLI fixture invocations | 99 unique files; no process failures or timeouts |
| Populated signatures in those results | 0 |
| Emitted project index call sites | 0 |
| Sampled vendored parser and runtime files | All 24 are byte-identical to the original |

The fixture results compare the current Swift CLI with the original's checked-in expectations. They are not a live comparison of two executable engines, a percentage of total product parity, or the original's complete regression sweep. Missing declarations prevent some subordinate assertions from being evaluated. Counts and required relation names follow the manifest's minimum/subset semantics; signature layout is adapted between the two schemas. Selection round trips, general contract validation and corpus mutation tests remain additional gates.

The original CLI is not built on this machine, and Qt 5 is not installed under the checked Homebrew prefix. Its existing `LUMENCODE_CLI_ONLY` build avoids Kirigami/QML but currently requires Qt Core, Concurrent and Widgets. No original runtime performance comparison is claimed.

## Backend findings

### The grammars are shared but the analysis engine is separate

The compiled bridge uses the same sampled generated grammars as the original. The handwritten bridge is approximately 3,770 lines, while the original `symbolparser.cpp` is approximately 16,095 lines and has separate project index, web linkage, HTTP linkage and query modules. Line counts describe the architecture; they are not a quality metric.

The highest-value reuse candidates are:

- `src/symbolparser.cpp/.h`: language dispatch, AST extraction, signatures, recovery, fallbacks, provenance, routes and configuration structure.
- `src/projectindex.cpp/.h`: import and receiver binding, ambiguity rules, cross-file relations, cached facts, helper batching and cancellation.
- `src/weblinks.cpp/.h` and `src/httpclients.cpp/.h`: HTML/CSS/JS relationships, WebForms and client-to-backend links.
- `src/agentqueries.cpp/.h`: ranked file and symbol search and query behavior.
- Existing fixture manifest, corpus tools and damage probe: the acceptance suite.

These modules use Qt data types internally; `SymbolParser` also inherits `QObject`. There is no need to translate their algorithms into Swift or `std::regex` to put a SwiftUI interface over them. A helper process can retain those types internally and return JSON. A C ABI can also wrap an extracted engine, with Qt kept behind that boundary. Parser and web-link code visibly depend on Qt Core; the index additionally uses Qt Concurrent. A smaller standalone target still needs a build proof before promising its final dependency set.

### Language coverage is incomplete

| Language family | Swift status relative to the original |
| --- | --- |
| JS/JSX and TS/TSX | AST support exists, with some same-file and simple imported calls; test suites, wrappers, barrels, bindings and indexing lag |
| Swift, Python, Rust, Java, C#, PHP | AST extraction exists, but signatures, recovery, imports, routes and relations differ materially |
| C/C++ | Detected and scanned, but uses shallow heuristic extraction; no compiled C++ grammar or original macro pre-pass |
| Objective-C | Classified as C++; original selector and relationship behavior is absent |
| HTML and CSS | Link/class extraction exists, but HTML declarations are absent in the tested web fixtures and reciprocal web facts lag |
| JSON | Package summary works; general outlines and configuration semantics lag |
| Go, QML, VB.NET, SQL, shell, Kotlin, Ruby, SCSS, LESS | Missing from scanner coverage and/or language dispatch |
| ASP.NET WebForms | Markup extensions are not scanned or understood as HTML/WebForms |

Twenty-five fixture cases select files with an empty detected language and no declarations. Explicit `--path` still allows those files to be analysed, which is why a successful CLI process does not prove GUI language support.

The scanner also omits extensions the bridge already recognizes, including `mjs`, `cjs`, `mts`, `cts` and `htm`. Ignore rules, entrypoint ranking and the three representations of language/file type differ. `ProjectAnalyzer` skips JS/TS entries because the scanner calls them `code`, which its source-type list omits.

### Signatures and indexing are scaffolding

`extractTSSignature` is defined but never invoked. Every symbol serializes a default signature object, so object presence must not count as working signature support. The original carries parameter defaults, return descriptions and per-symbol provenance that the Swift model does not preserve in the same form.

The bridge builds call facts from already-resolved same-file calls, rather than recording unresolved AST call expressions. More decisively, its serializer hard-codes `indexFacts.callSites` and `callEdges` to empty arrays. `ProjectAnalyzer` is not called by either the GUI or the CLI.

The unused Swift resolver also sets the caller name to the callee name, can connect an import to same-named definitions outside its imported file, performs repeated linear edge searches, and loads caches without verifying root, checksums or parser version. Incoming GUI relations filter the selected file's outgoing edges, which would omit external callers even after connecting the current index. Reusing the original resolver avoids rebuilding its visibility and ambiguity rules.

### Recovery and provenance need the original policy

The port has branch-scoped repair code, but does not preserve the original's complete AST-first policy: authoritative clean trees; guarded repairs that retain declarations; fallback restricted to uncovered ranges; macro preprocessing; lost-brace insertion and re-nesting; symbol-level confidence and notices.

A clean parse can be labelled `recovered` because `repairResult.clean` is treated as evidence that repair occurred. Unsupported recovery languages can instead retain an `ast` label despite heuristic extraction. The recovery dispatcher tests `cs`, while detection returns `csharp`. File confidence is numeric in Swift and categorical in the original; symbol provenance and several damage fields are absent. These are contract differences, not merely missing warning badges.

The lost-brace Swift fixture misses `clear` and `add`; Java Spring route fixtures return zero routes where four are required; basic Swift calls are empty. Such examples show why connecting unused fields alone will not establish parity.

## GUI responsiveness

The audit hosts the actual `ContentView` and `WorkspaceViewModel` in an AppKit window, renders SwiftUI, invokes model actions and samples a 10 ms main-runloop timer. Data readiness is measured separately from an additional 150 ms rendering observation interval. Screenshot capture is outside timed intervals.

| Operation | Time until data ready | Largest heartbeat gap |
| --- | ---: | ---: |
| Open the Swift port repository | 145 ms | 28 ms |
| Select the app's 1,141-line Swift source | 247 ms | 138 ms |
| Select the 3,770-line C++ bridge | 738 ms | 43 ms |
| Open the original repository and finish initial selection | 25.53 s | 27 ms |
| Open a generated 500-file JS project | 1.02 s | 27 ms |
| Five uncached selections in that JS project | 903–908 ms | 16–23 ms |
| Five cached selections | 0.10–0.43 ms | 12–38 ms |
| Sixteen rapid file selections, final result | 2.55 s | 189 ms |

The final requested selection was displayed correctly, and none of these actions timed out or reported an analysis error. Reselecting a file after editing it on disk failed the freshness check: the previous cached declarations remained visible.

These are Debug measurements on this Mac, not release benchmarks or input-to-frame latency. Five stress samples are sufficient to expose the repeated-work problem, not to establish a production latency distribution. The final run overlapped the Xcode build at its beginning; a previous GUI run independently measured 25.23 s for the original repository and a 132 ms app-source presentation pause.

`ProjectRelationships.augmentSelectedAnalysis` reparses all candidate script files for every uncached selection and discards its local cache afterwards. The GUI cache stores selected results, so it does not amortize that neighbour analysis. Independent CLI measurements confirm approximately 8.3 ms to dump one small JS file versus 947 ms to select it in the 500-file project, including process startup.

Opening the original chooses a test fixture's `main.js`, rather than `src/main.cpp`, which also triggers project-wide script augmentation. Entrypoint ranking differs from the original.

Cancelling the outer GUI task suppresses stale publication but does not stop its detached parsing task. Rapid switching therefore allows obsolete analysis to keep consuming resources. A background thread keeps the runloop moving, but does not bound time to useful results.

Presentation still has measurable stalls. `CodeSnippetView` rebuilds attributed text and resets its scroll position on every update, including unchanged content. Full-file preview preparation can read and split the entire file on the main actor. Profiling is needed to apportion the measured stalls between view layout, snippet preparation and highlighting. AppKit also emitted an NSTableView reentrant-operation warning during both GUI runs.

An oversized synthetic JS file of 2,325,000 bytes exceeded a three-second CLI deadline and was terminated by the audit. The original has a 2 MiB parser limit, a 512 KiB auxiliary-read limit, minification checks and a crash-isolated helper. Swift's in-process bridge has no equivalent whole-analysis isolation or input-size gate; its recovery loop budget does not bound the complete analysis.

## Visual assessment

The dark palette, monospaced code and consistent cards provide a coherent starting point. Default and minimum-size screenshots render readable content without obvious horizontal clipping. The three-column split is useful, but large card padding, repeated summaries and a substantial lower pane leave relatively few declarations visible at 1080 by 560. Long declaration details wrap into several lines. Retain the broad layout and improve its density.

Observed issues:

- Opening another project shows the previous inspector and snippet alongside the new scanning state. [Screenshot](audit-2026-10-12/04-loading-stale-content.png).
- A file selection can show its first symbol's excerpt, rather than a file overview. The snippet title remains the filename, which obscures the selected context.
- Broken Swift has no recovery badge or damaged-line markers. [Screenshot](audit-2026-10-12/08-broken-swift.png).
- HTML shows “No Symbols” where the original provides meaningful page structure. This is an analysis gap with a visible product consequence.
- The interface has no project search, filtering or indexed type navigation.
- Signatures appear incidentally in declaration text, rather than as navigable structured parameters and returns.
- Some user-facing summaries expose bridge implementation terminology. Replace them with useful file facts.

The screenshots cover app content, not the production toolbar/titlebar. Mouse/keyboard delivery, accessibility, live split dragging, multiple windows, system appearance changes and native menu behavior are not validated by this harness. Those need an XCUITest/accessibility pass against the built `.app`.

Representative views: [Swift inspector](audit-2026-10-12/03-swift-symbol.png), [minimum window](audit-2026-10-12/07-minimum-window.png), [app source](audit-2026-10-12/10-app-source.png). All 13 captures and raw timings are in [the audit directory](audit-2026-10-12/).

## Prioritised implementation plan

### Priority 0 Prove reuse of the original engine

Make the next implementation a build and integration proof. Compile a pinned copy of the original engine on macOS, initially using its existing CLI-only target. Package it as a helper callable by Swift. Once working, extract a narrower target around the parser, index and link modules if that reduces dependencies. Retain Qt internally for this first step.

Keep the Swift native interface and UI models as adapters. Add decoding for the original's signature, provenance and index contracts, including absent fields and stable symbol identity. Put language inference and fallback decisions in the shared engine. Avoid returning success with empty data when a decode fails.

Acceptance: dump and decode representative Swift, TS, Python, C++, Go, VB.NET, SQL and WebForms fixtures from the original engine in the Swift CLI; demonstrate packaged launch, error handling and one GUI selection. Record engine revision, grammar revisions and any adapter changes. This proof determines packaging feasibility before further parser work.

Use a checked-in snapshot or a shared engine dependency with an explicit update procedure; a shipped application must not require `../lumencode` to exist. Long term, let both repositories consume one engine source. Keep UI controllers toolkit-specific. A Qt-free common library is possible later, but translating 16,000 lines while chasing parity is a separate undertaking.

### Priority 1 Bound analysis and fix stale state

Give the selected file priority over indexing. Replace per-selection neighbour sweeps with a project analysis store; expose local results promptly and publish indexed relations as they become available. Use the original's size/minification policy and helper crash boundary, with cancellation that stops or supersedes actual work. Bound worker concurrency.

Reset inspector and snippet state on project changes; key results by project generation, canonical path, file revision and engine revision. Invalidate changed files and affected dependent facts. Avoid re-highlighting unchanged snippets and preserve scrolling. Use profiling to target the presentation stalls. Align scanner extensions, file-type mapping and entrypoint ranking with the engine, while keeping file browsing and expensive indexing policies distinct.

Proposed acceptance budgets after warm-up: cached selection under 50 ms, ordinary local file data under 250 ms, no main-runloop pause above 50 ms during routine browsing, oversized-file skip under 100 ms, and local file results within one second on either repository without waiting for the entire index. These are targets, not current guarantees. Preserve final-selection correctness during rapid switching and show the edited declaration after an on-disk change.

### Priority 2 Make language and recovery parity executable

Run both CLIs on the same 101-case manifest, then the pinned public corpus and damage probe. Reuse the original's validator where possible; extend the Swift adapter rather than relaxing semantic expectations. Normalize only intentional differences such as root paths and generated UI IDs.

Close gaps through the reused engine: language recognition, C++/Go grammars, structural languages, AST signatures, recovery guards, fallbacks, routes, configuration and WebForms. Include negative assertions for ambiguous links and declarations lost by repair.

Acceptance: all required fixture expectations pass; the original validator's analysis contracts pass after intentional schema adaptation; damage retention and false declarations match the pinned engine on shared inputs. Document intentional product differences separately. The current audit is the starting baseline, not a substitute for these broader gates.

### Priority 3 Connect the original project index and links

Reuse `ProjectIndex` and the web/HTTP link modules. Consume per-symbol and module call sites, import bindings, receiver/package evidence and reciprocal relations. Expose incoming and outgoing edges with symbol identity, confidence and evidence. Follow the original batching, cache invalidation and ambiguity suppression behavior instead of completing a second name-only resolver.

Acceptance: cross-file fixtures work in both directions across the supported languages; imported aliases and same-named definitions resolve correctly; C++ prototypes and definitions connect; browser calls navigate to Express/Flask routes; ambiguous calls stay unresolved. Incremental updates refresh affected edges without a project-wide parse on every click.

### Priority 4 Present the shared data clearly

Show structured signatures, type links, source/confidence badges, damaged-line notices and broken-link states. Add ranked file/symbol search and filters using the shared query engine. Make relation navigation select the exact symbol in its target file. Clarify file versus symbol snippets and implement diagnostics according to their payload semantics.

Reduce duplicate cards and padding, improve the symbol-list density, and make sparse inspectors useful. Keep responsive split panes and check long paths/declarations at the minimum window size. Separate cached/indexing/loading states so a user can tell what is ready.

Acceptance: screenshot review across normal, empty, damaged, large and multilingual projects; real GUI tests for keyboard search, folder navigation, member/dependency/route selection, relation clicks, resizing and scroll preservation. Gate SwiftUI/AppKit runtime warnings, including the observed table reentrancy warning.

### Priority 5 Keep future updates inexpensive

Run shared contract fixtures on engine updates, corpus/damage tests on parser changes, and GUI performance/screenshot checks on presentation changes. Record release-mode p50/p95 timings over a repeatable workload and test the packaged app on a clean machine. Include non-ASCII paths and identifiers, inaccessible files, deleted files, mixed-language roots and rapid project replacement.

Acceptance: one documented engine update advances both applications' analysis behavior; no manually maintained duplicate parser/index algorithms are needed for normal updates.

## Reproducing the audit

From the Swift repository:

```sh
swift build --scratch-path /tmp/lumencode-audit-build --disable-sandbox
python3 tools/parity_audit.py --cli /tmp/lumencode-audit-build/debug/lumencode-swift-cli --output docs/audit-2026-10-12/parity.json
python3 tools/backend_benchmark.py --cli /tmp/lumencode-audit-build/debug/lumencode-swift-cli --output docs/audit-2026-10-12/cli-timings.json
python3 tools/run_gui_audit.py
/tmp/lumencode-gui-audit/gui-audit "$PWD/docs/audit-2026-10-12" "$PWD"
xcodebuild -project lumencode-swift.xcodeproj -scheme lumencode-swift -configuration Debug -derivedDataPath /tmp/lumencode-audit-xcode CODE_SIGNING_ALLOWED=NO build
```

The parity tool deliberately exits 1 while gaps remain. GUI launch requires a desktop session. The GUI preparation tool generates 500 tiny JS files in a dedicated temporary directory and compiles the unmodified application views with a separate audit entrypoint; it does not add test hooks to the shipped app. The freshness test edits and restores only a generated temporary file. The benchmark terminates its oversized-file process after three seconds. Raw [fixture results](audit-2026-10-12/parity.json), [GUI timings](audit-2026-10-12/gui.json), [CLI timings](audit-2026-10-12/cli-timings.json) and [vendor comparison](audit-2026-10-12/vendor.json) retain the detailed evidence.

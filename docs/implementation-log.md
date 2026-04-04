# Implementation Log

## 2026-04-04

- **Parser Authority / Recovery Refactor (Phase 1):**
    - Added parser-owned provenance fields at both file and symbol/item level, including `analysisSourceMode`, `analysisConfidence`, `analysisHasAstErrors`, `analysisPartial`, `analysisNotices`, plus per-item `sourceMode` and `confidence`.
    - Changed provenance annotation so mixed analyses preserve item-level authority instead of flattening everything into one fake source mode.
    - Implemented the first deliberate partial-AST recovery path for TS/TSX:
      - keep partial Tree-sitter output even when the tree contains errors
      - supplement it with heuristic recovery instead of replacing the whole file analysis
      - return `analysisSourceMode: recovered` with a warning notice and partial-analysis flag
    - Added a checked-in broken TypeScript fixture to lock down that recovered-analysis contract through the CLI regression harness.
    - Extended `tools/regression_sweep.py` so fixtures can assert `analysisSourceMode`, `analysisPartial`, and analysis notices, not just symbol presence.
    - Extended the same recovered-analysis model to Python and Java, including checked-in broken-code fixtures for both languages.
    - Extended the same recovered-analysis model to C#, including a checked-in broken-code fixture for the current recovery contract.
    - Extended the same recovered-analysis model to Rust and PHP, including checked-in broken-code fixtures for both languages.
    - Added post-merge normalization for recovered analyses so merged symbol trees and relation targets are rewritten against the surviving canonical symbol set, reducing obvious AST/heuristic duplicates and stale reverse-edge targets.
    - Left the broader authority refactor intentionally incomplete: Swift and CSS still remain outside the deliberate recovered-analysis model, and heuristics are still merged at file scope rather than constrained to AST error ranges.

- **Callable Signature Contract Refactor:**
    - Added `parameters` and `returns` to callable symbol payloads as backend-owned fields instead of controller-only UI enrichment.
    - Moved callable signature extraction into `SymbolParser`, so CLI dumps, async GUI analysis, direct symbol selection, and relation rehydration all see the same signature data.
    - Preserved declared parameter types and return types for typed languages where the signature can be derived, while keeping fallback inferred `return ...` expression summaries for untyped cases.
    - Fixed typed C-style signatures so return types are no longer replaced by sampled return expressions, and parameter types now preserve pointer/reference markers.
    - Guarded obvious malformed callable snippets such as constructor initializer-list fragments so they no longer emit nonsense return values.
    - Left the current parser-layer signature logic explicitly transitional: the architecture is now correct, but several languages still derive signatures from snippets/signature heads inside the parser rather than directly from grammar nodes.

- **QML Support:**
    - Added first-class `.qml` file discovery in the filesystem crawler and regression harness.
    - Added pragmatic QML language detection and heuristic parsing for imports, root/inline components, properties, signals, functions, and common `on...` handlers.
    - Wired QML through the existing lower-pane snippet language and keyword-highlighting path so it behaves like a supported language in the current UI.
    - Added a checked-in `qml_basic` fixture and validated it through `lumencode-cli` and the fixture sweep.

- **Cross-File Relation Parity / Web Work:**
    - Improved JS/TS/CommonJS cross-file relation augmentation so local alias bindings are respected instead of relying only on raw name matching.
    - Added binding metadata for named imports, aliased imports, and destructured `require(...)` imports in script dependency payloads.
    - Extended the project-side relationship augmentation to use those bindings when deriving cross-file `Calls` / `Called By` links.
    - Added checked-in alias fixtures for TS module imports and CommonJS destructuring aliases.
    - Added reciprocal web-asset inspector behavior:
      - CSS files now show inbound HTML consumer links.
      - CSS files now show HTML-side matched/missing class usage from linked nearby HTML files.
      - Local script files now show inbound HTML consumer links when linked via `<script src=...>`.
    - Updated the right-pane CSS class navigation so entries can point to HTML or CSS appropriately instead of assuming every class entry is CSS-backed.
    - Added checked-in `html_script` fixture coverage and extended `html_css` fixture expectations so this web reciprocity model is regression-tested.

- **Robustness / Overload Hardening:**
    - Added controller-side budgets for project relationship augmentation, including a time budget and limits on imported and incoming file analyses.
    - Added explicit `analysisNotices` and partial-analysis summaries so bounded work now surfaces as visible warnings instead of silent sparse output.
    - Normalized helper failure cases such as missing helper, helper timeout, helper failure, invalid helper output, and oversized-file refusal into the same surfaced notice model.
    - Moved GUI file and cross-file relation analysis onto an asynchronous background path using `QtConcurrent`, with stale-result protection so rapid navigation does not apply obsolete completions.
    - Added overview-pane loading affordances so users see when analysis is in progress instead of assuming empty or half-filled panes are bugs.
    - Confirmed the fixture baseline still passes after the async hardening and bounded-analysis changes.
    - Added a low-value relationship gate in `ProjectController` so variable-only script files no longer trigger the expensive incoming relationship scan just because they are script-like.
    - This specifically reduces misleading timeout/partial warnings on setup-style files that are unlikely to expose meaningful imported symbol relationships.
    - Added parser-side minified/bundled asset detection for script-like files and CSS, so obvious `.min.*` or newline-starved vendor assets are skipped with explicit warning summaries instead of being explored like first-party source.

- **Relation Navigation / Backend Work:**
    - Fixed relation click handling in `ProjectController` so selecting `Calls` / `Called By` entries rehydrates into the actual destination symbol instead of leaving the inspector on a thin edge payload.
    - This restored reciprocal relation visibility in the common case where the destination symbol already has its own relation data, especially when navigating through Swift files.
    - Tightened relationship target selection so callable declarations are preferred over weaker export/property shadows when multiple symbols share a name.
- **More Relation Parity:**
    - Added same-file relation coverage for Python, Rust, Java, and C# to the checked-in fixture suite instead of only asserting symbol presence for those languages.
    - Added a proper AST walk for Python call relations after real docstring-heavy files proved that bounded snippet-based relation detection was too weak.
    - Changed the snippet-based relation fallback to merge with existing AST-derived edges instead of overwriting them, which fixed missing reverse links during relation round-trips on real class methods.
- **JavaScript Stability Follow-up:**
    - Investigated restoring plain JS/JSX to the native Tree-sitter path.
    - Confirmed there are still stability problems on real-world JS files in that path, so plain JS/JSX were left on the heuristic parser for now.
    - Added same-file call-relation extraction to the heuristic JS/JSX parser so it participates in the same `Calls` / `Called By` contract without regressing crash behavior.
- **Fixture / Regression Work:**
    - Added a checked-in baseline fixture suite under `tests/fixtures/baseline/` with a manifest-driven set of compact structural test projects spanning JS, TS, PHP, Swift, Python, Rust, Java, C#, C++, Objective-C, HTML/CSS, and `package.json`.
    - Extended `tools/regression_sweep.py` with `--fixture-manifest` and `--fixtures-only`.
    - Added relation round-trip checks to the regression harness by driving `selectSymbolByData` through the interactive CLI and verifying reverse edges on the selected destination symbol.
    - Fixed the fixture harness so it uses `--dump-file` as the authoritative file-analysis source and reserves interactive CLI state for controller-backed behaviors such as cross-file augmentation and relation round-trips.
    - Revalidated the current backend with both fixture-only runs and wider local-corpus sweeps under `/home/user/Code`, ending this session with `issues_found: 0` on both passes.
- **Range-Aware Recovery Follow-up:**
    - Started a first parser-wide attempt at AST error-range harvesting as groundwork for range-aware recovery.
    - Backed that attempt out in the same session after it proved unstable across several grammars.
    - Kept the repository on the known-good file-level recovered-analysis behavior and recorded range-aware recovery as the next step, but only via narrower language-specific work and fixture-gated rollout.
- **Known Remaining Gap:**
    - Relation traversal is substantially better, but overall relation completeness still needs work across languages and cross-file shapes.
    - Swift functions still often leave the right inspector feeling sparse; useful additional symbol detail should be added later once backend payloads are more trustworthy.
    - The next iteration should continue the authority/recovery refactor into the remaining Tree-sitter languages beyond C#, while continuing to tighten recovered merge quality where broken-code fixtures expose residual duplication.
    - Surfaced warnings are expected now under bounded degradation, but each one should still be treated as a lead for future optimization or parser/integration investigation rather than dismissed as inevitable.

## 2026-04-03

- **Repository Repair:**
    - Replaced broken parser gitlinks in `third_party/` with fully tracked vendored source trees so fresh clones configure and build correctly.
    - Added bundled Tree-sitter Swift sources to the repository.
- **Parser / Backend Work:**
    - Added Tree-sitter-backed Swift parsing, including top-level symbols, nested members, and bounded snippets.
    - Added intra-file `Calls` / `Called By` extraction for Swift and PHP.
    - Extended project-side relationship augmentation so the detail pane can surface more useful caller/callee context.
    - Updated filesystem scanning to include Swift files and ignore common Swift build artifacts such as `.build`, `.swiftpm`, and `DerivedData`.
- **CLI / Regression Work:**
    - Updated `tools/regression_sweep.py` to use the current repository path dynamically instead of a stale hard-coded checkout.
    - Expanded the sweep to include Swift files and lightweight relation-presence checks.
    - Re-validated the backend against real local Swift and PHP files through `lumencode-cli --dump-file`.
- **Explorer UI Lockdown:**
    - Made the right detail pane permanently visible as the stable inspector surface.
    - Added clickable `Calls` / `Called By` sections in the detail pane.
    - Reworked the center-pane interaction model so top-level symbols are full-card click targets and nested members are individually hoverable/clickable rows.
    - Restored spacing between outer symbol blocks while keeping nested rows visually distinct and denser than the prior card-in-card layout.
- **Known Remaining Gap:**
    - Relationship traversal is improved but not finished; navigating through `Called By` entries does not always produce the corresponding reciprocal `Calls` view yet.

## 2026-03-29

- Checked local environment constraints
- Found working Qt 5.15 and Kirigami usage in `../it-tools-kirigami`
- Chose a first implementation based on Qt5/KF5 Kirigami
- Added `README.md` and `docs/spec.md`
- Vendored official Tree-sitter sources into `third_party/`
- Added `THIRD_PARTY_NOTICES.md` with source provenance and license notes
- Implemented:
  - `FileSystemModel` as a custom `QAbstractItemModel`
  - `ProjectController` as the QML bridge
  - `SymbolParser` with bundled Tree-sitter-backed parsing for PHP, JS/TS/TSX, and CSS plus heuristic support where retained
  - `Main.qml` with a three-column Kirigami explorer
- Extended the explorer for Node/CommonJS service repos:
  - dependency extraction for `require(...)` and `import`
  - export-aware overview for `module.exports` and `exports.*`
  - Express route detection
  - related file/test linking
  - `package.json` parsing and display
- Refined navigation flow:
  - removed the side-by-side drawer/path entry pattern
  - added a dedicated startup path selection screen
  - added a folder picker dialog
  - added a back action from explorer to picker
- Verified configuration and build with CMake
- Performed an offscreen runtime smoke test
- Observed a non-fatal Kirigami platform plugin warning during offscreen launch on this machine

## 2026-03-30

- **CLI Enhancements:**
    - Introduced `lumencode-cli`, a standalone executable mirroring GUI functionality.
    - Implemented one-shot and persistent interactive modes (`-i`) with JSON command support.
    - Added support for relative path resolution in the CLI.
    - Integrated `projectSummary` into CLI output, providing file type counts and main entry point detection.
- **Parser Improvements:**
    - **Tree-sitter Integration:** Significantly upgraded parsing for JavaScript, TypeScript, PHP, and CSS using Tree-sitter for enhanced accuracy.
    - **Export Handling:** Improved detection and surfacing of exported symbols (CommonJS and ES modules).
    - **Dependency and Route Extraction:** Enhanced Tree-sitter based extraction for imports, exports, `require`, and basic Express route detection.
    - **Symbol Details:** Added source snippets to symbols for better context. Marked symbols as 'exported' where applicable.
    - **Data Contract Hardening:** Ensured `ProjectController::selectPath` uses a skeleton for stable data contracts, preventing QML errors.
    - **Code Cleanup:** Removed unused lambdas (`addVariableDeclarator`, `symbolExists`).
- **Documentation Updates:**
    - Updated `README.md` to reflect current capabilities and roadmap progress.
    - Updated `docs/cli-testing-strategy.md` to detail CLI features like snippets and Tree-sitter usage.
    - Updated `docs/spec.md` for enhanced parsing, symbol details, and project summary.
    - Added entries to this log reflecting recent work.
- **Web-Stack Validation & Follow-up Fixes:**
    - Validated parser behavior against local PHP, Node.js, TypeScript, React/TSX, HTML, and CSS projects under `/home/user/Code`.
    - Fixed CLI interactive mode so relative `selectPath` and `toggleExpanded` commands resolve against the active root.
    - Added explicit handling for the `getProjectSummary` interactive command.
    - Corrected TypeScript language detection so `.ts` files use the TypeScript Tree-sitter grammar instead of the JavaScript fallback.
    - Improved project summary entrypoint selection to prefer root/package-driven app entry files over arbitrary deep `index.*` files.
    - Fixed Express `app.use(...)` route labels so non-string middleware arguments are preserved instead of being truncated.
- **GUI Source Pane Investigation:**
    - Confirmed that symbol snippets are generated in `SymbolParser` and present in the data payload.
    - Confirmed that the GUI does **not** yet render those snippets anywhere.
    - Confirmed that the intended lower source pane has **not** yet been implemented.
    - Confirmed that no syntax-highlighting dependency is currently linked in `CMakeLists.txt`.
    - Confirmed that no lint or parser-diagnostic payload is currently exposed through `ProjectController`.

- **Stability, Fallback, and Regression Work:**
    - Preserved explorer scroll position across left-tree redraws and fixed visible child indentation after folder expansion.
    - Updated the overview pane so PHP class members render as nested cards instead of disappearing into the detail pane.
    - Auto-collapsed the right detail pane when the selection has no unique context.
    - Added bounded filesystem crawl safeguards:
      - max tree depth `64`
      - max scanned nodes `50000`
      - max included entries per directory `2000`
      - synthetic `[skipped: ...]` children when limits are hit
    - Added parser-side graceful degradation for large inputs:
      - large files are summarized instead of fully parsed
      - lower-pane file previews are truncated instead of unbounded
      - auxiliary reads such as CSS/HTML support paths are capped
    - Moved GUI file analysis onto a crash-isolated helper path by routing selection through `lumencode-cli --dump-file`.
    - Introduced an explicit lower-pane snippet contract using `snippetKind` and `diagnosticsMode`, replacing ad hoc assumptions about whether snippets are parseable.
    - Marked dependency, route, fallback-symbol, and file-preview payloads with the new snippet contract so diagnostics are only attempted on standalone constructs.
    - Hardened JS fallback parsing to:
      - bypass unstable native parsing for plain JS/JSX where needed
      - emit snippets for variables/functions/classes
      - anchor declaration snippets cleanly
      - avoid treating control-flow keywords as object/class members
    - Tightened fallback snippet extraction in C++, Java, and C# so declarations no longer inherit leading braces or access labels.
    - Anchored PHP class regexes to real declarations so comments do not create fake classes.
    - Added `tools/regression_sweep.py`, a corpus-based CLI harness that samples files under `/home/user/Code`, drives `selectPath`/`selectSymbolByData`, and validates snippet and diagnostics invariants.
    - Used the sweep harness to reproduce and close regressions across:
      - JS fallback snippets with leading `}`
      - false parser warnings on excerpt snippets
      - C# and C++ declaration previews with leaked surrounding context
      - PHP false-positive class detection from comments
      - JS object/member extraction producing fake `if` / `for` members

## 2026-03-31

- **Explorer UI Refactor:**
    - Replaced the fixed three-column-only explorer with resizable split views.
    - Added an embedded lower source pane.
    - Removed the top toolbar/header from the explorer view.
    - Moved the back action into a thin left control rail for better vertical density.
    - Reduced margins, padding, icon sizes, and row heights throughout the main explorer.
    - Changed the filesystem root label from `.` to `/`.
- **Source Pane State & Rendering:**
    - Added explicit snippet-view state to `ProjectController`.
    - Wired file and symbol selection into a dedicated lower-pane payload.
    - Added internal syntax coloring for snippet display.
    - Added lightweight parser-aware diagnostics for supported languages.
    - Suppressed false diagnostics when snippets are intentionally truncated with `...`.
    - Added file preview snippets when selecting a file directly.
    - Reduced code tab width in the lower pane for better density.
- **HTML/CSS Follow-up Work:**
    - Added structured CSS class summary entries instead of plain class-name strings.
    - Surfaced snippets for CSS class matches and misses in the detail pane.
    - Wired CSS class entries so they can populate the lower source pane.
    - Fixed CSS-file class inspection so Tree-sitter-backed CSS symbols carry rule-level snippets instead of bare selector tokens.
- **Repository Hygiene:**
    - Added a repo-root `.gitignore` containing `build/`.
    - Removed the previously tracked `build/` output tree from git history going forward.

## Next Session Starting Point

- Continue broadening the CLI regression corpus and assertion set.
- Improve snippet highlighting fidelity or adopt a stronger highlighting dependency if one is available locally later.
- Improve diagnostics beyond the current conservative parser-aware checks.
- Re-run GUI verification on a real display session and capture any remaining QML/runtime issues.
- Continue reducing noisy HTML/CSS and Node/CommonJS output on real repositories.
- Start native parser rehabilitation only after working from minimized crash repros through `lumencode-cli --dump-file`, keeping the current helper isolation and fallbacks in place until each language path is proven stable.

## Wrap-Up State

Current build state:

- The project builds both the GUI (`lumencode`) and CLI (`lumencode-cli`) targets.
- The CLI tool is fully functional, supports stateful interaction, and provides enhanced output including project summaries and symbol snippets.
- Parsing accuracy has been significantly improved through Tree-sitter integration.
- The GUI now includes a lower source pane with internal syntax coloring and lightweight diagnostics.
- The repository no longer tracks `build/` outputs.

Known runtime/UI problems:

- some output remains noisy or underwhelming on real projects
- HTML/CSS class comparison can still be noisy on complex HTML documents
- highlighting is intentionally lightweight and not yet language-complete
- diagnostics are conservative and not equivalent to a full linter
- some lower-pane context sources are still not wired
- some languages still rely on heuristic fallback paths for stability on hostile files

Important product-direction notes for next time:

- the app needs a stabilization pass before more ambitious features
- the next major usability improvement should be broader lower-pane context wiring and better diagnostics/highlighting
- Node/CommonJS structure support exists but needs refinement to become genuinely useful
- documentation now reflects the current project direction instead of the original bootstrap brief
- helper-process isolation and the regression sweep now form part of the intended safety architecture, not just temporary debugging tools

## 2026-03-30 (continued)

- **Core/GUI Parity Follow-up:**
    - Added `selectedSnippet` to CLI state output so CLI payloads stay aligned with the GUI lower-pane contract.
    - Added interactive `selectSymbolByData` support in `lumencode-cli` so lower-pane payloads for routes, dependencies, and quick links can be exercised without the GUI.
    - Wired dependency, route, and quick-link selections in QML into the shared snippet-selection path instead of file-only navigation.
- **Broader Language Support:**
    - Expanded filesystem scanning to include Python, C/C++, Java, and C# files.
    - Added heuristic parsing for Python, C/C++, Java, and C# symbols plus import/include metadata.
    - Added conservative Flask/FastAPI and ASP.NET route extraction.
    - Added related-file support for C/C++ headers and Python/C# test/project neighbors.
- **Project Summary Improvements:**
    - Added a conventional-entrypoint pass so obvious roots like `src/main.cpp`, `app.py`, and `Program.cs` are preferred before generic fallback scoring.
- **Additional Language Coverage:**
    - Added Rust scanning and heuristic parsing for `use`, `mod`, `fn`, `struct`, `enum`, `trait`, and `impl`.
    - Added Objective-C / Objective-C++ scanning and heuristic parsing for `#import`, `@implementation`, and per-class method extraction.
    - Vendored official Tree-sitter Rust and Python grammars and routed both languages through AST-backed symbol extraction before heuristic fallback.
    - Rust snippets now come from AST node bounds, which fixes prior bleed between adjacent functions inside `impl` blocks.
    - Vendored official Tree-sitter Java and C# grammars and routed both languages through AST-backed symbol extraction before heuristic fallback.
    - Tightened Java field extraction so initializer values are no longer surfaced as fake members.
    - Switched Java and C# import / using payloads to AST-backed line and snippet capture.
- **Project Summary Follow-up:**
    - Added recursive Android-oriented conventional entry detection so `MainActivity.java` is preferred over tests or assets on Android app roots.
- **Desktop / Utility Actions:**
    - Added a bundled app icon plus desktop launcher installation through `cmake --install`.
    - Expanded the left control rail with open-in-folder, open-in-editor, and settings actions.
    - Added a persisted preferred-editor setting with `%f` substitution and system-default fallback.

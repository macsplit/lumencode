# Implementation Log

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

- Expand lower-pane selection behavior beyond files, symbols, and CSS class entries.
  - dependencies
  - routes
  - quick links
- Improve snippet highlighting fidelity or adopt a stronger highlighting dependency if one is available locally later.
- Improve diagnostics beyond the current conservative parser-aware checks.
- Re-run GUI verification on a real display session and capture any remaining QML/runtime issues.
- Continue reducing noisy HTML/CSS and Node/CommonJS output on real repositories.

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

Important product-direction notes for next time:

- the app needs a stabilization pass before more ambitious features
- the next major usability improvement should be broader lower-pane context wiring and better diagnostics/highlighting
- Node/CommonJS structure support exists but needs refinement to become genuinely useful
- documentation now reflects the current project direction instead of the original bootstrap brief

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

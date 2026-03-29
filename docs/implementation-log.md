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

## Next Session Starting Point

- Bring the documentation back in line with the real UI state before adding more UI claims.
- Add explicit snippet-view state to `ProjectController` so the GUI can select and display:
  - the current symbol snippet
  - dependency/link context
  - route context
- Refactor `Main.qml` from a three-column-only layout into:
  - top explorer/detail area
  - bottom source/snippet pane
- Implement syntax highlighting for the bottom pane.
  - Preferred path: add a proper highlighting dependency if available in the local Qt/KDE stack.
  - Fallback path: add a small internal highlighter for the currently supported web languages.
- Add parser-aware linting/diagnostics for snippets.
  - Minimum viable version: surface Tree-sitter parse-error state and a short diagnostics summary.
  - Later version: add line/column-specific diagnostics where the parser makes that practical.
- Wire selection behavior so clicking a symbol, route, dependency, or quick link updates the bottom pane consistently.
- Re-run GUI verification after the pane exists; the current user-visible problem is expected until that work lands.

## Wrap-Up State

Current build state:

- The project builds both the GUI (`lumencode`) and CLI (`lumencode-cli`) targets.
- The CLI tool is fully functional, supports stateful interaction, and provides enhanced output including project summaries and symbol snippets.
- Parsing accuracy has been significantly improved through Tree-sitter integration.

Known runtime/UI problems:

- repeated QML `TypeError` failures in the detail pane
- undefined/object conversion errors
- some bindings assume sections exist when they do not
- some output remains noisy or underwhelming on real projects
- HTML/CSS class comparison can still be noisy on complex HTML documents
- The UI is functional but underpowered; it does not yet justify itself as a serious structural explorer.
- There is currently no embedded source pane, which limits inspection usefulness significantly.

Important product-direction notes for next time:

- the app needs a stabilization pass before more ambitious features
- the next major usability improvement should be a bottom pane for syntax-highlighted, line-focused source snippets
- Node/CommonJS structure support exists but needs refinement to become genuinely useful
- documentation now reflects the current project direction instead of the original bootstrap brief

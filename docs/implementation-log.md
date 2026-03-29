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

## Wrap-Up State

Current build state:

- the project configures and builds
- the application is still buggy at runtime

Known runtime/UI problems:

- repeated QML `TypeError` failures in the detail pane
- undefined/object conversion errors
- some bindings assume sections exist when they do not
- some output remains noisy or underwhelming on real projects

Important product-direction notes for next time:

- the app needs a stabilization pass before more ambitious features
- the next major usability improvement should be a bottom pane for syntax-highlighted, line-focused source snippets
- Node/CommonJS structure support exists but needs refinement to become genuinely useful
- documentation now reflects the current project direction instead of the original bootstrap brief

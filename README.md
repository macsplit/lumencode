# LumenCode

LumenCode is a structural code explorer for local source trees. It focuses on code shape rather than freeform text editing. The application reads a filesystem tree, extracts symbols from supported source files, and lets the user drill from folders to files to symbols to member detail.

LumenCode is intentionally:

- filesystem-aware
- parser-assisted
- non-editing
- non-Git-aware

## Current State

The project has undergone significant development, enhancing its parsing capabilities and CLI features.

What currently works:

- Browsing a project tree with robust path resolution.
- Opening files into overview/detail panes with syntax-highlighted source snippets.
- Extracting detailed symbols for PHP, JS, TS, TSX, CSS, and HTML using Tree-sitter parsers.
- Improved CommonJS and Node/service-style repo analysis, including:
    - Accurate dependency extraction (`require(...)` and `import`) with file existence checks.
    - Reliable export surfacing for CommonJS (`module.exports`, `exports.*`) and ES modules (`export ...`).
    - Enhanced Express route detection.
    - Linking to related files (tests, implementations).
    - Parsing `package.json` for scripts, entrypoint, and dependency lists.
- A functional, standalone CLI tool (`lumencode-cli`) for automated testing and analysis.
- Project-level summaries including file type counts and main entry point detection.
- Stable data contracts between C++ and QML, preventing runtime errors from missing data sections.

## Build

Typical local build:

```bash
cmake -S . -B build
cmake --build build
```

If KF5 development packages are missing from the CMake search path, CMake will fail during configuration. That is an environment issue, not an application logic issue.

## Roadmap

Phase 1. Stabilization

- Eliminate the recurring QML `undefined`/type errors in the detail pane.
- Harden data contracts between C++ and QML so missing sections never crash bindings.
- Clean up the current Node/CommonJS extraction so it is reliable on real projects. **(Partially completed: Tree-sitter integration for better accuracy)**

Phase 2. Make the structure actually useful

- Improve dependency resolution and local jump links. **(Completed: Tree-sitter based import/export resolution)**
- Improve exported API surfacing for CommonJS and service-style repos. **(Completed: Enhanced export detection)**
- Improve route detection and project-level summaries. **(Completed: Basic Express route detection, Project Summary implemented)**
- Improve HTML/CSS analysis so only actionable matches/mismatches are shown. **(In Progress: Snippets added, further analysis could be refined)**

Phase 3. Add source visibility

- add a bottom pane for syntax-highlighted, linted source snippets. **(Initiated: Snippet generation added to symbol data)**
- sync that pane with the selected file, symbol, route, or dependency
- support jumping to relevant lines for overview and detail items

Phase 4. Improve usability

- better visual hierarchy and denser information display
- search/filter across files and symbols
- clearer project-level overview for package metadata, tests, and API artifacts

## Known Issues

- The QML detail pane still throws runtime errors in some navigation paths, including `TypeError` and `Unable to assign [undefined] to bool`.
- HTML/CSS class comparison can still be noisy on complex HTML documents.
- There is currently no embedded source pane, which makes inspection much less useful than it should be.

Licensing note:

- Vendored parser sources and their retained upstream license files are documented in `THIRD_PARTY_NOTICES.md`

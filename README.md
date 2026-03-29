# LumenCode

LumenCode is a structural code explorer for local source trees.

This repository therefore implements:

- A Kirigami application using the working Qt5/KF5 stack
- A C++ filesystem model that crawls directories and exposes a hierarchical tree to QML
- A C++ symbol parser backed by vendored Tree-sitter sources for supported languages
- A three-column drill-down UI for folders, files, top-level symbols, and member detail
- Lightweight HTML dependency indexing and CSS class cross-highlighting
- Node/CommonJS project structure extraction including dependencies, exported API members, Express routes, related tests/files, and `package.json` metadata

## Current Requirements

- CMake 3.28+
- Qt 5.15
- KF5 Kirigami development packages

Bundled parser dependencies:

- Tree-sitter runtime source is vendored into the repo
- Official Tree-sitter grammars for JavaScript, TypeScript/TSX, PHP, CSS, and HTML are vendored into the repo
- No separate Tree-sitter package install is required

## Current State

The project builds and the core explorer flow exists, but the app is still unstable and incomplete. It should be treated as a prototype, not as a polished daily-use tool.

Current UX flow:

- launch into a dedicated project-folder selection screen
- open the structural explorer only after a folder is chosen
- use the explorer back action to return to the folder selection screen

What currently works:

- browsing a project tree
- opening files into overview/detail panes
- extracting many symbols for PHP, JS, TS, TSX, CSS
- surfacing Node/CommonJS dependencies and likely route metadata
- reading `package.json` and OpenAPI-style JSON structure

What is still weak:

- QML stability in the detail pane
- correctness and usefulness of some extracted structure
- HTML/CSS matching quality on complex real-world pages
- overall presentation depth and usefulness for larger projects
- lack of an embedded source view/snippet pane

## Structure

- `docs/spec.md`: fuller functional and technical specification
- `docs/implementation-log.md`: concise build notes and decisions
- `THIRD_PARTY_NOTICES.md`: vendored dependency provenance and license notices
- `src/`: application source
- `third_party/`: bundled parser runtime and grammar sources

## Build

Typical local build:

```bash
cmake -S . -B build
cmake --build build
```

If KF5 development packages are missing from the CMake search path, CMake will fail during configuration. That is an environment issue, not an application logic issue.

## Roadmap

Phase 1. Stabilize the existing explorer

- eliminate the recurring QML `undefined`/type errors in the detail pane
- harden data contracts between C++ and QML so missing sections never crash bindings
- clean up the current Node/CommonJS extraction so it is reliable on real projects

Phase 2. Make the structure actually useful

- improve dependency resolution and local jump links
- improve exported API surfacing for CommonJS and service-style repos
- improve route detection and project-level summaries
- improve HTML/CSS analysis so only actionable matches/mismatches are shown

Phase 3. Add source visibility

- add a bottom pane for syntax-highlighted, linted source snippets
- sync that pane with the selected file, symbol, route, or dependency
- support jumping to relevant lines for overview and detail items

Phase 4. Improve usability

- better visual hierarchy and denser information display
- search/filter across files and symbols
- clearer project-level overview for package metadata, tests, and API artifacts

## Known Issues

- The QML detail pane still throws runtime errors in some navigation paths, including `TypeError` and `Unable to assign [undefined] to bool`.
- Some extracted structure is still shallow or misleading on real projects.
- The CSS class comparison can still be noisy on complex HTML documents.
- The UI is functional but underpowered; it does not yet justify itself as a serious structural explorer.
- There is currently no embedded source pane, which makes inspection much less useful than it should be.

Licensing note:

- Vendored parser sources and their retained upstream license files are documented in `THIRD_PARTY_NOTICES.md`

# LumenCode Specification

## 1. Product

LumenCode is a structural file explorer for source trees. It focuses on code shape rather than freeform text editing. The application reads a filesystem tree, extracts symbols from supported source files, and lets the user drill from folders to files to symbols to member detail.

LumenCode is intentionally:

- filesystem-aware
- parser-assisted
- non-editing
- non-Git-aware

## 2. Technical Direction

Current target environment:

- Qt 5.15.13
- working Kirigami QML modules in the Qt5 import path
- a known-good Kirigami application in `../it-tools-kirigami`

Implementation decision:

- Use Qt5 + KF5 Kirigami
- Vendor Tree-sitter core and official grammars into the repository
- Avoid external Tree-sitter package dependencies at build time and run time

## 3. Functional Scope

### 3.1 Column View

The main window uses a three-column drill-down layout:

- Column 1: filesystem tree
- Column 2: selected file overview or selected folder summary
- Column 3: selected symbol detail and related links

### 3.2 Filesystem Rules

The crawler must:

- recurse directories without blocking the UI
- ignore `.git`
- ignore Python and C++ source trees by default
- focus on these file types: `js`, `jsx`, `ts`, `tsx`, `php`, `html`, `css`, `json`

### 3.3 Symbol Extraction

Current parser coverage:

PHP:

- classes
- traits
- interfaces
- methods
- properties
- class constants
- top-level constants

JavaScript and TypeScript:

- exported functions
- exported const/let/var declarations
- classes
- methods
- CommonJS `require(...)` dependencies
- `module.exports` and `exports.*` API members
- Express route handlers and middleware-style endpoints where detectable
- related test files by naming convention

React TSX/JSX:

- functional components
- class components
- props interfaces and type aliases
- hook-style functions beginning with `use`

HTML:

- linked scripts
- linked stylesheets
- CSS classes used in markup

CSS:

- class selectors
- custom properties

JSON:

- `package.json` scripts, entrypoint, dependency lists
- OpenAPI-style path and operation summaries where detectable

### 3.4 Neon Link Features

When an HTML file is selected:

- inspect `<script src>` links
- inspect stylesheet links
- resolve sibling and relative paths against the HTML file
- show navigable quick links in the detail pane

When an HTML file and sibling CSS files are available:

- collect CSS class names from the HTML file
- collect CSS class selectors from sibling CSS files
- show matches and missing classes

### 3.5 Node Project Features

When a Node/CommonJS project is explored:

- surface local and package dependencies from `require(...)` and `import ... from ...`
- elevate exported API members from `module.exports` objects into the file overview
- detect Express routes such as `app.get(...)`, `app.post(...)`, and `router.use(...)`
- link source files to likely tests in nearby `tests/` folders
- parse `package.json` scripts, entrypoint, and dependency list

## 4. UX

### 4.1 Visual Style

The UI follows Kirigami conventions with a dark-first palette.

Symbol visual language:

- folder
- file
- class/trait/interface
- method/function/hook
- property/constant

### 4.2 Primary Flow

1. Open app
2. Choose or confirm a root path
3. Browse folders and files in the left column
4. Inspect top-level symbols in the center column
5. Inspect symbol members and quick links in the right column
6. Future: inspect syntax-highlighted source snippets in a bottom pane

## 5. Architecture

### 5.1 Backend

- `FileSystemModel`: recursive tree model exposed to QML
- `SymbolParser`: parser service using bundled Tree-sitter grammars with heuristic fallback where useful
- `ProjectController`: bridge object that manages selection, parsing, quick links, and derived models

### 5.2 Frontend

- `Kirigami.ApplicationWindow`
- picker-first flow with a dedicated path-selection screen
- central content built from Kirigami cards and list views
- future bottom source pane for snippets and line-focused inspection

## 6. Parser Strategy

Release 1 parser strategy:

- Use bundled Tree-sitter runtime and vendored official grammars where integrated
- Preserve heuristic extraction for specific convenience features where AST coverage is not yet wired
- Return stable structured data objects
- Keep parser entry points language-specific

## 7. Known Problems

- The QML layer is still too fragile and emits runtime errors when some expected sections are missing.
- Data contracts between C++ and QML need hardening.
- The explorer still feels shallow on real projects because it often lists symbols without enough architectural context.
- HTML/CSS analysis is still noisy.
- There is no embedded source pane yet, which limits usefulness significantly.

## 8. Roadmap

Phase 1. Stabilization

- remove current QML runtime errors
- normalize file detail payloads so every section is always safe to bind
- reduce noisy or misleading structural output

Phase 2. Better project structure

- improve CommonJS and Node service understanding
- improve project-level summaries and dependency navigation
- improve test linkage and route understanding

Phase 3. Source inspection

- add a bottom pane for syntax-highlighted source snippets
- sync selected symbols and routes to source lines
- add lint-aware or parser-aware snippet context where feasible

Phase 4. General polish

- add search/filtering
- improve density and readability
- refine navigation and jump behavior

## 9. Non-Goals

- editing files
- Git history
- semantic refactoring
- language-server integration
- full AST fidelity in release 1

## 10. Acceptance Criteria for the Current Phase

- Application builds locally against the installed Qt/Kirigami stack
- The filesystem tree is browsable from QML
- Supported source files show structural symbols
- HTML files show quick links for linked assets
- HTML/CSS class matching is visible in the detail pane
- Documentation explains current limits, next phases, and bundled dependency decisions

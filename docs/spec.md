# LumenCode Specification

## 1. Product

LumenCode is a structural file explorer for source trees. It focuses on code shape rather than freeform text editing. The application reads a filesystem tree, extracts symbols from supported source files, and lets the user drill from folders to files to symbols to member detail. It now includes source snippets for enhanced inspection.

LumenCode is intentionally:

- filesystem-aware
- parser-assisted (leveraging Tree-sitter for core languages)
- non-editing
- non-Git-aware

## 2. Technical Direction

Current target environment:

- Qt 5.15.13
- working Kirigami QML modules in the Qt5 import path
- a known-good Kirigami application in `../it-tools-kirigami`

Implementation decisions:

- Use Qt5 + KF5 Kirigami
- Vendor Tree-sitter core and official grammars into the repository
- Avoid external Tree-sitter package dependencies at build time and run time

## 3. Functional Scope

### 3.1 Column View

The current main window uses a three-column drill-down layout:

- Column 1: filesystem tree
- Column 2: selected file overview or selected folder summary
- Column 3: selected symbol detail and related links

Planned next layout step:

- Add a bottom pane for syntax-highlighted source snippets and snippet diagnostics.

### 3.2 Filesystem Rules

The crawler must:

- recurse directories without blocking the UI
- ignore `.git`
- ignore Python and C++ source trees by default
- focus on these file types: `js`, `jsx`, `ts`, `tsx`, `php`, `html`, `css`, `json`

### 3.3 Symbol Extraction

Current parser coverage, now enhanced with Tree-sitter and source snippets:

PHP:

- Classes, traits, interfaces, methods, properties, class constants, top-level constants.
- Symbols now include source snippets for easier inspection.

JavaScript and TypeScript:

- Exported functions, `const`/`let`/`var` declarations, classes, methods.
- CommonJS `require(...)` dependencies and ES module `import ... from ...` links.
- `module.exports` and `exports.*` API members, with improved export surfacing.
- Express route handlers and middleware-style endpoints where detectable.
- Related test files by naming convention.
- Symbols now include source snippets.

React TSX/JSX:

- Functional components, class components, props interfaces, type aliases.
- Hook-style functions beginning with `use`.

HTML:

- Linked scripts and stylesheets with improved path resolution.
- CSS classes used in markup, with cross-highlighting against available CSS.

CSS:

- Class selectors and custom properties.

JSON:

- `package.json` scripts, entrypoint, dependency lists, with a project summary.
- OpenAPI-style path and operation summaries, with method and description as snippets.

### 3.4 Neon Link Features

When an HTML file is selected:

- Inspect `<script src>` links.
- Inspect stylesheet links.
- Resolve sibling and relative paths against the HTML file.
- Show navigable quick links in the detail pane.

When an HTML file and sibling CSS files are available:

- Collect CSS class names from the HTML file.
- Collect CSS class selectors from sibling CSS files.
- Show matches and missing classes, with class names as snippets.

### 3.5 Node Project Features

When a Node/CommonJS project is explored:

- Surface local and package dependencies from `require(...)` and `import ... from ...`.
- Elevate exported API members from `module.exports` objects into the file overview and symbols.
- Detect Express routes such as `app.get(...)`, `app.post(...)`, and `router.use(...)`.
- Link source files to likely tests in nearby `tests/` folders.
- Parse `package.json` scripts, entrypoint, and dependency list, providing a project summary.

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

1.  Open app.
2.  Choose or confirm a root path.
3.  Browse folders and files in the left column.
4.  Inspect top-level symbols in the center column, with source snippets available.
5.  Inspect symbol members and quick links in the right column.
6.  Future: inspect syntax-highlighted source snippets in a bottom pane.

## 5. Architecture

### 5.1 Backend

- `FileSystemModel`: Recursive tree model exposed to QML, now includes file type counts and main entry point detection in its summary.
- `SymbolParser`: Parser service using bundled Tree-sitter grammars with heuristic fallback where useful; enhanced for snippets, exports, and improved dependency/route extraction.
- `ProjectController`: Bridge object managing selection, parsing, quick links, and derived models, now providing project summary data.

### 5.2 Frontend

- `Kirigami.ApplicationWindow`
- Picker-first flow with a dedicated path-selection screen.
- Central content built from Kirigami cards and list views.
- Future bottom source pane for snippets and line-focused inspection.

## 6. Parser Strategy

Release 1 parser strategy:

- Use bundled Tree-sitter runtime and vendored official grammars where integrated.
- Preserve heuristic extraction for specific convenience features where AST coverage is not yet wired.
- Return stable structured data objects, including source snippets.
- Keep parser entry points language-specific.

## 7. Known Problems

- The QML detail pane still emits runtime errors in some navigation paths, including `TypeError` and `Unable to assign [undefined] to bool`.
- Some extracted structure is still shallow or misleading on real projects.
- HTML/CSS class comparison can still be noisy on complex HTML documents.
- The UI is functional but underpowered; it does not yet justify itself as a serious structural explorer.
- There is currently no embedded source pane, which limits inspection usefulness significantly.

## 8. Roadmap

Phase 1. Stabilization

- Remove current QML runtime errors.
- Normalize file detail payloads so every section is always safe to bind.
- Reduce noisy or misleading structural output.

Phase 2. Better project structure

- Improve CommonJS and Node service understanding. **(Completed: Tree-sitter integration for better accuracy and export surfacing)**
- Improve project-level summaries and dependency navigation. **(Completed: Project summary implemented, improved dependency resolution)**
- Improve test linkage and route understanding. **(Completed: Basic Express route detection)**

Phase 3. Source inspection

- Add a bottom pane for syntax-highlighted source snippets. **(Initiated: Snippet generation added to symbol data)**
- Sync selected symbols and routes to source lines.
- Add lint-aware or parser-aware snippet context where feasible.

Phase 4. General polish

- Add search/filtering.
- Improve density and readability.
- Refine navigation and jump behavior.

## 9. Non-Goals

- editing files
- Git history
- semantic refactoring
- language-server integration
- full AST fidelity in release 1

## 10. Acceptance Criteria for the Current Phase

- Application builds locally against the installed Qt/Kirigami stack.
- The filesystem tree is browsable from QML.
- Supported source files show structural symbols with snippets.
- Current GUI note: snippets exist in backend data, but are not yet rendered in the GUI.
- HTML files show quick links for linked assets.
- HTML/CSS class matching is visible in the detail pane.
- Documentation explains current limits, next phases, and bundled dependency decisions.

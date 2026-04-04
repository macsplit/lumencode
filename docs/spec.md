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

The current main window uses a resizable explorer layout:

- Top left: thin global control rail plus filesystem tree
- Top center: selected file overview or selected folder summary
- Top right: selected symbol detail and related links
- Bottom: syntax-colored source pane with snippet diagnostics

Current layout behavior:

- The top and bottom regions are vertically resizable.
- The three top panes are horizontally resizable.
- The back action lives in the left control rail instead of a top toolbar.
- Explorer expand/collapse must preserve scroll position instead of jumping back to the top.
- Child entries must remain visibly indented under expanded folders.
- The right detail pane remains present at all times as the stable inspector surface.
- Top-level symbol blocks in the center pane are clickable as whole cards.
- Nested member rows in the center pane are individually clickable and must provide their own hover state.
- Arrow glyphs in the center pane are affordances only, not exclusive click targets.
- The overview pane must surface loading and bounded-analysis warnings explicitly rather than silently showing sparse or incomplete data.

### 3.2 Filesystem Rules

The crawler must:

- recurse directories without blocking the UI
- ignore `.git`
- focus on these file types: `js`, `jsx`, `ts`, `tsx`, `php`, `html`, `qml`, `css`, `json`, `py`, `c`, `cc`, `cpp`, `cxx`, `h`, `hpp`, `java`, `cs`, `rs`, `m`, `mm`
- focus on these file types: `js`, `jsx`, `ts`, `tsx`, `php`, `html`, `qml`, `css`, `json`, `py`, `c`, `cc`, `cpp`, `cxx`, `h`, `hpp`, `java`, `cs`, `rs`, `m`, `mm`, `swift`
- fail gracefully on hostile trees by applying bounded recursion and bounded entry counts instead of unbounded scans

Current crawler safeguards:

- maximum directory depth: `64`
- maximum scanned nodes per root load: `50000`
- maximum included entries per directory: `2000`

### 3.3 Symbol Extraction

Current parser coverage, now enhanced with Tree-sitter and source snippets:

PHP:

- Classes, traits, interfaces, methods, properties, class constants, top-level constants.
- Symbols now include source snippets for easier inspection.

JavaScript and TypeScript:

- Exported functions, `const`/`let`/`var` declarations, classes, methods.
- CommonJS `require(...)` dependencies and ES module `import ... from ...` links.
- Binding-aware cross-file relationship augmentation for named imports, aliased imports, and destructured CommonJS imports.
- `module.exports` and `exports.*` API members, with improved export surfacing.
- Express route handlers and middleware-style endpoints where detectable.
- Related test files by naming convention.
- Symbols now include source snippets.
- Fallback object/member extraction must not classify control-flow keywords as members.

React TSX/JSX:

- Functional components, class components, props interfaces, type aliases.
- Hook-style functions beginning with `use`.

QML:

- Heuristic extraction of imports, root components, inline components, properties, signals, functions, and common `on...` handlers.
- QML is expected to participate in the same snippet and lower-pane contract as other languages even without a dedicated AST grammar path yet.

HTML:

- Linked scripts and stylesheets with improved path resolution.
- CSS classes used in markup, with cross-highlighting against available CSS.
- CSS class matches and misses now carry snippets into the detail flow and lower source pane.
- HTML files are expected to expose linked local scripts/stylesheets as navigable quick links.

CSS:

- Class selectors and custom properties.
- CSS file class inspection now uses rule-level snippets instead of one-token previews.
- CSS files are also expected to surface reciprocal HTML consumer links and HTML-side matched/missing class usage when nearby HTML links them directly.

Python:

- Top-level functions, decorated functions, classes, and class methods.
- Imports and conservative Flask/FastAPI-style route extraction remain available.
- Tree-sitter-backed snippets now preserve decorator context and class/method boundaries.

Rust:

- Top-level `mod`, `fn`, `struct`, `enum`, `trait`, and `impl` items.
- `impl` and trait members are extracted from the AST with bounded snippets.
- `use` and external `mod` declarations are surfaced as dependencies.
- Grouped `use` imports retain full snippets while showing shorter dependency labels in the UI.

Swift:

- Top-level classes, structs, enums, protocols, actors, extensions, functions, and variables.
- Nested members extracted from the AST with bounded snippets.
- Intra-file call topology for callable declarations.
- Swift workspace scanning recognizes `Package.swift` and conventional SwiftPM entry locations.

JSON:

- `package.json` scripts, entrypoint, dependency lists, with a project summary.
- OpenAPI-style path and operation summaries, with method and description as snippets.

C/C++:

- Includes, top-level classes/structs, and top-level functions.
- Header/implementation related-file linking.
- Parsing remains heuristic.

Java:

- Tree-sitter-backed imports, top-level classes/interfaces/enums/records, constructors, methods, and field/property extraction.

C#:

- Tree-sitter-backed `using` imports, top-level types, constructors, methods, fields, and properties.
- Top-level statements/variables and basic ASP.NET route extraction from `Program.cs` style apps remain available.

Objective-C:

- `#import` dependency extraction.
- `@implementation` class extraction with per-class Objective-C method summaries.
- Parsing remains heuristic.

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

When a CSS or local script file is selected and nearby HTML files link it directly:

- Show inbound HTML consumer links in the right pane as `quickLinks`.
- Reuse the same selection payload path so those HTML consumer entries can drive the lower pane.
- For CSS specifically, show the HTML-side matched/missing class breakdown from linked consumer pages.

### 3.5 Node Project Features

When a Node/CommonJS project is explored:

- Surface local and package dependencies from `require(...)` and `import ... from ...`.
- Elevate exported API members from `module.exports` objects into the file overview and symbols.
- Detect Express routes such as `app.get(...)`, `app.post(...)`, and `router.use(...)`.
- Link source files to likely tests in nearby `tests/` folders.
- Parse `package.json` scripts, entrypoint, and dependency list, providing a project summary.

### 3.6 Snippet Contract

Selections that drive the lower pane carry an explicit snippet contract:

- `snippetKind`
  - `file_preview`
  - `exact_construct`
  - `block_excerpt`
  - `line_excerpt`
  - `context_excerpt`
- `diagnosticsMode`
  - `parse_snippet`
  - `none`

Contract rules:

- File previews and intentionally partial excerpts default to `diagnosticsMode: none`.
- Only standalone parseable snippets should request diagnostics.
- Dependencies, routes, quick links, and fallback parser excerpts must forward this contract unchanged from backend to QML.

### 3.7 Relationship Detail

Where available, symbol detail should include:

- nested members
- `Calls`
- `Called By`
- `Parameters`
- `Returns`

Current expectation:

- relationship sections are shown in the right pane when the backend provides them
- relationship entries are clickable and reuse the shared selection payload path
- callable signature fields are shown after higher-priority relation sections in the right pane
- `parameters` / `returns` are part of the backend symbol payload contract rather than controller-only UI enrichment
- parity is still incomplete across languages and files, so missing or asymmetric relations are currently a known limitation rather than a UI bug
- non-call web relationships should be modeled explicitly through quick links and CSS summaries rather than being forced into `Calls` / `Called By`

Analysis provenance contract:

- file analysis payloads may expose `analysisSourceMode` as `ast`, `heuristic`, or `recovered`
- file analysis payloads may expose `analysisConfidence` as a coarse trust signal
- file analysis payloads may expose `analysisHasAstErrors`, `analysisPartial`, and `analysisNotices`
- symbols and flat source-context items may expose `sourceMode` and `confidence`
- mixed analyses must preserve item-level provenance instead of flattening everything into a single fake authority

Callable signature contract:

- typed languages should prefer declared parameter types and declared return types
- untyped languages may fall back to parameter names plus inferred `return ...` expressions when no declared return type exists
- the current backend may still derive some signature fields from parser-layer snippet/signature analysis where grammar-specific AST field extraction has not yet been implemented

### 3.8 Robustness Contract

The app should degrade visibly and predictably under load:

- file analysis runs out-of-process through `lumencode-cli --dump-file`
- file and cross-file analysis in the GUI run asynchronously so the UI remains responsive
- oversized files are skipped with explicit summaries instead of being parsed anyway
- file previews are truncated with visible markers instead of loading unbounded content
- project-level relationship expansion is budgeted by time and by number of secondary file analyses
- when a budget is hit, the result is returned as partial and surfaced through overview warnings instead of silently dropping data

Warnings are part of the product contract, not decorative text:

- they mean the app stayed responsive and bounded its work
- they should still be treated as a debugging signal, because the cause may be algorithmic weakness or poor processing strategy rather than only repo size

Current parser authority model:

- AST output is the intended authority where a Tree-sitter path exists
- TS/TSX, Python, Java, C#, Rust, and PHP now keep partial AST output even when the tree has errors and supplement it with heuristic recovery, returning a `recovered` analysis rather than a file-wide fallback
- Swift and CSS still mostly behave as file-wide AST-or-heuristic paths because the current recovery handshake is not yet implemented there
- recovered analyses should normalize merged symbol trees and relation targets against the surviving canonical symbol set so obvious AST/heuristic duplicates do not leak directly into the UI
- the long-term direction is to apply that same authority/recovery pattern across the other Tree-sitter-backed languages instead of treating parsing as a file-wide pass/fail switch

Controller-side relationship budgeting model:

- incoming relationship scans should be skipped for low-value script files that expose no meaningful callable/API surface
- variable-only setup scripts should still parse and display normally, but should not spend project-wide relationship budget looking for importers

## 4. UX

### 4.1 Visual Style

The UI follows Kirigami conventions with a dark-first palette and a dense, low-chrome presentation.

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
6.  Inspect syntax-colored source snippets and lightweight diagnostics in the bottom pane.

## 5. Architecture

### 5.1 Backend

- `FileSystemModel`: Recursive tree model exposed to QML, now includes file type counts and main entry point detection in its summary, with bounded scanning safeguards for large trees.
- `SymbolParser`: Parser service using bundled Tree-sitter grammars with heuristic fallback where useful; enhanced for snippets, exports, snippet-contract metadata, improved dependency/route extraction, Swift parsing, and relation payloads.
- `ProjectController`: Bridge object managing selection, parsing, quick links, relation augmentation, derived models, bounded background analysis, and normalized lower-pane snippet payloads.
- `lumencode-cli`: Shared testing and isolation surface for one-shot analysis, interactive state testing, and crash-contained per-file parsing.

### 5.2 Frontend

- `Kirigami.ApplicationWindow`
- Picker-first flow with a dedicated path-selection screen.
- Central content built from split views, cards, and list views.
- Thin left control rail for global actions, file-opening shortcuts, and settings.
- Embedded bottom source pane for snippets and line-focused inspection.

## 6. Parser Strategy

Release 1 parser strategy:

- Use bundled Tree-sitter runtime and vendored official grammars where integrated.
- Keep bundled parser sources fully tracked in-repo so first-clone builds do not depend on broken submodule state.
- Preserve heuristic extraction for specific convenience features where AST coverage is not yet wired.
- Return stable structured data objects, including source snippets.
- Keep parser entry points language-specific.
- Keep helper-process isolation and heuristic fallback in place until native parser paths have been validated against real repro corpora.

## 7. Known Problems

- Some extracted structure is still shallow or misleading on real projects.
- Some parser paths are intentionally bypassed or downgraded to heuristic fallback on specific hostile inputs to preserve application stability.
- Relationship data is still incomplete; `Calls` and `Called By` are not yet consistently reciprocal after cross-symbol navigation.
- Project summary entrypoint selection is better for conventional roots, but still imperfect on broad multi-project trees.
- HTML/CSS class comparison can still be noisy on complex HTML documents.
- The current snippet highlighter is intentionally lightweight and not language-complete.
- Current diagnostics are parser-aware but not equivalent to full external linting.
- The UI is much denser and more usable now, but still needs polish.

## 8. Roadmap

Phase 1. Stabilization

- Remove current QML runtime errors.
- Normalize file detail payloads so every section is always safe to bind.
- Reduce noisy or misleading structural output.
- Continue corpus-driven CLI regression sweeps across heterogeneous real projects.
- Lock down the current pane layout and continue improving interaction density without changing the basic inspector model.

Phase 2. Better project structure

- Improve CommonJS and Node service understanding. **(Completed: Tree-sitter integration for better accuracy and export surfacing)**
- Improve project-level summaries and dependency navigation. **(Completed: Project summary implemented, improved dependency resolution)**
- Broaden parser coverage for common non-web languages used in mixed repos. **(Completed: Python and Rust are AST-backed; Java and C# are now AST-backed; C/C++ and Objective-C remain heuristic)**
- Improve test linkage and route understanding. **(Expanded: Express, Flask/FastAPI-style, and basic ASP.NET route extraction are present)**

Phase 3. Source inspection

- Add a bottom pane for syntax-highlighted source snippets. **(Completed with internal highlighting and diagnostics)**
- Continue AST-backed parity work, especially relationship extraction, before adding broader new UI concepts.
- Sync selected symbols and routes to source lines. **(Expanded: symbols, CSS class entries, dependencies, routes, and quick links now share the snippet-selection path)**
- Add lint-aware or parser-aware snippet context where feasible. **(Partially completed: conservative parser-aware diagnostics are present, and AST-backed snippets now cover more languages)**
- Continue improving snippet extraction quality so declarations are anchored cleanly and do not inherit leading braces, access labels, or unrelated context.

Phase 4. Native parser rehabilitation

- Use crash-isolated `lumencode-cli --dump-file` repros to recover native parser coverage language by language.
- Minimize crashing files and determine whether the fault is in LumenCode integration, snippet traversal, or upstream grammar/runtime code.
- Keep fallback parsing and helper-process isolation until native parser paths are proven stable on the regression corpus.

Phase 5. General polish

- Add search/filtering.
- Improve density and readability. **(Partially completed: denser explorer layout and larger default source pane landed)**
- Refine navigation and jump behavior. **(Partially completed: open-in-folder, open-in-editor, settings, and desktop launcher integration landed)**

## 9. Non-Goals

- editing files
- Git history
- semantic refactoring
- language-server integration
- full AST fidelity in release 1
- removing safety fallbacks before native parser paths are proven stable

## 10. Acceptance Criteria for the Current Phase

- Application builds locally against the installed Qt/Kirigami stack.
- The filesystem tree is browsable from QML.
- Supported source files show structural symbols with snippets.
- Snippets are rendered in the GUI lower pane.
- HTML files show quick links for linked assets.
- HTML/CSS class matching is visible in the detail pane.
- CSS class entries can drive the lower pane.
- Documentation explains current limits, next phases, and bundled dependency decisions.

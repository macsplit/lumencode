# Development Plan

## Goal

Bring `lumencode-swift` to functional parity with the Kirigami original where it matters, while taking advantage of a cleaner split between:

- native analysis core
- shared Swift core models/helpers
- platform-native macOS interface

## Current baseline

Implemented:

- real Xcode app target
- SwiftUI three-column explorer plus lower snippet pane
- project scanning and project summary
- selection-driven symbol inspection
- dependency and route inspection hooks
- native C++ bridge returning structured JSON
- internal CLI harness for backend iteration

Missing or incomplete:

- Tree-sitter-backed parsing parity
- cross-domain linkage between HTML, CSS, and JavaScript facts
- symbol-topology parity for calls and called-by relationships
- typed-signature and type-link parity for typed languages
- richer quick links and related-file behaviors
- lower-pane diagnostics and highlighting parity
- more complete language support
- better package, HTML/CSS, and framework-specific structure output
- deeper symbol/member detail interactions

## Workstreams

## 1. Native analysis parity

Primary objective:

- move the current bridge from placeholder heuristics toward reused or extracted logic from the original parser stack

Tasks:

- extract reusable parser logic from the Kirigami project behind a C ABI
- preserve the JSON payload contract so Swift stays decoupled from Qt types
- expand coverage for:
  - JavaScript and TypeScript exports/imports
  - PHP classes and members
  - Python classes, functions, imports, routes
  - Rust items and `use` extraction
  - Java and C# type and import extraction
- restore package summary richness
- add quick links, related files, and route payload parity
- add symbol-topology payloads for:
  - same-file calls
  - same-file called-by links
  - project-level call indexing later where confidence is strong
- add typed-signature payloads for typed languages:
  - parameter names
  - parameter types
  - return types
  - property and variable types where declared
  - generic type parameters where easy to extract
- use type data to improve:
  - symbol labels and signatures in the inspector
  - cross-file navigation from types to type definitions
  - method-call resolution where receiver type is explicit
  - overload and same-name disambiguation in typed languages
- add a shared linkage layer for:
  - HTML class usage to CSS selectors
  - HTML script and stylesheet references to source files
  - JavaScript and TypeScript selector usage to HTML and CSS entities
  - React and TSX stylesheet, asset, and component relationships where confidence is strong
  - JSX and TSX class usage to imported or sibling stylesheets

Definition of done:

- CLI output for representative repos is meaningfully close to the Kirigami original
- Swift UI can render the richer payload without custom fallback glue

High-value type-driven use cases:

- show full function and method signatures instead of just bare symbol names
- click a parameter type or return type to jump to the related type definition
- use receiver and parameter types to improve `calls` and `calledBy` accuracy
- make service, DTO, model, and API exploration easier in Swift, TypeScript, Java, C#, and Rust codebases
- expose property and field types in the inspector for faster data-model understanding

## 2. Lower-pane fidelity

Primary objective:

- make the lower pane genuinely useful for inspection work

Tasks:

- improve file preview behavior
- support snippet selection from:
  - file
  - symbol
  - member
  - dependency
  - route
  - quick link
- add lightweight syntax highlighting
- add conservative diagnostics metadata to the snippet payload
- show typed signatures in snippet headers and detail summaries when available
- preserve snippet contract parity with the original app

Definition of done:

- the lower pane is the canonical inspection surface in both versions

## 3. Interaction parity

Primary objective:

- keep the Swift app aligned with the useful interaction model of the Kirigami version without copying Kirigami-specific presentation

Tasks:

- better selection persistence when switching files
- clickable members in both overview and detail panes
- make it obvious when a selected symbol has:
  - no resolved calls
  - no resolved callers
- expose signature-aware labels for typed symbols
- add type-link inspection actions when type targets are known
- quick-link and related-file navigation
- package summary inspection
- status surfaces for unsupported or degraded analysis
- keyboard navigation polish

Definition of done:

- core workflows feel equivalent even when the visuals differ

## 4. macOS polish

Primary objective:

- make the Swift app feel native on macOS instead of like a portability shim

Tasks:

- improve Dock icon asset quality
- refine typography and spacing
- improve split-view defaults and persistence
- add standard macOS menu commands and shortcuts
- support reopening last project
- add file reveal/open-in-editor actions

Definition of done:

- the app feels intentionally macOS-native rather than minimally functional

## 5. Tooling and validation

Primary objective:

- use the CLI to keep backend iteration fast and reproducible

Tasks:

- add a small regression corpus under this repo later
- add JSON snapshots for selected sample files/projects
- document expected payload invariants
- compare Swift-port CLI output against Kirigami CLI output for parity checks
- add representative web-stack fixtures that exercise cross-domain linkage

Definition of done:

- backend changes can be validated without manual GUI testing

## Suggested implementation order

1. strengthen the native bridge payloads to match the original data contract more closely
2. expand Tree-sitter coverage for mainstream languages used by the original app
3. add cross-domain HTML, CSS, and JavaScript linkage on top of per-language AST facts
4. extend that linkage to mainstream React and TSX file relationships
5. add quick links, related files, and richer package summary support
6. add typed-signature extraction and type-link navigation for typed languages
7. improve lower-pane rendering and syntax emphasis
8. refine macOS interaction polish
9. add regression fixtures and parity checks

Near-term follow-up:

- expand call-topology extraction beyond current same-file script and TSX cases
- add stronger call resolution for Swift and other AST-backed languages
- move from per-file call edges toward project-level reverse links where practical
- extract declared parameter and return types for Swift, TypeScript, Java, C#, Rust, and typed Python/PHP where available
- use explicit type information to improve symbol exploration before attempting deeper inference

## Non-goals for now

- duplicating Qt/Kirigami UI structure line for line
- introducing editing features
- making the CLI a polished end-user tool
- adding Git-aware behavior

## Decision rule

When choosing between speed and architecture:

- reuse native analysis logic where possible
- keep the Swift UI and Qt UI separate
- keep the payload contract shared
- prefer a stable C boundary over direct Swift-to-Qt coupling

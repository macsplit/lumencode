# Swift and Kirigami Parity Guide

## Purpose

This document explains how to evolve `lumencode-swift` in parallel with the original Kirigami/Qt version without creating accidental behavioral drift.

The key rule is:

- keep analysis and payload behavior in parity
- allow interface-layer implementation to differ by platform

## Repositories in play

- Kirigami original:
  - `/Users/user/Code/Coding/lumencode`
- Swift port:
  - `/Users/user/Code/Coding/lumencode-swift`

## Provenance and licensing

Parser parity work now includes vendored Tree-sitter code. Keep the provenance boundary explicit:

- vendored runtime and grammar code must keep upstream license notices
- generated parser sources must be documented as generated/vendor-derived inputs
- handwritten bridge code should remain clearly separate from vendored/generated files

Reference:

- [Licensing and Derivation](licensing-and-derivation.md)

## What must stay in parity

These are product behaviors or data contracts, not UI toolkit details.

## 1. File analysis contract

The meaning of these payloads should stay aligned:

- `FileAnalysis`
- `SymbolItem`
- `DependencyItem`
- `RouteItem`
- `RelatedFileItem`
- `QuickLinkItem`
- `PackageSummary`
- `CSSSummary`
- `SnippetPayload`
- `ProjectSummary`

Parity means:

- same conceptual fields
- same field meanings
- same selection semantics
- same snippet-kind behavior
- same diagnostics-mode intent

Field ordering and framework-specific serialization details do not matter.

## 2. Selection behavior

When the user inspects a file or sub-item, both implementations should conceptually do the same thing:

- selecting a file updates overview, detail context, and lower snippet pane
- selecting a symbol updates detail context and lower snippet pane
- selecting a dependency or route can drive snippet inspection without forcing file navigation
- selecting a related file navigates to that file

The exact buttons, cards, and layout may differ.

## 3. Project scanning rules

These behaviors should stay aligned:

- supported file types
- ignored directories
- scan limits
- project entrypoint heuristics
- project summary meaning

## 4. Parser capabilities

Language coverage should be tracked for parity, even if implementation differs.

Parity-sensitive areas:

- JS/TS exports/imports
- JSX/TSX class usage and stylesheet imports
- React-style cross-extension links such as component to CSS, CSS module, and asset files
- symbol topology such as `calls` and `calledBy` links where the analyzer has strong confidence
- typed signatures such as parameter lists, return types, and declared property types in typed languages
- type-driven navigation such as jumping from a parameter or return type to the related definition where confidence is strong
- PHP class/member extraction
- Python imports/routes
- Rust item extraction
- Java/C# type extraction
- package.json summaries
- HTML/CSS quick links and class summaries
- cross-domain HTML, CSS, and JavaScript linkage
- route extraction
- related-file linking

## What is interface-layer only

These can differ between Swift and Kirigami without being considered parity bugs.

## 1. View technology

- SwiftUI view composition
- Kirigami cards, controls, and split views
- AppKit menu integration
- Qt-specific toolbar/rail behavior

## 2. Styling

- exact fonts
- exact spacing
- exact icon system
- color system
- animation details
- desktop conventions specific to macOS vs Qt/Kirigami

## 3. Platform integration

- Dock icon handling
- Xcode project setup
- Swift Package setup
- macOS command menus
- Qt/KDE launcher integration

## Recommended development flow

## Backend and payload work

Use the Swift CLI harness first.

Why:

- fast iteration
- scriptable output
- no manual GUI loop required
- easier parity comparison against the Kirigami CLI behavior

Typical flow:

1. change native bridge or shared core
2. run `swift run lumencode-swift-cli ...`
3. inspect JSON output
4. compare against expected behavior from the original app
5. only then verify the GUI presentation

## GUI work

Use the real Xcode app target.

Typical flow:

1. open `lumencode-swift.xcodeproj`
2. run the `lumencode-swift` scheme
3. verify that the UI presents the already-validated payload correctly

## When to port behavior from Kirigami

Port behavior directly when it affects:

- what data is shown
- what a click means
- what the lower pane shows
- how project summaries or links are derived

Do not port line for line when it only affects:

- how Kirigami layouts are composed
- Qt-specific models and signal wiring
- QML convenience structures

## Safe boundary for reuse

Best reuse target:

- native parsing and analysis logic behind a C ABI

Avoid direct reuse of:

- `QObject`-heavy controller logic
- QML-facing property wiring
- Qt model classes as a portability strategy

Those are implementation details of the Kirigami interface layer, not the product contract.

## Parity checklist for future changes

When changing analysis or interaction behavior, ask:

1. Does this change the meaning of a shared payload field?
2. Does this change what appears in the lower snippet pane?
3. Does this change how a symbol/dependency/route selection behaves?
4. Does the Kirigami version already do this differently?
5. Is the difference product-level or just interface-layer?

If the answer is product-level, document it and decide whether both repos should change.

## Current known asymmetry

Kirigami original is ahead in:

- parser depth
- Tree-sitter integration breadth
- lower-pane diagnostics/highlighting
- quick links and related-file richness
- package and web-stack detail extraction

Swift version is ahead or different in:

- native macOS app integration
- Xcode-first workflow
- cleaner separation between app shell and analysis boundary

## Practical rule of thumb

If a user would notice the difference in what the app understands about code, it is probably a parity issue.

If a user would notice only a difference in how the macOS app looks or how the Qt app is constructed, it is probably interface-layer only.

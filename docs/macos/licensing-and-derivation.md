# Licensing and Tree-sitter Derivation

The active engine is now pinned under `Engine/Upstream`, including its original license and third-party notices. `Engine/upstream.json` records source hashes and revision. The runtime packaging tool copies Qt license texts, available transitive dependency notices and source/version receipts into `EngineRuntime/licenses`. Complete release dependency notices and signing remain release gates; see [Engine build and packaging](../Engine/README.md).

## Purpose

This document records where the vendored Tree-sitter code in `lumencode-swift` comes from, what parts are generated, and what license obligations must be preserved.

## Vendored components

## 1. Tree-sitter runtime

Source upstream:

- `https://github.com/tree-sitter/tree-sitter`

Relevant code family:

- `lib/src/lib.c`
- `lib/src/*.h`
- `lib/src/unicode/*`
- `lib/src/portable/*`
- `lib/include/tree_sitter/api.h`

License:

- MIT
- Copyright (c) 2018 Max Brunsfeld

Role in `lumencode-swift`:

- shared parsing runtime used by generated language parsers
- compiled directly into the native bridge as vendored source code

## 2. Tree-sitter Swift grammar

Source upstream:

- `https://github.com/alex-pinkus/tree-sitter-swift`

Relevant code family:

- generated `src/parser.c`
- `src/scanner.c`
- grammar-specific headers under `src/tree_sitter`
- binding header `bindings/swift/TreeSitterSwift/swift.h`

License:

- MIT
- Copyright (c) 2021 alex-pinkus

Role in `lumencode-swift`:

- Swift syntax parser used to replace regex-only Swift extraction with AST-backed analysis

## Derivation notes

The maintained Swift grammar upstream does not keep `parser.c` on its main branch. For this project, generated parser sources are derived from the upstream project’s published generated-files branch/workflow output, then vendored into this repo as plain source code.

This mirrors the original Kirigami `lumencode` approach:

- one shared Tree-sitter runtime
- one generated parser source set per language
- all parser code compiled directly into the application
- no end-user dependency on the Tree-sitter CLI or app

## What must be preserved

When updating vendored Tree-sitter code:

- keep upstream license texts intact
- keep provenance notes for runtime vs grammar code
- note whether `parser.c` was generated locally or copied from an upstream generated-files branch/artifact
- avoid mixing original handwritten bridge code with copied/generated code in the same file without documenting the boundary

## Repo policy for this project

For `lumencode-swift`, Tree-sitter is treated as vendored source, not as an installed dependency.

That means:

- Xcode builds must not require the `tree-sitter` CLI
- SwiftPM builds must not require npm or other package-manager setup
- all code necessary to compile the parser path must live in the repository

## Interface boundary

Vendored Tree-sitter code is implementation detail.

The stable product contract remains:

- `FileAnalysis`
- symbol/dependency/route payload meaning
- snippet semantics
- selection behavior exposed to the GUI and CLI harness

This lets parser internals evolve without changing the Swift app surface or the CLI-driven development loop.

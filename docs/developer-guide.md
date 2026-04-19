# LumenCode Developer Guide

This guide explains how LumenCode is structured, how data moves through the system, and how to work on it without destabilizing the product.

The project goal is not “parse everything perfectly.” The goal is “stay responsive, produce useful structural output, and degrade visibly rather than flakily.”

## System Overview

```mermaid
flowchart LR
    FS[FileSystemModel] --> PC[ProjectController]
    PC --> GUI[QML UI]
    PC --> CLI[lumencode-cli]
    CLI --> SP[SymbolParser]
    PC --> Helper[Helper process\nlumencode-cli --dump-file]
    Helper --> SP
    SP --> TS[Tree-sitter parsers]
    SP --> H[Heuristic parsers]
```

## Core Runtime Flow

```mermaid
sequenceDiagram
    participant UI as QML
    participant PC as ProjectController
    participant HP as Helper Process
    participant SP as SymbolParser
    UI->>PC: selectPath(...)
    PC->>PC: beginAsyncAnalysis(...)
    PC->>HP: lumencode-cli --dump-file
    HP->>SP: parseFile(...)
    SP-->>HP: analysis JSON
    HP-->>PC: parsed result
    PC->>PC: augment relationships
    PC-->>UI: selectedFileData / selectedSymbol / selectedSnippet
```

## Main Components

### `FileSystemModel`

- Owns the visible project tree.
- Applies crawl limits.
- Filters supported source types.

### `ProjectController`

- Owns selected path, selected file payload, selected symbol, and lower-pane snippet state.
- Runs file analysis asynchronously.
- Rehydrates relation clicks into actual symbols.
- Adds bounded project-level relationship augmentation.

### `SymbolParser`

- Produces file analysis payloads.
- Uses Tree-sitter where supported.
- Uses heuristics where Tree-sitter is unavailable or intentionally avoided.
- Owns symbol signatures, provenance, and recovered-analysis behavior.

### `lumencode-cli`

- Exposes the same backend contract as the GUI path.
- Used for crash isolation.
- Used for fixtures and real-repo sweeps.

## Parsing Authority Model

```mermaid
flowchart TD
    A[Parse file] --> B{Tree-sitter path exists?}
    B -- No --> H[Heuristic result]
    B -- Yes --> C[AST result]
    C --> D{AST has errors?}
    D -- No --> E[Authoritative AST result]
    D -- Yes --> F[Recovered result]
    F --> G[Keep AST-owned structure]
    F --> I[Heuristic supplementation only for AST-uncovered ranges]
    G --> J[Merged recovered payload]
    I --> J
```

This is the current stable recovery contract. Earlier generic error-node harvesting was backed out because it was not stable enough across grammars.

## Data Contract

The important backend payloads are:

- file analysis
- symbol tree
- relation entries
- source-context items for lower-pane navigation

Important file-level fields:

- `symbols`
- `dependencies`
- `routes`
- `quickLinks`
- `relatedFiles`
- `summary`
- `analysisSourceMode`
- `analysisConfidence`
- `analysisHasAstErrors`
- `analysisPartial`
- `analysisNotices`

Important symbol-level fields:

- `kind`
- `name`
- `line`
- `detail`
- `snippet`
- `members`
- `calls`
- `calledBy`
- `parameters`
- `returns`
- `sourceMode`
- `confidence`

## Selection and Rehydration Model

```mermaid
flowchart TD
    A[User clicks symbol or relation] --> B{Target file already loaded?}
    B -- No --> C[Queue pending selection]
    C --> D[Async analysis completes]
    D --> E[Hydrate real symbol from loaded payload]
    B -- Yes --> E
    E --> F[Update selectedSymbol and selectedSnippet]
```

Important rule: thin relation payloads should never remain the final selected state when a real symbol can be hydrated from the current file payload.

## Relationship Layers

```mermaid
flowchart LR
    A[Same-file AST relations] --> D[Final symbol relations]
    B[Same-file snippet fallback] --> D
    C[ProjectController cross-file augmentation] --> D
```

Guidance:

- prefer AST walks first
- keep snippet fallback supplemental
- budget cross-file augmentation aggressively
- gate low-value expensive work

## Robustness Model

```mermaid
flowchart TD
    A[User selects file] --> B[Async helper analysis]
    B --> C{Result usable?}
    C -- Yes --> D[Render payload]
    C -- Partial --> E[Render payload + warning]
    C -- No --> F[Render explicit fallback summary]
```

Current robustness interventions include:

- helper-process crash isolation
- async analysis
- large-file refusal
- minified/bundled asset skipping
- bounded relationship augmentation
- explicit loading and warning states
- recovered-analysis provenance

## How To Add Or Improve A Language

```mermaid
flowchart TD
    A[Pick one language problem] --> B[Add or improve parser output]
    B --> C[Add fixture coverage]
    C --> D[Run fixture-only sweep]
    D --> E[Run normal corpus sweep]
    E --> F[Run broader stress sweep]
    F --> G[Only then inspect GUI behavior]
```

Rules:

- avoid broad parser-wide changes when a language-specific step will do
- keep the CLI as the main regression loop
- prefer improving payload quality before touching UI presentation

## Regression Strategy

There are three useful lanes:

```bash
python3 tools/regression_sweep.py --fixtures-only
python3 tools/regression_sweep.py --max-files 24 --limit-per-project 3
python3 tools/regression_sweep.py --max-files 80 --limit-per-project 4
```

Use them in that order.

## What Not To Do

- Do not treat warnings as harmless by default.
- Do not add expensive controller work without a clear budget.
- Do not let heuristics compete with AST output across the whole file.
- Do not rely on GUI behavior as the first signal of parser correctness.

## Current Architectural Boundaries

- Swift is AST-backed but still not as rich in inspector payload as desired.
- QML remains heuristic.
- Signature extraction is backend-owned, but not every language uses grammar-node extraction yet.
- Cross-file relations are materially better, but still not uniformly complete across all languages and repos.

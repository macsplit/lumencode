# LumenCode End-User Guide

LumenCode is a structural code explorer. It is designed to help you understand how a codebase is organized, what a file exposes, and how symbols relate to each other.

It is not a text editor and it is not a Git client. The product value is fast structural inspection.

## Mental Model

```mermaid
flowchart LR
    A[Choose project root] --> B[Browse folders and files]
    B --> C[Open a file]
    C --> D[Inspect top-level symbols]
    D --> E[Inspect symbol detail]
    E --> F[Read snippet in lower pane]
    E --> G[Follow calls, callers, links, routes, dependencies]
```

## Main Screen

```mermaid
flowchart TB
    subgraph Top
        L[Left pane\nProject tree]
        M[Middle pane\nFile overview]
        R[Right pane\nInspector]
    end
    B[Bottom pane\nSnippet and diagnostics]
    L --> M
    M --> R
    R --> B
```

### Left Pane

- Shows the project tree.
- Lets you move through folders and files.
- Uses `/` as the visible project root.

### Middle Pane

- Shows the structural overview of the selected file.
- Top-level symbols are clickable blocks.
- Nested members are individually clickable rows.

### Right Pane

- Always visible.
- Shows the current symbol’s detail.
- Typical sections include members, `Calls`, `Called By`, `Parameters`, `Returns`, quick links, dependencies, CSS summaries, and related files.

### Bottom Pane

- Shows the current snippet.
- For files: preview text.
- For symbols and related items: focused excerpts.
- Diagnostics appear when the snippet is meant to be parseable.

## Typical Workflow

```mermaid
sequenceDiagram
    participant U as User
    participant T as Tree
    participant O as Overview
    participant I as Inspector
    participant S as Snippet Pane
    U->>T: Select file
    T->>O: Show file structure
    U->>O: Click symbol
    O->>I: Show detail
    O->>S: Show symbol snippet
    U->>I: Click relation or quick link
    I->>O: Move to new symbol or file
    O->>S: Refresh snippet
```

## What To Expect In Different File Types

### Code files

- Functions, methods, classes, structs, interfaces, traits, modules, properties, and constants may appear.
- Some languages also expose routes, imports, exports, and related files.

### Web files

- HTML can expose linked scripts and stylesheets.
- CSS can expose classes and linked HTML consumers.
- JS/TS can expose cross-file calls and inbound HTML consumers.

### Package and API metadata

- `package.json` can expose scripts, entrypoints, and dependencies.
- OpenAPI-style JSON can expose route summaries.

## Understanding Warnings

Warnings are part of normal bounded behavior, not necessarily a failure.

```mermaid
flowchart TD
    A[Analysis starts] --> B{Normal size and cost?}
    B -- Yes --> C[Full structural result]
    B -- No --> D[Bounded partial result]
    D --> E[Warning shown in UI]
    E --> F[App stays responsive]
```

Common reasons:

- very large file
- minified or bundled asset
- expensive cross-file relationship work
- broken source where only partial structure is safe to show

## Known Product Boundaries

- Some languages are deeper than others.
- Cross-file call graphs are useful but not universally complete.
- QML support is useful but still heuristic.
- Swift symbol extraction exists, but some Swift inspector views are still sparser than ideal.

## Practical Tips

- Start from a narrow project root when you want faster, cleaner relation data.
- Use the middle pane to pick the right symbol before relying on the right pane.
- Treat warnings as “bounded result returned” rather than “application broke”.
- Use the CLI when you want repeatable inspection or regression-style checks.

## CLI Use

```bash
./build/bin/lumencode-cli --dump-file /path/to/file
```

```bash
./build/bin/lumencode-cli -i
```

The CLI is useful when you want the same backend payloads without the GUI.

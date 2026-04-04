# CLI Testing Strategy & Implementation

This document outlines the `lumencode-cli` tool created to enable robust, "like-for-like" testing of the LumenCode project analysis engine without relying on the GUI.

## Overview

The `lumencode-cli` is a standalone executable that exercises the same core libraries as the main `lumencode` application:
- `ProjectController`: High-level application state and logic.
- `FileSystemModel`: Directory scanning, filtering, and tree structure.
- `SymbolParser`: Language-specific symbol and metadata extraction, now using Tree-sitter for TS/TSX, PHP, CSS, Python, Rust, Java, C#, and Swift where integrated, while retaining heuristics where coverage is still incomplete.
- `SymbolParser`: Plain JS/JSX currently remain on the heuristic path for stability, but are still expected to participate in the same relation and snippet contracts as the AST-backed languages.

By using the same underlying classes, the CLI provides an accurate representation of what the GUI would display, making it ideal for automated testing and rapid iteration on parsing features.
The CLI is also now part of the runtime safety story: the GUI routes file analysis through `lumencode-cli --dump-file`, so parser crashes are contained to the helper process rather than taking down the main app.

## Features

- **One-shot Mode**: Analyze a project root, select a file, and extract symbols, dependencies, routes, and source snippets in a single command.
- **Crash-Isolated Dump Mode**: `--dump-file <path>` parses exactly one file and prints the same analysis JSON used by the GUI-safe selection path.
- **Persistent Interactive Mode (`-i`)**: Maintains application state across multiple JSON-based commands on `stdin`.
- **JSON Output**: All state changes and analysis results are emitted as compact JSON for easy machine processing, including source snippets for symbols.
- **Snippet Contract Validation**: Interactive state includes `selectedSnippet`, exposing `snippetKind` and `diagnosticsMode` so excerpt-vs-parseable behavior can be tested without QML.
- **Corpus Sweep Harness**: `tools/regression_sweep.py` samples projects under `/home/user/Code`, drives interactive selection, and checks snippet, diagnostics, and member-shape invariants across many files.
- **Fixture Baselines**: `tests/fixtures/baseline/manifest.json` defines a checked-in multi-language fixture corpus for deterministic regression runs.
- **Relation Checks**: The sweep now includes relation-presence checks and relation round-trip checks for languages where `Calls` / `Called By` should exist, while tolerating files that do not contain enough callable symbols to make that meaningful.
- **Fixture Relation Coverage**: The checked-in fixture corpus now asserts concrete relation pairs for JS, TS, PHP, Swift, Python, Rust, Java, and C# rather than only symbol presence.

## Testing Strategy

### 1. Verification of Parsing (Cross-Language)
Test the `SymbolParser` against diverse file types to ensure correct symbol extraction, line numbering, metadata (e.g., dependencies, routes), and generation of source snippets.

**Targets:**
- **PHP**: `src/symbolparser.cpp` (for internal tests if available) and sample PHP files.
- **TypeScript/JavaScript/JSX/TSX**: Utilized `it-tools` project (`src/router.ts`) for testing ES module imports, exports, and `vue-router` structure. Also tested CommonJS (`module.exports`) using `APIScraping/digest/urltomarkdown/url_to_markdown_processor.js`.
- **HTML/CSS**: Tested with `Android2023/www/index.html` and associated CSS.
- **JSON**: Tested `package.json` for scripts and dependencies, and OpenAPI JSON for route definitions.
- **Python**: Tested Flask-style routes and imports using `HardwareIoT/whisper-web-service/app.py`.
- **Rust**: Tested `impl`, `mod`, `use`, and bounded snippets using `Gaming/sokoban-emoji/src/main.rs`.
- **C/C++**: Tested function/include extraction using `Coding/dummy_languages/dummy.cpp`.
- **Java**: Tested Tree-sitter-backed class/member/import extraction using `Coding/dummy_languages/dummy.java` and `Coding/Android-WebView-Example/app/src/main/java/com/books/webview/MainActivity.java`.
- **C#**: Tested Tree-sitter-backed type/member/import extraction using `dotnet/TimeTracker/Models/TimesModel.cs` and top-level ASP.NET-style program files using `dotnet/TimeTracker/Program.cs`.
- **Swift**: Tested Tree-sitter-backed symbol/member extraction and intra-file relation payloads using files under `Coding/lumencode-swift/Sources`.
- **Objective-C**: Tested heuristic class/import extraction using `TimeSystem/DesignApp/platforms/ios/Time Designer/Classes/MainViewController.m`.

### 2. State Persistence & Sequencing
In interactive mode, verify that commands correctly update the internal state and that subsequent commands operate on that updated state.

**Example Sequence:**
1. `setRootPath`: Initialize the project.
2. `toggleExpanded`: Expand a directory to update the visible file system.
3. `selectPath`: Choose a file within that expanded structure.
4. `selectSymbol`: Extract detailed information about a specific symbol, including its snippet.

### 3. Comparison with GUI Behavior
Ensure that the JSON output from the CLI matches the data expected by the QML components, particularly regarding the presence and accuracy of symbols, snippets, project summaries, and selection payloads used by the lower source pane.

### 4. Regression Testing
Use the CLI in two layers:

- Checked-in fixture baselines under `tests/fixtures/baseline/` for deterministic structural coverage.
- Broader local-corpus sweeps under `/home/user/Code` to catch noisy or weak output on real projects.

The fixture layer should stay small, structural, and intentional. It is where every supported language or language-cluster should eventually have at least one minimal project that exercises symbols, members, imports, exports, snippets, and relations where applicable.

### 5. Safety Regression Testing
Exercise pathological inputs and failure-containment paths:

- Deep or wide directory trees, verifying bounded crawl behavior instead of unbounded recursion.
- Large files, verifying parser refusal/truncation summaries instead of full in-memory analysis.
- Known parser repros, verifying that `--dump-file` exits cleanly or fails in isolation without crashing the GUI.
- Snippet-contract probes, verifying that excerpts do not emit diagnostics and declaration snippets do not inherit leading braces, access labels, or control-flow keywords as fake members.

## Usage Examples

### One-shot analysis
```bash
./build/bin/lumencode-cli . -p src/symbolparser.cpp
```

### Crash-isolated single-file dump
```bash
./build/bin/lumencode-cli --dump-file /home/user/Code/APIScraping/crossword/cross.js
```

### Fixture-only regression pass
```bash
python3 tools/regression_sweep.py --fixtures-only
```

### Fixture plus local-corpus sweep
```bash
python3 tools/regression_sweep.py --max-files 24 --limit-per-project 3
```

### Interactive session
```bash
./build/bin/lumencode-cli -i
# Command 1: Set root
{"command": "setRootPath", "params": {"path": "."}}
# Command 2: Select a file
{"command": "selectPath", "params": {"path": "src/main.cpp"}}
# Command 3: Get project summary
{"command": "getProjectSummary"}
# Command 4: Exit
{"command": "exit"}
```

## Available Commands (Interactive)
- `setRootPath`: `{"path": "..."}`
- `selectPath`: `{"path": "..."}`
- `selectSymbol`: `{"index": <int>}`
- `selectSymbolByData`: Pass a payload object matching the GUI snippet-selection contract.
- `toggleExpanded`: `{"path": "..."}`
- `getState`: (No params)
- `getProjectSummary`: (No params)
- `exit`: (No params)

## Current Notes

- The CLI remains the best way to regression-test parsing and payload structure without launching the GUI.
- `--dump-file` is now the preferred repro path for parser crashes because it matches the helper process used by the GUI selection flow.
- The CLI can now exercise lower-pane source-context payloads directly through `selectSymbolByData`, making dependency/route/quick-link regressions testable without QML interaction.
- `tools/regression_sweep.py` currently serves as the broad local corpus harness for snippet-contract and payload-shape regressions.
- `tools/regression_sweep.py` now also supports a manifest-driven fixture suite. `--fixtures-only` should be the quickest regression check before broader corpus runs.
- `tools/regression_sweep.py` now resolves the active checkout path dynamically, so it can be run from `Coding/lumencode` without editing hard-coded repository paths.
- The harness now follows relation targets through `selectSymbolByData` and verifies that reverse edges are present on the selected destination symbol.
- The GUI now adds extra presentation behavior on top of the shared backend data, including lower-pane snippet rendering, compact snippet diagnostics, and CSS class drill-down behavior.
- Project-summary regressions are also worth checking through the CLI because `mainEntry` scoring is still evolving for mixed-language roots.
- Full GUI parity should not be assumed for purely visual concerns such as splitter layout, lightweight syntax coloring, or rich-text snippet presentation.
- Relation parity should also not be assumed yet; `Calls` and `Called By` payloads are materially better than before, but remain incomplete and sometimes asymmetric on real projects.
- Python now uses an AST walk for same-file call relations because bounded snippets were not sufficient on docstring-heavy real files. Similar upgrades are still on the table for other languages if the corpus sweep exposes the same pattern.
- Remaining native parser rehabilitation should proceed through the CLI first: collect a crashing file, minimize the repro, verify whether the fault is in LumenCode integration or an upstream grammar/runtime, and only then reduce the fallback/isolation layers for that language.

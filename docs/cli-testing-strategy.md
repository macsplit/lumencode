# CLI Testing Strategy & Implementation

This document outlines the `lumencode-cli` tool created to enable robust, "like-for-like" testing of the LumenCode project analysis engine without relying on the GUI.

## Overview

The `lumencode-cli` is a standalone executable that exercises the same core libraries as the main `lumencode` application:
- `ProjectController`: High-level application state and logic.
- `FileSystemModel`: Directory scanning, filtering, and tree structure.
- `SymbolParser`: Language-specific symbol and metadata extraction, now using Tree-sitter for JS/TS/TSX, PHP, CSS, Python, Rust, Java, and C#, while retaining heuristics where coverage is still incomplete.

By using the same underlying classes, the CLI provides an accurate representation of what the GUI would display, making it ideal for automated testing and rapid iteration on parsing features.

## Features

- **One-shot Mode**: Analyze a project root, select a file, and extract symbols, dependencies, routes, and source snippets in a single command.
- **Persistent Interactive Mode (`-i`)**: Maintains application state across multiple JSON-based commands on `stdin`.
- **JSON Output**: All state changes and analysis results are emitted as compact JSON for easy machine processing, including source snippets for symbols.

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
Use the CLI to create a suite of "golden" JSON outputs for known projects. Future changes to the parser can be automatically verified against these baselines.

## Usage Examples

### One-shot analysis
```bash
./build/bin/lumencode-cli . -p src/symbolparser.cpp
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
- The CLI can now exercise lower-pane source-context payloads directly through `selectSymbolByData`, making dependency/route/quick-link regressions testable without QML interaction.
- The GUI now adds extra presentation behavior on top of the shared backend data, including lower-pane snippet rendering, compact snippet diagnostics, and CSS class drill-down behavior.
- Project-summary regressions are also worth checking through the CLI because `mainEntry` scoring is still evolving for mixed-language roots.
- Full GUI parity should not be assumed for purely visual concerns such as splitter layout, lightweight syntax coloring, or rich-text snippet presentation.

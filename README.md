# LumenCode

LumenCode is a structural code explorer for local source trees. It focuses on code shape rather than freeform text editing. The application reads a filesystem tree, extracts symbols from supported source files, and lets the user drill from folders to files to symbols to member detail.

LumenCode is intentionally:

- filesystem-aware
- parser-assisted
- non-editing
- non-Git-aware

## Current State

What currently works:

- Browsing a project tree with a dense three-pane explorer and a resizable lower source pane.
- Resizing the top panes and lower source pane with draggable splitters.
- Opening files into overview/detail panes with a compact left control rail and project-root display as `/`.
- Preserving explorer scroll position across expand/collapse redraws and keeping child rows visibly indented.
- Using the left control rail for back navigation, open-in-folder, open-in-editor, and settings.
- Extracting detailed symbols for PHP, JS, TS, TSX, CSS, HTML, JSON, Python, C/C++, Java, C#, Rust, and Objective-C.
- Using Tree-sitter for PHP, JS, TS, TSX, CSS, Python, Rust, Java, and C# parsing where integrated.
- Automatically collapsing the right detail pane when a selection has no unique context to show.
- Showing syntax-colored lower-pane snippets with internal highlighting rather than an external highlighting dependency.
- Showing parser-aware diagnostics for supported snippet languages, while suppressing false errors for intentionally truncated previews.
- Showing file previews in the lower pane when a file itself is selected.
- Using an explicit snippet contract (`snippetKind`, `diagnosticsMode`) so preview excerpts are handled differently from standalone parseable code.
- Surfacing CSS class matches/misses from HTML and allowing those entries to drive the lower pane.
- Inspecting CSS classes directly from CSS files with rule-level snippets.
- Improved CommonJS and Node/service-style repo analysis, including:
  - dependency extraction from `require(...)` and `import`
  - export surfacing for `module.exports`, `exports.*`, and ES modules
  - basic Express route detection
  - related-file linking
  - `package.json` scripts, entrypoint, and dependency summaries
- A functional standalone CLI tool (`lumencode-cli`) for scripted analysis and regression checks.
- A crash-isolated file-analysis path: GUI selection now shells out to `lumencode-cli --dump-file`, so parser crashes terminate the helper instead of the main app.
- CLI state now exposes the active lower-pane snippet payload, and interactive mode can select arbitrary source-context payloads for GUI-parity checks.
- A regression sweep harness at `tools/regression_sweep.py` that samples local projects, drives the CLI selection path, and validates snippet/diagnostic contracts across languages.
- Project-level summaries including file type counts and main entry-point detection.
- Installing a desktop launcher and scalable app icon through `cmake --install`.
- Stable-enough backend payloads for the current QML bindings.
- Graceful failure paths for hostile inputs:
  - directory crawl limits for recursion depth, scanned nodes, and per-folder entries
  - parser-side large-file refusal with readable summaries
  - truncated file previews instead of unbounded lower-pane loads

## Build

Typical local build:

```bash
cmake -S . -B build
cmake --build build
```

`build/` is now ignored by git and is no longer tracked in the repository.

If KF5 development packages are missing from the CMake search path, CMake will fail during configuration. That is an environment issue, not an application logic issue.

## Roadmap

Phase 1. Stabilization

- Remove remaining QML/runtime edge-case binding failures.
- Harden all selection payloads so every detail section is safe to bind.
- Reduce misleading or noisy structural output on real projects.
- Continue broad CLI-driven regression sweeps against mixed real-world projects under `/home/user/Code`.

Phase 2. Better source inspection

- Improve snippet diagnostics beyond the current lightweight parser-aware checks.
- Continue improving context selection for dependencies, routes, and quick links in the lower pane.
- Support clearer line-focused navigation and open-in-editor behavior from overview/detail items into the lower pane.
- Rehabilitate the native Tree-sitter-backed parser paths language by language, using the new crash-isolated CLI flow and minimized repro files. Keep fallback and helper isolation in place until those native paths prove stable.

Phase 3. Better usability

- Search/filter across files and symbols.
- Improve visual hierarchy and information density further without sacrificing clarity.
- Continue refining the left global control rail and settings surface.

Phase 4. Broader project understanding

- Improve Node/CommonJS and service-repo structure understanding further.
- Refine HTML/CSS class analysis to reduce noisy matches/mismatches.
- Improve project-level summaries for package metadata, tests, and API artifacts.
- Continue refining entrypoint selection on broad multi-project roots.

## Known Issues

- Some extracted structure is still shallow or misleading on real projects.
- Some languages still rely on heuristic fallback paths for parts of the overview, especially when native parser paths have been bypassed for stability.
- Some project `mainEntry` guesses are still imperfect on broad mixed-language roots.
- HTML/CSS class comparison can still be noisy on complex HTML documents.
- Syntax highlighting is currently an internal lightweight implementation, not a full external highlighter.
- Snippet diagnostics are intentionally conservative for truncated previews and are not a full linter.
- The UI is materially improved, but still needs a stabilization and polish pass.

Licensing note:

- Vendored parser sources and their retained upstream license files are documented in `THIRD_PARTY_NOTICES.md`

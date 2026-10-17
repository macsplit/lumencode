# LumenCode (Swift)

A macOS tool for source code exploration.

*A SwiftUI macOS interface for the original [`lumencode`](https://github.com/macsplit/lumencode) C++ analysis engine, with a shared Swift core and CLI harness.*

## Screenshot

![Lumencode-swift Screenshot](Screenshots/screenshot.png)

## What is in this repo

- `lumencode-swift.xcodeproj`
  A real macOS app project for day-to-day GUI development in Xcode.
- `Package.swift`
  Swift Package manifest for the Swift core, app, CLI harness and integration tests.
- `Sources/LumencodeSwiftApp`
  SwiftUI macOS interface layer.
- `Sources/LumencodeCore`
  Shared Swift models and workspace helpers used by both the GUI and CLI.
- `Sources/LumencodeNativeBridge`
  Vendored parser inputs reused by the shared engine; the earlier bridge is excluded from builds.
- `Engine`
  Pinned original engine, JSON helper, source provenance and CMake build.
- `Sources/LumencodeCLI`
  Internal harness for regression-style development and analysis validation.

## Open and run

Install the engine build prerequisites once:

```sh
brew install cmake qt@5
```

The Xcode build packages the pinned engine and its runtime libraries automatically. See [engine build and packaging](Engine/README.md), including the current macOS runtime compatibility limitation.

For the GUI app, open:

- `lumencode-swift/lumencode-swift.xcodeproj`

Then in Xcode:

1. Select the `lumencode-swift` scheme.
2. Click Run.
3. Use `Open Folder…` in the app to inspect a project.

Important:

- Do not open `Package.swift` if your goal is to run the GUI app from Xcode.
- Opening `Package.swift` gives you package executables, not the real `.app` target.

## CLI harness

The CLI is intentionally not the end-user product. It exists so backend and analysis work can be developed in a closed loop without needing manual GUI verification.

Build:

```zsh
cd /lumencode-swift
swift build
```

Build the analysis runtime before using the package executables or running tests:

```sh
python3 tools/build_engine.py
swift test
```

Run the internal harness:

```zsh
swift run lumencode-swift-cli /path/to/project
```

Dump one file analysis:

```zsh
swift run lumencode-swift-cli --dump-file /path/to/file.swift
```

## Current architecture

- GUI responsibilities:
  - project opening
  - tree navigation
  - symbol and dependency inspection
  - lower source/snippet pane
  - platform-specific presentation and interaction behavior
- Core responsibilities:
  - stable data models
  - path and snippet helpers
  - selection payload shaping
  - project scanning
- Shared engine responsibilities:
  - the original AST parsing, recovery, fallbacks and callable signatures
  - the original project index and reciprocal relations
  - web/HTTP links, routes and ranked search
  - JSON transport through a separate helper process

## Key documents

- [Engine Integration and Verification](docs/engine-integration.md) — implemented changes, current measurements and remaining gates.
- [Current Assessment and Priorities](docs/current-assessment.md) — measured backend parity, GUI responsiveness, screenshots and the plan for sharing the original engine.
- [Architecture Guide](docs/architecture-guide.md)
- [Development Plan](docs/development-plan.md)
- [Parity Guide](docs/parity-guide.md)
- [Tool Comparison Report](docs/tool-comparison-report.md)
- [Licensing and Derivation](docs/licensing-and-derivation.md)
- [Vendor Update Notes](docs/vendor-update-notes.md)

## Vendored parser policy

Tree-sitter and the original engine are pinned source inputs. Builds do not require the sibling repository or the Tree-sitter CLI.

That means:

- after installing Qt 5 and CMake, Xcode packages the engine from checked-in sources
- SwiftPM executables use the runtime built by `tools/build_engine.py`
- generated parser sources and upstream license/provenance notes must stay documented

## Current status

The Swift app now has:

- a real Xcode app target
- a working Docked macOS app
- file browsing
- symbol inspection
- dependency and route inspection hooks
- a lower source pane
- the original analysis engine in an isolated helper

- a collapsible project tree, background folder scanning, and on-demand per-file analysis so the UI stays responsive

The current engine passes all 101 shared baseline cases and matches the original CLI's single-file payloads on all 99 fixture files. The GUI includes signatures, recovery notices and project search. The 17 October follow-up adds native sidebar/input regression checks and broader original-CLI comparisons across the pinned public corpus. Current evidence and remaining release, refresh and presentation work are tracked in the [follow-up report](docs/follow-up-2026-10-17.md) and [development plan](docs/development-plan.md).


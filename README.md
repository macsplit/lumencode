# LumenCode

LumenCode is a structural code explorer with a Qt/Kirigami interface and a native
SwiftUI macOS interface. Both use the same C++ analysis sources, Tree-sitter
grammars, recovery/fallback rules, signatures, project relationships and search.
Both products are in beta.

This is an ordinary monorepo: clone once, install the platform dependencies and
use the native build tools. No submodules, recursive clone or source-sync step is
needed. The former Swift repository's main history is imported under `apps/macos`.

| Directory | Purpose |
| --- | --- |
| `engine/` | Shared Qt Core/Concurrent analysis library and JSON helper |
| `third_party/` | Checked-in Tree-sitter runtime/grammars and provenance |
| `apps/qt/` | Qt/Kirigami application and original public CLI/controller |
| `apps/macos/` | Swift package, native application and Xcode project |
| `tests/fixtures/baseline/` | Shared language/recovery/relationship contracts |
| `tools/` | Validation, runtime packaging, corpus and agent tools |
| `docs/` | Engine, Qt, macOS and migration documentation |

## Linux Qt application and public CLI

Requires CMake 3.20+, a C/C++ compiler, Qt 5.15 Core/Concurrent/Widgets/Quick/
QuickControls2, ECM and KDE Frameworks 5 Kirigami2/CoreAddons. Install the matching
QML runtime modules too. No Tree-sitter generator or npm setup is needed.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/bin/lumencode
./build/bin/lumencode-cli --dump-file /absolute/path/to/source.swift
```

For the original public CLI without KDE/QML:

```sh
cmake -S . -B build-cli -DLUMENCODE_CLI_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cli --parallel 4
```

The public CLI retains its existing interactive and agent commands and its
Widgets dependency. A helper-only build needs just Qt Core/Concurrent:

```sh
cmake -S . -B build-helper -DLUMENCODE_BUILD_QT_APP=OFF -DLUMENCODE_BUILD_CLI=OFF
cmake --build build-helper --target lumencode-engine --parallel 4
```

## Native macOS application and Swift CLI

Requires Xcode/Swift 6, CMake and Qt 5.15 Core/Concurrent. Select a Qt installation
with `QT5_PREFIX` or `tools/build_engine.py --qt /path/to/qt5`. The existing
Homebrew setup uses `/opt/homebrew/opt/qt@5`.

From the repository root:

```sh
python3 tools/build_engine.py
swift build --package-path apps/macos
swift test --package-path apps/macos
```

Open `apps/macos/lumencode-swift.xcodeproj` and select the shared `lumencode-swift`
scheme, or build directly:

```sh
xcodebuild -project apps/macos/lumencode-swift.xcodeproj \
  -scheme lumencode-swift -configuration Debug \
  -destination 'platform=macOS,arch=arm64' build
```

Xcode builds the same helper and embeds its private runtime. SwiftPM uses the
runtime built by `tools/build_engine.py`; an independent CLI distribution must
carry `EngineRuntime` beside its executable. A packaged app uses its embedded
runtime and never searches for another checkout.

## Verification

```sh
python3 tools/verify_consolidation.py
python3 tools/verify_engine_contract.py --cli build/bin/lumencode-cli --output /tmp/contracts.json
python3 tools/parity_audit.py --interface qt --cli build/bin/lumencode-cli --output /tmp/parity.json
cmake --build build --target lumencode-gui-smoke
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ./build/bin/lumencode-gui-smoke tests/fixtures/baseline
```

The macOS rendered/native audit is `tools/run_gui_audit.py --run`; pass `--build`
the Swift build directory. It requires a graphical desktop session. Corpus and
damage comparisons are described in [the engine guide](engine/README.md).

## Beta status and documentation

Consolidation preserves the accepted beta behaviour. Type-link/diagnostic UI work,
filesystem refresh completeness and the 50 ms GUI responsiveness target remain
development work. The current Mac's bundled transitive libraries also prevent
claiming macOS 14 compatibility despite the executable deployment target; use the
explicit `tools/verify_runtime.py --target-macos 14.0` gate before shipping there.

- [Shared engine and provenance](engine/README.md)
- [Documentation index](docs/README.md)
- [Migration plan](docs/migration/plan.md)
- [Migration status and evidence](docs/migration/status.md)
- [Existing macOS beta assessment](docs/macos/follow-up-2026-10-17.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

The public CLI, `tools/agent/` entry points, desktop/bundle identities and web
download assets retain their existing names. Repository consolidation does not
deploy the website, install agent hooks or publish a release.

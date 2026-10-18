# Building and verifying LumenCode

This is an ordinary monorepo: clone once, install the platform dependencies and
use the native build tools. No submodules, recursive clone or source-sync step is
needed. The former Swift repository's main history is imported under `apps/macos`.

| Directory | Purpose |
| --- | --- |
| `engine/` | Shared Qt Core/Concurrent analysis library and JSON helper |
| `third_party/` | Checked-in Tree-sitter runtime/grammars and provenance |
| `apps/qt/` | Qt/Kirigami application and original public CLI/controller |
| `apps/macos/` | Swift package, native application and Xcode project |
| `apps/windows/` | Native C#/WinUI 3 application and shared-engine process adapter |
| `tests/fixtures/baseline/` | Shared language/recovery/relationship contracts |
| `tools/` | Validation, runtime packaging, corpus and agent tools |
| `docs/` | Engine, platform, distribution and migration documentation |

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

For universal Intel/Apple Silicon downloads, use the private macOS 14 Qt kit and
the staged Developer ID signing/notarization workflow in the
[macOS distribution guide](macos/distribution.md).

## Native Windows application

Requires the .NET 10 SDK, Visual Studio C++ build tools, Windows SDK, CMake and
Qt 5.15.2 MSVC x64. From a byte-preserving checkout (`core.autocrlf=false`):

```powershell
./tools/build_windows.ps1 -Qt C:/Qt/5.15.2/msvc2019_64
./build-windows/package/LumenCode.Windows.exe
```

The output includes the .NET/WinUI runtime and shared engine; copy the whole
package folder. See [Windows build and audit instructions](../apps/windows/README.md).

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
damage comparisons are described in [the engine guide](../engine/README.md).


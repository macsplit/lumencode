# Native Windows frontend

The initial Windows frontend uses C#/WinUI 3 on .NET 10. It consumes the same
C++17/Qt 5 analysis engine and vendored grammars as Linux and macOS. There is no
C# parser, fallback implementation or independent language heuristic layer.

This is an initial development checkpoint. Read
[current progress and resume instructions](../../docs/windows/status.md) before
continuing. It is not yet a claim of full Windows product parity.

## Build and run

Requirements: Windows 11 x64 (the tested target), .NET 10 SDK, Visual Studio 2022
desktop C++ tools/v143, Windows SDK, CMake and Qt 5.15.2 MSVC x64. The build uses
the Visual Studio CMake generator; Ninja is optional. Supported Visual Studio
IDE targeting of .NET 10 requires Visual Studio 2026, but the separate .NET 10
SDK builds the C# projects from the command line alongside VS 2022 C++ tools.

Clone with byte-preserving checkout settings:

```powershell
git -c core.autocrlf=false clone https://github.com/macsplit/lumencode.git
cd lumencode
git config --local core.autocrlf false
```

From the repository root, with CMake on PATH:

```powershell
./tools/build_windows.ps1 -Qt C:/Qt/5.15.2/msvc2019_64
./build-windows/package/LumenCode.Windows.exe
# Optional initial project path:
./build-windows/package/LumenCode.Windows.exe C:/Code/MyProject
```

Pass `-Dotnet <path/to/dotnet.exe>` for a separately installed SDK. `-EngineOnly`
builds/packages the native helper and public CLI without publishing WinUI.
`-Build` and `-Output` can select other paths, including paths containing spaces.
The output folder includes the self-contained .NET/WinUI app and `EngineRuntime`
with the helper, public CLI, Qt DLLs/plugins, MSVC runtime DLLs, notices and source
provenance. Copy the **whole package folder**, not just its executable.

For development builds, `LUMENCODE_ENGINE` can select an explicit helper path;
the default is `EngineRuntime/lumencode-engine.exe` beside the application.
Normal settings and caches live under `%LOCALAPPDATA%/LumenCode`.

## Checks

Python 3 is required for the existing repository audits. Set `PYTHONUTF8=1` when
using the older scripts on Windows, and use a working Python interpreter rather
than the Windows app execution alias.

```powershell
$env:PYTHONUTF8='1'
$env:QT_QPA_PLATFORM='offscreen'
$env:QT_HASH_SEED='0'
python tools/verify_consolidation.py
python tools/parity_audit.py --interface qt --cli build-windows/package/EngineRuntime/lumencode-cli.exe --output build-windows/reports/parity.json
python tools/verify_engine_contract.py --cli build-windows/package/EngineRuntime/lumencode-cli.exe --reference-cli build-windows/package/EngineRuntime/lumencode-engine.exe --output build-windows/reports/contracts.json
dotnet run --project apps/windows/LumenCode.CoreChecks -c Release -- build-windows/package/EngineRuntime/lumencode-engine.exe build-windows/reports/core-checks.json
./tools/run_windows_gui_audit.ps1 -Output build-windows/reports/gui
```

The final command needs a logged-in interactive desktop. It briefly registers
an interactive scheduled task, runs the application with its internal audit mode,
saves app-content screenshots/JSON and removes the task in `finally`. It does
not save audit workspace settings as the user's normal workspace. It tests real
engine-backed workflows programmatically, not physical mouse/keyboard input,
Narrator, multiple monitor DPIs or clean-machine distribution.

## Source ownership

- `LumenCode.Core`: bounded, cancellable process transport and JSON presentation
  helpers, plus lazy filesystem enumeration. Shared-index requests are serialized
  because the native cache writer uses a single temporary filename per cache.
- `LumenCode.Windows`: native views, navigation, dialogs, dark UI, preview and
  explicit audit mode. Folder scanning and engine work execute away from the UI
  thread; stale selections/searches are cancelled and discarded.
- `LumenCode.CoreChecks`: executable checks for argument preservation, helper
  failure, invalid JSON, output bounds, timeout, descendant-process cancellation
  and real engine indexing under a Unicode path with CRLF sources.
- `engine` and `third_party`: the only parser/index/grammar implementation.

Do not copy the engine into this app or silently replace its analysis fields.
Retain raw engine JSON in the transport and use the existing fixtures/contracts
when evolving the presentation.

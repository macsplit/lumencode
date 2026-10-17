# Windows implementation checkpoint

Paused at the user's request on 17 October 2026. Resume this checkpoint rather
than restarting the feasibility investigation or installing another UI stack.

## Position

- Branch: `windows/native-winui`, based on canonical main
  `4d164034b992a7818b54c8ce30d831c6c875c850`.
- Architecture agreed: C#/WinUI 3/XAML frontend in `apps/windows`, .NET 10,
  existing C++17/Qt Core/Concurrent helper and canonical vendored grammars.
- The shared engine build/parity gate is complete on this Windows VM.
- The initial frontend compiles and publishes as a self-contained folder.
- GUI runtime acceptance is not complete; see validation below. This branch is
  a development checkpoint, not a Windows release or a full parity claim.

## Completed and checked

| Check on Windows 11 x64 | Result |
|---|---|
| Shared engine and public CLI, MSVC Release | Built, including all pinned grammars |
| WinUI frontend, .NET 10 / stable Windows App SDK 2.5.1 | Build succeeded with zero warnings/errors |
| Self-contained .NET/WinUI package with native runtime | Published to `build-windows/package` |
| Ordinary layout and vendored provenance | All 841 input hashes pass |
| Existing public CLI baseline | 101/101 cases, 625/625 assertions, 99 invocations, no errors |
| Public CLI versus helper complete JSON contracts | 99 files, zero contract issues, zero differences |
| C# process/transport checks | 8/8 passed |
| Real worker/index/search in a Unicode path with CRLF input | Passed, including same-file call and cross-file caller |
| Final product GUI smoke | Startup failure under investigation; not passed |

Transport checks cover Unicode/quoted/empty arguments, invalid JSON, nonzero
exit, output bounds, timeout, missing engine and cancellation of child workers.
These use the same process adapter that the frontend consumes.

The earlier feasibility probes for WPF and WinUI ran successfully on the active
desktop, but those were small separate programs. Their success does not count as
acceptance of the actual frontend. The first actual packaged GUI smoke exited
with `0xc000027b` in `Microsoft.UI.Xaml.dll` before producing its report. Startup
error recording was added to make subsequent attempts diagnosable.

No Windows full public-corpus/damage sweep, direct cross-OS full-JSON comparison,
multi-DPI/accessibility checks, installer/signing or clean-machine validation has
been completed. Linux/macOS were not rebuilt locally during this checkpoint;
the existing workflow still provides Linux checks on branch pushes. Windows CI
has not been added. Do not interpret unrun checks as successful.

## Implementation included

- `tools/build_windows.ps1`: provenance gate, CMake/MSVC engine and public CLI,
  Qt/MSVC runtime deployment, notices/provenance and self-contained WinUI publish.
- `tools/run_windows_gui_audit.ps1`: bounded interactive-desktop smoke with app-only
  screenshots/JSON and temporary scheduled-task cleanup.
- `apps/windows/LumenCode.Core`: bounded async JSON process adapter, cancellation,
  per-index serialization, presentation helpers and lazy directory enumeration.
- `apps/windows/LumenCode.Windows`: initial dark project/overview/detail/preview
  layout; native folder picker, lazy file tree, index/search, symbols/calls,
  dependency/route/link/notice presentation, navigation history, refresh,
  Explorer/default-editor integration, copyable/selectable read-only preview,
  resize dividers and keyboard shortcuts. Compilation is verified; interactions
  still require the actual GUI smoke and review.
- `apps/windows/LumenCode.CoreChecks`: executable adapter/integration checks.
- Windows-specific helper executable discovery in the public Qt controller;
  explicit UTF-8 stdin in the shared helper and CLI; explicit UTF-8 decoding in
  the two Python audit tools; MSVC source-encoding options; checkout attributes
  preserving vendored and fixture bytes.

The five shared analysis module pairs and all vendored grammar/runtime files
remain unchanged. Do not fork them for Windows. The historical
`--migration-baseline` audit intentionally checks pre-migration source identity;
it is not a current development gate after deliberate CLI/controller changes.

## Machines and durable locations

Windows via `ssh windows`:

- Source: `C:\Users\leeha\lumencode`, branch `windows/native-winui`.
- Build/runtime/package/reports: `C:\Users\leeha\lumencode\build-windows`.
- Tools: `C:\Users\leeha\lumencode-tools`.
- Feasibility probes: `C:\Users\leeha\lumencode-windows-investigation`.
- Setup/build logs: `lumencode-tools/setup.log`, `engine-build.log`,
  `winui-build.log`, `publish.log`, `core-checks.log`, `gui-rebuild.log`.

Installed for this work: VS 2022 C++ tools 14.44.35207 and CMake component,
Python 3.13.15, a dedicated Python venv with aqtinstall 3.3.0/CMake 4.4.4/Ninja
1.13.2, Qt 5.15.2 MSVC2019 x64 (`qtbase` and `qttools`), and a user-local .NET
10.0.402 SDK. The venv's complete requirements are recorded in
`lumencode-tools/python-requirements.txt`. Visual Studio 2026 was not installed.

The original Mac and Linux product checkouts remain untouched. Local editing for
this branch used `/tmp/lumencode-windows-implementation` on the Mac; it is a
disposable staging checkout, not a required dependency. The Windows checkout and
the pushed Git branch are the resume sources. The user's SSH warning suppression
was tested successfully; no SSH configuration changes were made by this work.

## Resume

1. Read this file and inspect `git status`, then fetch `origin` and use the existing
   `windows/native-winui` branch. Preserve any newer user edits.
2. Finish diagnosing the actual app startup/GUI smoke. Check the latest GUI JSON
   and `startup-error.txt`, not only Event Viewer. Keep the diagnostic mode and
   app-only capture path; do not count the feasibility probe as product validation.
3. Once the app launches, run the existing smoke against the actual packaged
   runtime, inspect the dark and minimum-window images, and address any failures.
4. Then decide the next bounded implementation scope: visual/presentation polish
   (including syntax coloring), actual keyboard/pointer/accessibility/DPI testing,
   cross-OS corpus comparisons, Windows CI and relocated/clean-machine packaging.
   Do not start all of those together merely because the initial engine gate passed.

Commands on the current VM (from the repository root):

```powershell
$tools = "$env:USERPROFILE\lumencode-tools"
$env:PATH = "$tools\python\Scripts;$tools\dotnet;" + $env:PATH
$env:DOTNET_ROOT = "$tools\dotnet"
$env:PYTHONUTF8 = '1'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_HASH_SEED = '0'
./tools/build_windows.ps1 -Dotnet "$tools\dotnet\dotnet.exe"
python tools/verify_consolidation.py
python tools/parity_audit.py --interface qt --cli build-windows/package/EngineRuntime/lumencode-cli.exe --output build-windows/reports/parity.json
python tools/verify_engine_contract.py --cli build-windows/package/EngineRuntime/lumencode-cli.exe --reference-cli build-windows/package/EngineRuntime/lumencode-engine.exe --output build-windows/reports/contracts.json
dotnet run --project apps/windows/LumenCode.CoreChecks -c Release -- build-windows/package/EngineRuntime/lumencode-engine.exe build-windows/reports/core-checks.json
./tools/run_windows_gui_audit.ps1 -Output build-windows/reports/gui-resume
```

The scheduled-task runner must execute as the logged-in desktop user. All known
temporary audit tasks are removed by the runner in `finally`; confirm cleanup
after any interrupted run. No release, installer, signing or merge is authorized
by this pause checkpoint.

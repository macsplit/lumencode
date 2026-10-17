# Windows development status

Updated 17 October 2026 after the first responsiveness pass. The native Windows
application opens normally and now displays source before waiting for analysis. Continue this branch rather than
restarting the feasibility investigation or installing another UI stack.

## Position

- Branch: `windows/native-winui`, based on canonical main
  `4d164034b992a7818b54c8ce30d831c6c875c850`.
- Initial checkpoint: `f752dd68c7a14cb261e89d689a2f57b30c0012b9`.
  Startup fix: `79c5184c1ceb21fbc78b175cef90e3d773816f56`. The current
  follow-up improves source responsiveness without changing the shared engine.
- Architecture agreed: C#/WinUI 3/XAML frontend in `apps/windows`, .NET 10,
  existing C++17/Qt Core/Concurrent helper and canonical vendored grammars.
- The shared engine build/parity gate is complete on this Windows VM.
- The initial frontend compiles and publishes as a self-contained folder.
- The actual published GUI passes its initial functional smoke in both the normal
  package and a fresh package directory whose path contains spaces. Normal launch
  without the internal audit mode also opens a visible, responsive window.
- This remains an early Windows frontend, not a release or full parity claim.

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
| Actual published GUI smoke | 32/32 checks pass, including cancellation and a 2,000-symbol source file |
| Package relocation in a path with spaces | 32/32 checks pass; initial fresh-folder startup check also passed 26/26 |
| Normal executable launch with an initial file argument | Visible, responding window after five seconds; closed gracefully |

Transport checks cover Unicode/quoted/empty arguments, invalid JSON, nonzero
exit, output bounds, timeout, missing engine and cancellation of child workers.
These use the same process adapter that the frontend consumes.

The GUI checks exercise the actual packaged frontend and bundled engine: tree,
dark theme, analysis/preview, index readiness, 12 fixture file selections, symbol
details/calls, history, asynchronous search/clear, selectable read-only source
and minimum-window layout. The expanded checks also observe source while engine
analysis is still pending, exercise cancellation during rapid navigation, confirm
warm-cache reuse and display all 2,000 symbols from a generated JavaScript file. Both final app-content screenshots were visually
inspected. They are in [evidence](evidence/); the earlier feasibility mockups are
not counted as product acceptance. The normal-launch check restored the user's
previous workspace settings and closed only the process it started.

### Startup fix and initial measurements

The original `0xc000027b`/generic `XamlParseException` concealed a packaging
failure. Capturing `Exception.Data` revealed `Cannot locate resource from
'ms-appx:///MainWindow.xaml'`. The build directory contained the application PRI
and XBF files, but `dotnet publish` omitted them. The WinUI project now adds them
to `ResolvedFileToPublish`, including direct CLI publication. See the corresponding
[Windows App SDK issue](https://github.com/microsoft/WindowsAppSDK/issues/6720).
Removing the custom divider controls did not change the error; those controls
were restored and are present in the working build.

The GUI smoke originally started a second search directly after setting the text
box. The dispatched `TextChanged` event could cancel that request and make the
test check results too early. The test now drives and awaits the real event path.
Clearing search also resets its status label. Visual review caught preview
clipping at minimum size; proportional default row sizing and a resize clamp
now preserve the content area, with an explicit smoke assertion.

Recorded timings on this VM are diagnostic samples, not performance acceptance:

| Final sample | Normal package | Fresh package with spaces |
|---|---:|---:|
| File selection min / median / max | 25 / 135 / 157 ms | 25 / 129 / 187 ms |
| UI timer p95 gap | 107 ms | 78 ms |
| UI timer maximum gap | 164 ms | 173 ms |

Selections measure completion of engine/data/control updates, not presentation
of a rendered frame. The nominal 16 ms dispatcher timer includes startup,
indexing and layout activity; VM scheduling and timer coalescing also affect it.
These results do not establish the optional 50 ms responsiveness goal. Profiling
and improving UI latency remains a priority.

### Responsiveness follow-up

Source loading/decoding and analysis presentation preparation now run on workers.
Source is applied before the engine request, so process startup and project
analysis no longer delay reading the file. Overview JSON flattening, row formatting
and the cache-size check also leave the dispatcher. Cache writes happen only
after checking selection cancellation and project generation, preventing older
results from populating a newer workspace cache.

A diagnostic before/after run on the same VM used the same 12 fixture selections:

| Stage, min / median / max | Before | After |
|---|---:|---:|
| Source applied | 20 / 134 / 212 ms | 5 / 7 / 18 ms |
| Analysis available | 12 / 120 / 202 ms | 15 / 120 / 130 ms |
| Explicit UI application work | 4 / 8 / 17 ms | 2 / 3 / 7 ms |
| Whole selection operation | 20 / 135 / 213 ms | 16 / 121 / 131 ms |

Raw measurements are [latency-before.json](evidence/latency-before.json) and
[latency-after.json](evidence/latency-after.json). The baseline contains only
instrumentation over the startup checkpoint; the after run also includes the
expanded checks. These are single diagnostic runs, not statistical benchmarks.
The source timestamp records RichEditBox text assignment, not a rendered frame;
UI application excludes later layout/rendering and initial selection clearing.

The added 2,000-function JavaScript case shows the remaining limit clearly:
source assignment took 138 ms, analysis was available at 659 ms and selection
completed at 671 ms. Explicit UI work was 104 ms, primarily the larger text
assignment. The dispatcher timer still showed a 93 ms p95 gap and 160 ms maximum
in the expanded run (101 / 155 ms in the baseline). The heartbeat workloads differ
because the after run includes the new stress/cancellation checks. Do not claim
a general 50 ms target or a measured frame-rate improvement from these samples.
A second run against the usual `build-windows/package` executable also passed
32/32 checks; its [gui.json](evidence/gui.json) records a 124 ms p95 dispatcher
gap and 191 ms maximum, reinforcing that overall latency remains unresolved.
Large-preview text rendering remains a profiling priority; syntax colouring has
not been added in this pass.

The audit now waits for the initial index-triggered file refresh, which can
supersede its first selection. This fixes a test timing assumption exposed by the
new asynchronous source path. Dark/default and minimum-window screenshots were
inspected again; no layout regression was observed at this VM's scale.

No Windows full public-corpus/damage sweep, direct cross-OS full-JSON comparison,
multi-DPI/accessibility checks, installer/signing or clean-machine validation has
been completed. Linux/macOS were not rebuilt locally during this checkpoint;
the existing workflow still provides Linux checks on branch pushes. The initial
checkpoint's [Linux CI passed](https://github.com/macsplit/lumencode/actions/runs/39038433627).
The startup-fix checkpoint's [Linux CI also passed](https://github.com/macsplit/lumencode/actions/runs/39047973149).
Windows CI has not been added. Do not interpret unrun checks as successful.

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
  resize dividers and keyboard shortcuts. The programmatic GUI workflow passes;
  physical input and accessibility still need testing.
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
  `winui-build.log`, `publish.log`, `core-checks.log`, `gui-rebuild.log`,
  `fresh-publish.log`.
- Current runnable app:
  `C:\Users\leeha\lumencode\build-windows\package\LumenCode.Windows.exe`.
- Earlier verified relocation (startup checkpoint):
  `C:\Users\leeha\lumencode\build-windows\fresh verified package`.
- Current responsiveness relocation: `C:\Users\leeha\lumencode\build-windows\latency-after package`.
- Final raw GUI outputs: `build-windows/reports/latency-after` and
  `build-windows/reports/latency-final`; normal launch: `reports/normal-launch.json`.
  The old `startup-error.txt` in a reused audit folder may be historical; use the
  newest `gui.json` result and file timestamps. Both final GUI reports pass.

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

## Next work

1. Read this file and inspect `git status`, then fetch `origin` and use the existing
   `windows/native-winui` branch. Preserve any newer user edits.
2. Continue profiling large-preview text assignment and rendered/input latency.
   Small-file source display is decoupled from analysis; the shared engine is
   unchanged. Stage timings in the GUI report are diagnostic, not frame timings.
3. Test actual keyboard/pointer navigation, folder dialogs, pane dragging,
   accessibility and multiple DPIs. The current smoke is programmatic and does
   not establish those areas. Polish presentation, including source coloring.
4. Add Windows CI, cross-OS corpus comparisons and clean-machine/installer
   validation. Same-VM fresh-folder publication has passed; a machine with no
   developer/runtime setup has not yet been tested.

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
after any interrupted run. No release, installer or signing has been published;
the development branch remains unmerged.

# Windows port feasibility

Investigated 17 October 2026 against monorepo main `4d164034b992a7818b54c8ce30d831c6c875c850`.

This document records the initial investigation. Implementation has since begun;
see [the current checkpoint](status.md) for build results and remaining work.

## Recommendation

A native Windows frontend is feasible. Use **C# and WinUI 3/XAML**, with the current stable Windows App SDK and .NET 10 LTS for implementation. Keep the existing C++17/Qt Core/Concurrent analysis helper and the canonical Tree-sitter sources. Put the frontend in `apps/windows` in this monorepo. Communicate through the helper's existing JSON interface, following the architectural boundary used by the SwiftUI app.

This is an engineering recommendation based on source inspection, current official documentation, and native UI probes on the Windows VM. It is not a claim that the complete product has been ported or that its engine has already compiled on Windows. **The first implementation gate is a Windows engine build and parity run.**

WPF with C#/XAML remains a sound alternative. Choose it if built-in desktop controls and simpler runtime deployment outweigh WinUI's closer alignment with current Windows appearance. Both options preserve exactly the same shared engine. The available evidence does not justify claiming either framework is universally faster or more reliable.

## Machine and checkout

- SSH alias: `windows`; account `PC\leeha`; Windows 11 Pro x64, build `10.0.26200`.
- Fresh full Git checkout: `C:\Users\leeha\lumencode`, tracking canonical `origin/main` at the revision above.
- Disposable experiments and logs: `C:\Users\leeha\lumencode-windows-investigation`.
- Installed: Visual Studio Professional 2022 17.14, .NET SDKs 8.0.418 and 9.0.318, Windows SDK 10.0.26100.0, Git, Windows App Runtime packages including 2.5.1.
- Missing from the inspected installation: MSVC C/C++ tools, CMake, Qt development files. `python.exe` on PATH points to the Windows app execution alias; a working Python installation was not established.
- About 259 GB free space was available when inspected.

Restoring Microsoft NuGet packages was sufficient to build the two C# UI probes. No Visual Studio workloads, system SDKs, Qt installation, or production application were installed.

## What was actually tested

Both probes built in Release with zero warnings/errors using the existing .NET 9 SDK. WinUI used stable `Microsoft.WindowsAppSDK` 2.5.1, x64, `WindowsPackageType=None` and `WindowsAppSDKSelfContained=true`. The .NET runtime itself remained framework-dependent. This demonstrates an unpackaged developer build, not clean-machine distribution.

Both ran in the active Windows desktop session (session 1) through temporary interactive scheduled tasks. The tasks were removed after execution and both processes exited. Captures contain only each probe's content area, not the user's desktop.

Each probe displayed a three-column explorer with a code preview, 10,000 synthetic file rows and 1,000 symbol rows. It performed 60 programmatic symbol-selection/code-preview updates at approximately 50 ms intervals, sampled a nominal 16 ms UI timer, checked read-only text selection and saved an application-only PNG.

Final recorded run:

| Measurement | WPF | WinUI 3 |
|---|---:|---:|
| File rows with realized containers | 30 / 10,000 | 69 / 10,000 |
| Programmatic selection/preview updates | 60 | 60 |
| Time to settled loaded handler, including deliberate 200 ms delay | 468 ms | 748 ms |
| UI timer p95 gap | 42.7 ms | 60.2 ms |
| UI timer maximum gap | 45.8 ms | 87.5 ms |
| Read-only preview / selected text check | Passed | Passed |
| Dark rendering visually inspected | Yes | Yes |

These are feasibility samples, **not a controlled framework benchmark**: startup work, styling, controls, item models and timer scheduling differ; JIT/cache state and VM scheduling varied between runs. Initial WPF startup also produced a roughly 151 ms timer gap in an earlier run. No real engine work was attached. Neither frontend has demonstrated a product responsiveness budget. Hierarchical expansion, mouse/keyboard automation, drag-resizing, Narrator, high contrast, multiple DPIs, long source files, editor integration and clean-machine deployment remain untested.

The first WinUI preview attempt threw `UnauthorizedAccessException` because it called `Document.SetText` while `IsReadOnly=true`. The corrected probe clears the flag only during the synchronous document update and restores it in `finally`; subsequent runs succeeded. This is a control API constraint to handle in the frontend, not evidence that the engine or framework is unsuitable.

Initial content-only captures had transparent backgrounds, which distorted their appearance in image viewers. Final captures use opaque backgrounds. WPF's Fluent theme was applied at application scope. Both final captures show legible dark controls and selectable monospaced preview text. These disposable layouts are not proposed product designs.

Evidence in the investigation directory: `environment.json`, `wpf-build.log`, `winui-build.log`, `wpf-controls-build.log`, `winui-controls-build.log`, `wpf-ui.json`, `winui-ui.json`, `wpf-dark.png`, `winui-dark.png`, and both probe projects. The initial corrected failure is retained separately as `initial-winui-readonly-error.txt`.

## Framework comparison for this product

| Requirement | C# / WinUI 3 | C# / WPF |
|---|---|---|
| Windows appearance and idioms | Current Fluent controls, command bars, menus and keyboard accelerators | Native Windows desktop framework; modern Fluent styling available, still being improved |
| File hierarchy and large results | TreeView and virtualized ListView; both need careful data loading and templates | TreeView/ListBox/ListView; explicitly enable tree virtualization and recycling |
| Three columns and bottom preview | Grid plus a tested Community Toolkit GridSplitter or a small dedicated splitter | Grid plus built-in GridSplitter |
| Read-only code preview | RichEditBox supports selection and range formatting; respect its read-only update behavior | TextBox/RichTextBox supports selection; richer code viewers can be evaluated later |
| JSON process adapter | Standard .NET Process, cancellation and System.Text.Json | Same implementation |
| Distribution | Windows App SDK plus .NET plus shared engine runtime; unpackaged and MSIX options | .NET plus shared engine runtime; folder/installer and MSIX options |
| Practical assessment | Recommended for the preferred Windows-native product direction | Credible fallback with a strong desktop control fit |

C++/WinRT with WinUI would preserve the engine language but introduce additional UI interop and lifetime work without reducing parsing duplication: the JSON process boundary already preserves that reuse. C# is the simpler presentation language here. Qt/Kirigami could reuse the Linux UI, but entails a Qt5/KF5 dependency choice and Windows adaptation; theoretical portability is not a sufficient reason to prefer it. Electron/Tauri, Avalonia and MAUI add a different UI stack without meeting the preference for Windows-native controls as directly. None offers a demonstrated benefit for this scope.

The prototype used .NET 9 only because it is installed. .NET 8/9 support ends on 10 November 2026, so a new port should target .NET 10 LTS. Its SDK can be installed separately for command-line builds; supported Visual Studio IDE targeting of .NET 10 requires Visual Studio 2026. This need not force an engine compiler upgrade at the same time.

## Engine reuse and limits

`engine/helper/main.cpp` already exposes:

| Operation | Existing helper invocation |
|---|---|
| File analysis | `--dump-file <path>` |
| File with cross-file relations | `--root <root> --path <path>` |
| Build/refresh project index | `--root <root> --build-index` |
| Search | `--root <root> --search <query> --kind <kinds>` |
| Batched indexing facts | `--index-facts`, paths on stdin |

`--cache <path>` is available for an explicit index cache location. These are executable operations, not a resident server or an HTTP API. The C# adapter should retain raw engine JSON and add presentation models only. It must not reimplement language detection, AST extraction, parser repair/fallback, confidence, import resolution or cross-file heuristics. New transport operations, if needed for feature parity, belong in the shared helper and should be additive and tested.

The shared library uses Qt Core/Concurrent and vendored C grammars. Helper-only CMake configuration avoids Qt Widgets, Qt Quick and KDE entirely. The vendored Tree-sitter runtime contains Windows/MSVC portability branches, and source inspection found no direct POSIX dependency in the five shared C++ modules. This supports feasibility; compilation of all pinned scanners/grammars with MSVC is still unverified.

The existing `tools/build_engine.py` and `tools/verify_runtime.py` package/check Mach-O binaries with Apple tooling. They cannot package Windows binaries unchanged. Add Windows packaging alongside them, collecting the helper, required Qt DLLs and compiler runtime, with source provenance and notices. Do not copy the Mac runtime or rely on development-machine PATH entries.

Specific issues to validate early:

0. **Preserve vendored bytes:** the VM's global `core.autocrlf=true` setting converted tracked text files on checkout and caused 237 provenance SHA-256 mismatches. This was caught during the investigation, not an upstream source change. The fresh checkout was configured locally with `core.autocrlf=false` and its clean tracked files restored from a binary-safe Git archive. All **841 vendored input hashes then matched**. Future Windows setup should clone with `git -c core.autocrlf=false clone ...` and persist that setting locally, or add a carefully reviewed repository attributes policy. Do not change the manifest hashes to accommodate checkout conversion.
1. **UTF-8 worker input:** `ProjectIndex::runHelperBatch` writes UTF-8 paths, but `QTextStream input(stdin)` uses the locale codec by default in Qt 5. On a non-UTF-8 Windows locale this can corrupt non-ASCII paths. Make the transport explicitly UTF-8 and test actual child-worker indexing, not only `--dump-file`.
2. **Compiler and source encoding:** build every vendored parser/scanner unchanged first; use suitable MSVC C11/C++17 and UTF-8 compilation settings. Do not regenerate grammars or replace heuristics to sidestep a build issue.
3. **Paths and filesystem semantics:** drive letters, separators, case differences, Unicode, UNC paths, long paths, junctions, inaccessible folders and CRLF files need Windows fixtures. Cross-platform JSON comparisons must normalize only platform-specific paths/metadata and unordered collections, preserving semantic fields.
4. **Executable discovery:** the Qt controller checks an extensionless `lumencode-cli` with `QFileInfo::exists`; tools also assume Unix-style output names/locations. Windows `.exe` and multi-configuration output paths need handling if the public CLI/agent tools are included. The engine helper uses its actual application executable path, avoiding that specific controller lookup.
5. **Cancellation:** cancelling `WaitForExitAsync` alone does not stop a helper. Kill and reap the process tree on cancellation/timeout; test descendants created during indexing. Bound stdout/stderr, drain both without deadlocks, pass arguments with `ProcessStartInfo.ArgumentList`, and explicitly use UTF-8.
6. **Responsiveness:** folder scanning, process launch, JSON decoding, filtering and syntax highlighting must stay off the UI thread. Use request generations to discard stale results, cancellation, debounced search and a bounded cache. The current Mac implementation is a useful behavior reference. Do not assume the UI framework solves synchronous work or repeated index loading.

## Prioritized implementation sequence

1. **Prove the shared engine on Windows.** Add MSVC desktop C++ tools, CMake/Ninja and a pinned Qt 5 development kit; install real Python for audits. Build helper-only from the fresh checkout, fix transport/platform issues, then build the optional public CLI. Run the existing 99-file contract checks and 101-case/625-assertion baseline through suitable existing or thin helper adapters. Compare Windows results with the canonical Linux/Mac engine after narrowly defined normalization. Include Unicode paths, CRLF, damaged source, imports and cross-file calls. Exit criterion: demonstrated language/heuristic parity, not just a successful executable build.
2. **Add the .NET 10 process adapter and one complete native workflow.** Create `apps/windows`, open a folder, browse a virtualized hierarchy, select a file, display symbols and code, inspect a member and follow a relation. Test cancellation, missing/crashed helpers, bounded output, stale responses and paths with spaces. Use the existing engine JSON as the contract.
3. **Complete the Windows interaction design.** Preserve the product's project/overview/detail/code organization, dark default and familiar information hierarchy; use Windows menus, shortcuts, folder dialogs, Explorer reveal, default-editor launch, accessible labels and focus behavior. Add resizing, search, history, refresh, confidence/notices, routes, dependencies and asset links. Share fixtures and behavior specifications across frontends.
4. **Validate actual GUI responsiveness and visuals.** Measure folder open, cold/warm selection, search and cancellation with real projects. Capture app-only dark screenshots at minimum window size and 100/150/200% DPI; check keyboard traversal, high contrast/Narrator and text selection. Record p50/p95/max and UI stalls separately from engine latency. Establish acceptance budgets before declaring parity.
5. **Make deployment and CI reproducible.** Pin SDK/package/toolchain versions; add Windows CI for engine builds, contracts and frontend builds. Use an interactive Windows test environment for visual acceptance. Start with an unpackaged release folder, verify relocation and clean-machine launch without developer tools, then add a signed installer or MSIX when distribution warrants it. Keep all shared engine and grammar sources canonical in this repository.

## Official sources consulted

- [Microsoft: choosing a Windows development path](https://learn.microsoft.com/windows/apps/get-started/) — WinUI/C# or C++ and native/cross-platform choices.
- [Windows App SDK release channels](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/release-channels) — stable 2.5.1 verified against NuGet during the probe.
- [Unpackaged WinUI deployment](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/unpackage-winui-app) — runtime and deployment considerations.
- [WinUI TreeView](https://learn.microsoft.com/en-us/windows/apps/design/controls/tree-view) and [ListView performance](https://learn.microsoft.com/en-us/windows/apps/develop/performance/optimize-gridview-and-listview) — hierarchy and virtualization guidance.
- [Community Toolkit GridSplitter](https://learn.microsoft.com/en-us/dotnet/communitytoolkit/windows/sizers/gridsplitter) and [WPF GridSplitter](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/controls/gridsplitter) — pane resizing choices.
- [WPF .NET 9 Fluent theme](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/whats-new/net90) and [WPF .NET 10 changes](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/whats-new/net100) — dark styling and continuing Fluent work.
- [.NET support policy](https://dotnet.microsoft.com/en-us/platform/support/policy/dotnet-core) and [Windows .NET installation/IDE compatibility](https://learn.microsoft.com/en-us/dotnet/core/install/windows) — target/runtime lifecycle and SDK setup.
- [Qt 5 QTextStream](https://doc.qt.io/archives/qt-5.15/qtextstream.html) — locale-default encoding.
- [MSVC binary compatibility](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017?view=msvc-170) — compatibility constraints when selecting a Qt binary kit and compiler.

The recommendation and risk assessment are project-specific judgments. Microsoft documentation establishes framework capabilities, not a guarantee of LumenCode performance or parity.

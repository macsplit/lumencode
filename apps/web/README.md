# Browser prototype

A local-first, read-only browser interface around the canonical C++ engine.
The initial target is desktop Chrome/Edge. This is a technical demonstrator, not
a replacement for the native applications.

## What works

- Shared C++ analysis and all vendored grammars compiled into one WebAssembly module.
- Analysis and project indexing in a dedicated worker, using the same recovery,
  heuristics, facts and graph construction as native consumers.
- Read-only folder selection, a directory-import fallback, example project,
  source/symbol inspection, relationships and symbol search.
- Refresh of a granted directory; a fresh worker clears source/path caches.
- File-backed WORKERFS mapping preserves relative hierarchy and modification
  times without eagerly copying every file's contents into WebAssembly memory.
- Service-worker precaching of the entire app and engine for offline startup.
  Cache versions derive from asset hashes; the UI reports storage persistence.

The small interface and worker are plain JavaScript/HTML/CSS to keep the first
prototype independent of a frontend framework. A TypeScript/component-tooling
decision remains open for a fuller interface.

## Toolchain setup

Validated on Kubuntu with Qt 6.8.3 single-threaded WebAssembly, matching Linux
Qt host tools, Emscripten 3.1.56, CMake, Ninja and Python 3.12. Native builds
continue to use Qt 5.15. The web option defaults off.

Choose a tools directory outside the checkout; commands below use
`/home/user/Code/Coding/lumencode-wasm-tools` as the example. The Qt download
tool verifies package checksums. No shell startup files need modifying.

```sh
python3 -m venv /home/user/Code/Coding/lumencode-wasm-tools
/home/user/Code/Coding/lumencode-wasm-tools/bin/pip install aqtinstall
/home/user/Code/Coding/lumencode-wasm-tools/bin/aqt install-qt all_os wasm 6.8.3 wasm_singlethread --archives qtbase -O /home/user/Code/Coding/lumencode-wasm-tools/Qt
/home/user/Code/Coding/lumencode-wasm-tools/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 --archives qtbase -O /home/user/Code/Coding/lumencode-wasm-tools/Qt
git clone https://github.com/emscripten-core/emsdk.git /home/user/Code/Coding/lumencode-wasm-tools/emsdk
/home/user/Code/Coding/lumencode-wasm-tools/emsdk/emsdk install 3.1.56
/home/user/Code/Coding/lumencode-wasm-tools/emsdk/emsdk activate 3.1.56
```

From the repository root:

```sh
python3 tools/build_web.py \
  --build /home/user/Code/Coding/lumencode-wasm-build \
  --qt /home/user/Code/Coding/lumencode-wasm-tools/Qt/6.8.3/wasm_singlethread \
  --qt-host /home/user/Code/Coding/lumencode-wasm-tools/Qt/6.8.3/gcc_64 \
  --emsdk /home/user/Code/Coding/lumencode-wasm-tools/emsdk
```

The script compiles, stages assets, creates a content-derived service-worker
version and records sizes, hashes and source provenance in `web/build-report.json`
inside the build directory. After interface-only edits, use the same `--build`
with `--stage-only`. Rebuild normally after any C++ change.

## Run locally

```sh
python3 -m http.server 8765 --bind 127.0.0.1 \
  --directory /home/user/Code/Coding/lumencode-wasm-build/web
```

Open `http://localhost:8765/` in desktop Chrome or Edge. Choose **Open folder**
and grant read access, or choose **Try example**. **Build relationships** enables
cross-file results and symbol search. Press Enter in search to query the index.
**Import folder** creates a snapshot; import again to see external changes.

Wait for **Ready for offline use** before disconnecting. Browser-private caches
and permissions belong to that browser profile and origin, including its port.
The local server is only for delivering static application files; the engine
does not call it to analyse source. Public hosting would use HTTPS.

When using the browser on another machine, serve the files on that machine or
forward a local port to the remote server using SSH. Opening an HTTP LAN address
directly does not provide the secure context required for all features.

## Verification

Build the native reference helper from the same checkout:

```sh
cmake -S . -B /home/user/Code/Coding/lumencode-wasm-native-check -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLUMENCODE_BUILD_QT_APP=OFF \
  -DLUMENCODE_BUILD_CLI=OFF -DLUMENCODE_BUILD_ENGINE_HELPER=ON
cmake --build /home/user/Code/Coding/lumencode-wasm-native-check --parallel 6
```

Use a Python environment with Playwright installed and an available Chromium:

```sh
python tools/check_web_prototype.py \
  --web-root /home/user/Code/Coding/lumencode-wasm-build/web \
  --native-helper /home/user/Code/Coding/lumencode-wasm-native-check/bin/lumencode-engine \
  --chromium /snap/bin/chromium \
  --output /tmp/lumencode-pwa-validation
```

The checker compares complete JSON for 102 unique baseline fixture files, plus
three selected indexed analyses. Only the physical project-root prefix is
normalized; array ordering, recovery metadata and unresolved paths remain visible.
It also exercises the UI, source-file import, real OPFS directory handles through
a test-supplied picker, refresh, layouts and indexing responsiveness. It closes
the browser, stops the origin server, relaunches the same profile offline and
analyses newly generated source. The operating-system folder picker and its
permission dialog are **not** covered by the injected-picker test.

## Current limits and next work

- Directory handles and indexes are session-only; reopening projects is manual.
- The UI bounds folder enumeration to 20,000 files after exclusions. The shared
  engine retains its own indexed file-size and language selection rules.
- No filesystem watcher, background indexing, persistent index or editable source.
- Cancellation/timeout terminates and restarts the worker, discarding session
  analysis. Indexing currently reports completion rather than per-file progress.
- Case-sensitive virtual paths and missing symlink semantics need broader parity
  coverage, especially for projects from case-insensitive host filesystems.
- The fixture measurements are not large-project memory/performance guarantees.
- Full corpus/damage/index comparisons, browser matrix testing, native permission
  prompts, interrupted-update/storage-loss checks and user acceptance remain.
- The application has not been deployed to the public website. Before publishing
  a distributable bundle, include the linked Qt/runtime license notices and source
  availability alongside the existing project and grammar notices.

See the [feasibility roadmap](../../docs/web/offline-pwa-feasibility.md) and
[prototype status](../../docs/web/offline-pwa-status.md).

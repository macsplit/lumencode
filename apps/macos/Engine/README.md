# Shared LumenCode engine

The Swift app and CLI use the original LumenCode C++ engine through a separate JSON helper. Language extraction, signatures, AST repair, fallbacks, project relations and search come from the pinned upstream sources. Qt stays inside the helper.

`Upstream/src` is copied without edits from revision `48cf4db8042d51cfe67c2344e4d7a3824576517f`. `upstream.json` records hashes for the engine, baseline fixtures, original contract validator and parser inputs. Existing grammars are reused from `Sources/LumencodeNativeBridge/Vendor`; C++ and Go grammar sources are included in the snapshot. The old handwritten bridge is no longer compiled by SwiftPM or Xcode. Legacy Swift resolver files are not used by the app or CLI.

## Build

Install CMake and Qt 5 Core/Concurrent, then run:

```sh
brew install cmake qt@5
python3 tools/build_engine.py
swift build
swift test
```

Qt 5 is deprecated by Homebrew. This integration deliberately retains the original's Qt version; moving the shared engine to Qt 6 needs a separate compatibility pass.

`QT5_PREFIX` or `--qt` selects another Qt 5 installation. No sibling checkout or network access is required by the build after dependencies are installed. Xcode's **Build Analysis Engine** phase runs the same tool and embeds the runtime in the app's Resources.

The packaged runtime has `bin/lumencode-engine`, private dynamic libraries in `lib`, license files, source provenance, dependency receipts and `minimum-macos.json`. Library load paths are rewritten relative to the runtime and the copied binaries are signed ad hoc. Developer ID signing, notarization and a clean-machine release test remain release work.

The engine executable targets macOS 14, but the installed Homebrew transitive libraries currently require macOS 27. The manifest records each library's minimum version, and Swift reports an explicit incompatibility instead of launching an unsupported runtime. Shipping to macOS 14 requires building or obtaining compatible dependencies and repeating the relocation test. Tests on this Mac establish current-machine integration, not macOS 14 compatibility.

Swift finds an embedded runtime first. A packaged app never falls back to a development checkout. Development executables can find `.build/EngineRuntime` or use an explicit `LUMENCODE_ENGINE=/absolute/path/to/lumencode-engine` override. To distribute a standalone CLI, copy `EngineRuntime` beside its executable.

## Transport

The small `main.cpp` implements transport and argument dispatch only:

```sh
.build/EngineRuntime/bin/lumencode-engine --dump-file /path/to/file
.build/EngineRuntime/bin/lumencode-engine --root /path/to/project --path /path/to/file
.build/EngineRuntime/bin/lumencode-engine --root /path/to/project --build-index
.build/EngineRuntime/bin/lumencode-engine --root /path/to/project --search greet --kind function,method
```

`--index-facts` accepts file paths on stdin for the original index's crash-isolated batch workers. `--cache` optionally sets the original index cache location. The original `ProjectIndex` owns binding, ambiguity, cache and relation policies; the wrapper does not infer relations.

`NativeAnalyzer` reads the helper's JSON, bounds runtime and output size, and projects it into Swift models. It retains the original payload for CLI serialization, including fields the current UI does not display. Stable UI IDs derive from source identity. Display defaults and signature formatting are the adapter's responsibility; language heuristics are not.

## Verify

```sh
python3 tools/build_engine.py --verify-only
python3 tools/parity_audit.py --cli .build/debug/lumencode-swift-cli --original Engine/Upstream --output /tmp/parity.json
python3 tools/verify_engine_contract.py --cli .build/debug/lumencode-swift-cli --output /tmp/contracts.json
python3 tools/verify_runtime.py --runtime .build/EngineRuntime --output /tmp/runtime.json
python3 tools/verify_runtime.py --runtime .build/EngineRuntime --target-macos 14.0 --output /tmp/runtime-macos14.json
```

The explicit macOS 14 gate currently fails on the installed transitive libraries,
even though relocation succeeds. It must pass with compatible libraries before
shipping a macOS 14 build.

The upstream public-corpus and damage tools are also pinned without edits. To
compare complete payloads against an original CLI built from the pinned revision:

```sh
python3 Engine/Upstream/tools/fetch_corpus.py --dest /tmp/lumencode-public-corpus
python3 tools/upstream_resilience.py corpus --cli /tmp/lumencode-shared-build/debug/lumencode-swift-cli --reference-cli /path/to/original/lumencode-cli --root /tmp/lumencode-public-corpus --output /tmp/corpus.json
python3 tools/upstream_resilience.py damage --cli /tmp/lumencode-shared-build/debug/lumencode-swift-cli --reference-cli /path/to/original/lumencode-cli --root /tmp/lumencode-public-corpus --output /tmp/damage.json
```

The adapter substitutes the CLI invocation only; upstream file discovery,
contract validation, mutation generation and retention scoring remain unchanged.
It compares all clean and mutated dump payloads on the same paths with
`QT_HASH_SEED=0`, checks the 27 corpus revisions and snapshots executables/runtime
so concurrent builds cannot invalidate running signed binaries. The adjacent
`*-upstream.json` report retains upstream's own results.

`verify_engine_contract.py --reference-cli /path/to/original/lumencode-cli` additionally compares live single-file output from both executables on the same fixtures. It fixes `QT_HASH_SEED=0` for both processes because upstream QHash iteration affects the order of some relation/link arrays. It does not remove fields or relax semantic comparisons.

## Update

Check out a clean original repository at the desired revision, then explicitly import it:

```sh
python3 tools/sync_engine.py --source ../lumencode --revision COMMIT
python3 tools/build_engine.py
```

Review source and provenance changes, run the shared fixtures and original contract validator, then the corpus/damage and rendered GUI gates. The sync tool stops if an existing shared grammar differs; update that grammar deliberately before accepting a new engine version. Do not patch language algorithms inside the transport or Swift adapter.

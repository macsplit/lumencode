# Shared analysis engine

`LumenCode::Analysis` is the single CMake analysis target used by the Qt GUI,
original public CLI, Qt smoke test and macOS JSON helper. Its five modules live
in `engine/src/`; all compiled grammars live in root `third_party/`.

The library keeps Qt Core/Concurrent internally. Qt presentation/controllers stay
in `apps/qt/`; Swift models, scheduling and presentation stay in `apps/macos/`.
The JSON helper in `engine/helper/main.cpp` handles transport only. Its own
`--index-facts` command supports the index's self-launched worker processes.

## Build boundaries

Root CMake options select `LUMENCODE_BUILD_QT_APP`, `LUMENCODE_BUILD_CLI` and
`LUMENCODE_BUILD_ENGINE_HELPER`. The historical `LUMENCODE_CLI_ONLY=ON` option
enables the public CLI and disables the Qt GUI. The public CLI can retain Widgets
without making it a helper-only dependency.

On macOS, `python3 tools/build_engine.py` configures helper-only CMake, copies its
private libraries, rewrites load paths, signs the copies ad hoc and records their
minimum OS versions. Xcode invokes the same script with DerivedData-specific
build and app runtime directories. This does not provide release signing or
notarization, or fix the existing macOS 14 transitive-library limitation.

The [macOS distribution workflow](../docs/macos/distribution.md) builds a private
universal Core/Concurrent kit without those Homebrew transitive dependencies,
checks both architecture slices and signs/notarizes the complete application.

## Normal development and provenance

Edit analysis code here and validate both consumers in the same change. There is
no engine snapshot, `sync_engine.py`, independent engine checkout or copied parser
implementation to update. Runtime `source-provenance.json` records the monorepo
commit when Git is available, source hashes and dependency origins; source-archive
builds still work without Git metadata.

`engine/origin.json` records the original repository revisions and migration
hashes. Those hashes support the one-time `verify_consolidation.py
--migration-baseline` gate; they do not block future engine edits. Ordinary
`verify_consolidation.py` checks the layout and vendored inputs.

`third_party/provenance.json` and root notices preserve the vendored sources and
licenses. Review grammar changes and update their provenance deliberately. Build
verification never downloads or regenerates parsers.

## Parity and resilience

Shared fixtures remain at `tests/fixtures/baseline/`. Root `tools/regression_sweep.py`,
`corpus_scan.py`, `damage_probe.py`, `corpus.json` and `fetch_corpus.py` retain the
original validator, discovery and recovery measurements.

```sh
python3 tools/fetch_corpus.py --dest /tmp/lumencode-public-corpus
python3 tools/upstream_resilience.py corpus \
  --cli /path/to/new/cli --reference-cli /path/to/frozen/reference-cli \
  --root /tmp/lumencode-public-corpus --output /tmp/corpus.json
python3 tools/upstream_resilience.py damage \
  --cli /path/to/new/cli --reference-cli /path/to/frozen/reference-cli \
  --root /tmp/lumencode-public-corpus --output /tmp/damage.json
```

Comparisons fix `QT_HASH_SEED=0` and retain complete JSON values. During migration,
use a frozen pre-migration executable as an independent reference; comparing two
newly rebuilt consumers alone could miss a shared extraction mistake. The runner
freezes executable/runtime copies to avoid interference from concurrent relinks.

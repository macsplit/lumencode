# LumenCode monorepo consolidation plan

Status: proposed implementation plan, 17 October 2026. No history import,
repository restructuring, GitHub archival or deployment has been performed.

## Decision and scope

Use `macsplit/lumencode` as the canonical repository. Import the Swift main-branch
history once beneath `apps/macos/`, then reorganise both products around a single
analysis library. The final checkout uses ordinary tracked directories, one Git
repository and standard CMake, Xcode and SwiftPM entry points. It requires no
submodule initialisation, subtree commands, source synchronisation, sibling
checkout or repository-specific Git wrapper.

Preserve the current analysis algorithms, CLI contracts, application identities
and beta limitations throughout the migration. This work does not include Qt 6
migration, a new engine ABI, changing either interface's toolkit, replacing the
public CLI with the helper, or resolving the existing GUI timing and macOS 14
dependency issues.

The migration is complete when a fresh clone can build and test both products
from the shared sources and the former Swift repository points users to it.

## Starting state

These are inspected local revisions. Fetch and confirm GitHub state before
execution; tracking refs alone are not a fresh remote inventory.

| Area | Current state | Consequence |
| --- | --- | --- |
| Canonical candidate | `macsplit/lumencode`, local `main` at `48cf4db` | Preserve its existing commit IDs and extend its history normally. |
| Swift application | `macsplit/lumencode-swift`, local `main` at `7523df0` | Import its main-branch ancestry after prefixing paths. |
| Shared analysis | Five C++ module pairs: `symbolparser`, `projectindex`, `weblinks`, `httpclients`, `agentqueries` | Extract these into a common library without algorithm changes. |
| Source equivalence | All 255 manifest inputs match the original checkout at the inspected revision | There is no current engine-source conflict to resolve. Recheck at the execution freeze. |
| Grammar copies | Original `third_party/`; Swift active `Sources/LumencodeNativeBridge/Vendor/` plus C++/Go under `Engine/Upstream/` | Keep one authoritative, fully vendored grammar tree, based on the original `third_party/`. |
| Inactive Git links | Nine mode-160000 entries under Swift `Vendor/` | Remove these from the final tracked tree. Active parser sources are ordinary files elsewhere. |
| Qt targets | GUI, public CLI and GUI smoke target each list analysis sources separately | Link all three to the shared library instead of compiling private engine copies. |
| Qt public CLI | Includes `ProjectController` and `FileSystemModel`; current build requests Widgets | Preserve the existing CLI/controller layer. Do not accidentally add its dependencies to the Swift helper. |
| Swift helper | `Engine/main.cpp`, using Qt Core and Concurrent | Move its transport into the shared engine area and link the same library. |
| Swift build | Xcode shell phase runs `tools/build_engine.py`; SwiftPM uses the resulting runtime | Repair source discovery and packaging paths in both build routes. |
| Distribution | Original Flatpak manifest, tracked `web/` assets and a signing/deployment script | Preserve application IDs, download paths and release assets; change path handling without deploying. |
| Automation | Neither inspected main tree has tracked `.github` workflows | Add the proposed checks explicitly rather than assuming CI already exists. |

Existing verification evidence is in the [integration report](engine-integration.md)
and [17 October follow-up](follow-up-2026-10-17.md). The accepted baseline includes
an 84 ms maximum GUI heartbeat gap in the expanded audit and a failing macOS 14
runtime gate. Those limitations remain visible; they are not new blockers that
this structural migration must fix.

## Final directory layout

```text
lumencode/
├── CMakeLists.txt
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── README.md
├── engine/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── symbolparser.cpp/.h
│   │   ├── projectindex.cpp/.h
│   │   ├── weblinks.cpp/.h
│   │   ├── httpclients.cpp/.h
│   │   └── agentqueries.cpp/.h
│   └── helper/
│       └── main.cpp
├── third_party/
│   ├── tree-sitter/
│   └── tree-sitter-*/
├── apps/
│   ├── qt/
│   │   ├── CMakeLists.txt
│   │   ├── src/
│   │   │   ├── main.cpp
│   │   │   ├── lumencode-cli.cpp
│   │   │   ├── projectcontroller.cpp/.h
│   │   │   ├── filesystemmodel.cpp/.h
│   │   │   ├── resources.qrc
│   │   │   ├── contents/ui/
│   │   │   └── icons/
│   │   └── packaging/
│   └── macos/
│       ├── Package.swift
│       ├── lumencode-swift.xcodeproj/
│       ├── Sources/
│       ├── Tests/
│       ├── Xcode/
│       └── tools/
├── tests/
│   ├── fixtures/baseline/
│   └── qt/
├── tools/
│   ├── build_engine.py
│   ├── regression_sweep.py
│   ├── corpus_scan.py
│   ├── damage_probe.py
│   ├── fetch_corpus.py
│   ├── corpus.json
│   ├── agent/
│   └── ...shared validation and packaging utilities
├── docs/
│   ├── engine/
│   ├── qt/
│   ├── macos/
│   └── migration/
├── web/
└── .github/workflows/
```

Keep the original public CLI source beside its Qt controller dependencies for
this migration. It remains a first-class product even though its source lives
under `apps/qt/`. A further controller/CLI split is optional future work.

Keep Swift source and test targets inside their Swift package directory. SwiftPM
does not need to compile C++ sources outside that package: the existing helper
boundary remains in place. Preserve QML paths relative to `resources.qrc` during
the first move to avoid changing resource URLs unnecessarily.

### Ownership and dependency rules

| Target or component | Owns | Depends on |
| --- | --- | --- |
| `LumenCode::Analysis` | Language analysis, AST/recovery/fallbacks, indexing, links and queries | Qt Core, Qt Concurrent, vendored Tree-sitter targets |
| `lumencode-engine` | JSON transport and helper-process entry point | `LumenCode::Analysis`; no Widgets, QML or KDE requirement |
| Qt controller target | Filesystem model and application/controller state | Shared analysis and its existing Qt dependencies |
| `lumencode` | Qt/Kirigami presentation | Qt controller, Quick/QuickControls2, Widgets/KDE as required |
| `lumencode-cli` | Existing public and interactive CLI commands | Qt controller and shared analysis |
| Swift core and applications | Swift models/adaptation, scanning, scheduling and presentation | Packaged helper runtime |
| Shared tests and tools | Contracts, fixtures, corpus selection and damage generation | Explicit executable paths and the single fixture tree |

An engine change must have one source location. Both products consume the same
checkout revision; there is no separate engine version that applications must
manually synchronise inside the monorepo. Packaged releases record the monorepo
commit and third-party provenance so shipped behaviour remains traceable.

## Phase 1: Freeze and capture the baseline

Work in disposable migration clones. Keep the two current working checkouts
available and unchanged until the combined build is accepted.

1. Fetch both remotes and confirm the exact main-branch tips. Reconcile any new
   work before setting the migration inputs. Pause feature changes during the
   final import, or restart the import if either source main advances.
2. Inventory remote branches, tags, releases/assets, issues, pull requests,
   default branches, protection/rulesets, Actions settings, secrets by name,
   webhooks and any Pages configuration. Git history import does not transfer
   GitHub metadata. Retain useful issue context through links or explicit issue
   transfer where supported; do not assume there is nothing to preserve.
3. Create complete local Git bundles of both repositories and verify them. Store
   bundles and metadata exports outside the migration checkout on durable storage.
   Record the starting SHAs and checksums. Keep non-main branches in the backups
   and original repositories unless deliberately included in the import.
4. Prepare clearly named pre-consolidation tags for each source revision. Record
   existing tag-name collisions before importing any tags. Publishing these tags
   belongs to the execution/cutover workflow, not this planning step.
5. Freeze pre-migration CLI binaries and their required runtimes outside mutable
   build directories. Record checksums, build settings and OS/toolchain versions.
   The macOS original CLI is a usable behaviour reference for this work.
6. Capture file inventories and hashes for the engine, grammars, fixtures and
   third-party licenses. Recheck the current 255-input correspondence.
7. Verify the current builds on macOS and obtain a Linux environment for the Qt
   GUI and Flatpak checks. Distinguish pre-existing SDK/dependency failures from
   migration regressions before accepting a new baseline.

**Exit gate:** durable backups, an agreed pair of source SHAs, recorded reference
binaries, the baseline evidence and a usable validation environment for each
platform. No source algorithm changes have been introduced.

## Phase 2: Import Swift history once

Create a normal migration branch from the canonical `lumencode` main tip.

1. Make a fresh, independent clone of Swift main, without importing tags or
   unrelated branches by default. Install and record `git-filter-repo` in the
   migration environment if needed; it is a one-time preparation tool, not a
   developer or application dependency.
2. Run the following only in that disposable clone, using the recorded tool
   version. Keep empty/merge topology and original commit-message hash references,
   and do not introduce replacement refs that require special client behaviour:

   ```sh
   git filter-repo --to-subdirectory-filter apps/macos \
     --prune-empty never --prune-degenerate never \
     --replace-refs delete-no-add --preserve-commit-hashes
   ```

   `--preserve-commit-hashes` preserves hash references in messages; it does not
   prevent rewritten commits from receiving new IDs. Do not run this against
   either live repository or rewrite the original LumenCode history. Avoid
   `--force` by starting with a suitable fresh clone. These options are documented
   in the [upstream git-filter-repo manual](https://github.com/newren/git-filter-repo/blob/main/Documentation/git-filter-repo.txt).
3. Save its old-to-new commit mapping and the tool/version/options used. Preserve
   commit authorship, messages and dates. Rewritten Swift commits receive new
   hashes; existing links remain valid in the retained Swift repository.
   Any existing cryptographic commit/tag signatures cannot remain valid over
   rewritten objects; the original signed objects remain in the source archive.
4. Fetch the rewritten main tip into the canonical migration clone and merge it
   with `--allow-unrelated-histories`. This creates a normal merge commit; it
   does not create a subtree relationship or nested checkout.
   This is the standard [Git unrelated-history merge](https://git-scm.com/docs/git-merge#Documentation/git-merge.txt---allow-unrelated-histories),
   followed by ordinary commits and a normal repository workflow.
5. Do not squash or rebase away the imported ancestry when the migration branch
   is eventually merged. Use a normal merge commit for the final pull request.
6. Compare the imported `apps/macos/` tree with the original Swift tip before
   making structural edits. Verify both the untouched original tip and mapped
   Swift tip are ancestors of the combined branch, and run `git fsck`.

The old inactive `Vendor/` Git links may exist temporarily in this imported
snapshot and in historical commits. Phase 5 removes them from the final tree;
there will be no need to initialise them to build either product.

**Exit gate:** both main histories are reachable, the existing original commit
IDs are unchanged, and imported Swift files match their source revision. Remove
any temporary import remote after use; no persistent repository linkage remains.

## Phase 3: Make the imported layout build before extracting the engine

At this checkpoint the original application remains at the repository root and
Swift is under `apps/macos/`. Keep the duplicate engine snapshot temporarily so
history/path changes can be verified separately from engine extraction.

1. Repair the path-bearing entries in Swift's `Engine/upstream.json` for its new
   temporary location without changing source hashes, or make the verifier resolve
   paths relative to a clearly recorded manifest base. Do not regenerate hashes
   merely to silence discrepancies.
2. Make the imported Swift package, Xcode project, helper packager and tests work
   from their new location. Keep build output under ignored build directories.
3. Give build/test tools an explicit repository/app root rather than assuming the
   current working directory. Source archives and Git worktrees should work too;
   locating sources must not require `.git` to be a directory or Git to be present
   in an end-user installation.
4. Replace the GUI audit's sibling `../lumencode` assumption with an explicit
   benchmark root. Continue using the same fixture/stress workload for comparable
   timings instead of assuming the larger monorepo is an identical workload.
5. Commit or otherwise reproduce the Xcode scheme needed by command-line builds.
   Narrow the existing blanket `*.xcscheme` ignore rule so shared schemes can be
   tracked while user-specific settings remain ignored.

**Exit gate:** original CLI/Qt baseline remains intact; relocated SwiftPM and real
Xcode builds pass; fixtures/contracts and native GUI functional checks still pass.
This checkpoint is independently reviewable even if engine extraction is delayed.

## Phase 4: Extract one analysis library

1. Move the five engine module pairs from the original `src/` into `engine/src/`
   using ordinary Git moves. Preserve file contents, including upstream formatting.
2. Keep the original complete `third_party/` tree as the canonical runtime and
   grammar source. Map Swift's `tree-sitter-runtime` name to its original
   `tree-sitter` location in build definitions; do not create an alias checkout or
   symlink dependency. Check every compiled parser/scanner/header and notice before
   discarding a duplicate.
3. Create a static CMake target, exposed as `LumenCode::Analysis`, for those engine
   modules. Compile its QObject metadata with AUTOMOC and link its grammar targets
   once per build configuration. Express public Qt/header dependencies and static
   link dependencies through targets rather than copied flag lists.
4. Link the Qt GUI, public CLI and Qt GUI smoke target to that library. Factor
   their common controller/filesystem sources into a separate Qt controller target
   where useful. They must stop compiling private copies of engine source files.
5. Move Swift's transport-only `Engine/main.cpp` to `engine/helper/main.cpp` and
   link `lumencode-engine` to the same library. Keep its executable name, commands,
   JSON payloads and stdout/stderr behaviour unchanged.
6. Define explicit root CMake options for the Qt GUI, public CLI and engine helper.
   A helper-only build must configure with Core/Concurrent and no Widgets/KDE/QML.
   A public-CLI build may retain its existing Widgets dependency. Preserve the
   existing `LUMENCODE_CLI_ONLY` entry point as a compatibility option mapping to
   the appropriate target selection.
7. Build from clean output directories; do not rely on old CMake caches referring
   to moved paths. Test QObject generation in a checkout path containing spaces.

**Exit gate:** one maintained source copy and one analysis target per configuration;
all consumers link successfully; helper-only configuration does not acquire GUI
dependencies; frozen pre-migration CLI comparisons pass before duplicate removal.

## Phase 5: Complete the layout and remove source duplication

### Qt and public CLI

- Move presentation/controllers and their resources beneath `apps/qt/`; preserve
  the relative QML/resource paths where possible.
- Preserve executable names, command options, interactive requests, agent-facing
  output, settings identifiers and application/desktop IDs.
- Move Qt smoke-test sources to `tests/qt/` and update their include/resource paths.
- Update desktop/icon installation and make root CMake the standard entry point.

### Swift and runtime packaging

- Keep the Swift package and Xcode project beneath `apps/macos/`.
- Put shared native build/packaging orchestration at root `tools/build_engine.py`;
  it configures the root CMake project in helper-only mode. App-local wrappers, if
  retained, forward to that script and never maintain their own engine definitions.
- Update the Xcode shell phase's script/source paths and its source/license inputs.
  Preserve the packaged `Contents/Resources/EngineRuntime` location and DerivedData
  isolation; an Xcode build must not overwrite a SwiftPM runtime under test.
- Adjust development CLI/helper discovery for the monorepo. Preserve the packaged
  app's rule that it only uses its embedded runtime, with no development fallback.
- Remove obsolete Xcode C++ include paths and bridge references once their lack of
  use is verified. Do not turn SwiftPM into a separate parser build.

### Shared sources, tests and provenance

- Remove `apps/macos/Engine/Upstream`, duplicate compiled grammars under the old
  native bridge path and the nine inactive `apps/macos/Vendor/` Git links after
  every active consumer points to the canonical files.
- Consolidate fixtures at `tests/fixtures/baseline/`, keeping manifest semantics
  and expected values intact. Keep Swift-specific tests within the Swift package.
- Consolidate identical corpus/validator tools at root `tools/`. Update CLI/root/
  fixture arguments instead of importing modules from a copied upstream checkout.
- Retire `sync_engine.py`: engine updates become ordinary monorepo changes.
  Replace the engine snapshot manifest with a migration-origin record and a
  third-party provenance manifest. Application source edits must no longer require
  copying code or refreshing a checksum to permit a normal build.
- Keep the original repository URLs and revisions as historical origin records.
  Preserve all license texts, authorship and parser generation information.
- Preserve dated audit evidence as historical results; add new results at their
  new location. Update current documentation links without rewriting old evidence.
- Leave unrelated unused Swift resolver/parser cleanup for a later change unless
  it blocks the directory move. Eliminate its duplicate grammar/build dependency,
  not its history.

**Exit gate:** no active `Engine/Upstream` copy, no tracked Git links, no nested Git
metadata, no sibling-checkout requirement, and no source-sync command in the normal
workflow. The current tree contains a single authoritative engine/grammar/fixture
set. Historical commits may still contain their original snapshots and Git links.

## Phase 6: Repair distribution and developer entry points

1. Keep the existing application names, bundle/desktop IDs and settings/cache
   identifiers. Use cold caches for migration validation, then verify warm caches;
   do not change cache policy as part of a path move.
2. Update Flatpak's source root and CMake options for the complete monorepo. If its
   manifest moves into `apps/qt/packaging/`, ensure `type: dir` still includes the
   repository root, shared engine and grammars, not just the application folder.
3. Audit `tools/deploy-web.sh`: it currently assumes root `web/` and `repo/`, signs
   assets and updates a remote server. Make path resolution explicit and validate
   generated content locally. Preserve existing web/download URLs and tracked
   release assets during migration. Do not run deployment or rotate signing keys.
4. Keep `tools/agent/` at the root where practical. Repair CLI discovery and test
   MCP/hook commands against an explicitly supplied public CLI. Preserve the
   installer entry point; do not install user hooks or alter external settings.
5. Keep the root license/notices and bring Swift-specific provenance under current
   macOS documentation. Ensure packaging collects licenses from `third_party/`.
6. Provide a root README with exact Linux Qt, headless CLI/helper, SwiftPM and Xcode
   build commands. Preserve executable names to avoid unnecessary agent breakage.
7. Consolidate ignore rules. Ignore caches, generated app/runtime/build outputs and
   user settings without hiding shared schemes, fixtures, notices or source files.

**Exit gate:** native build tools work directly, the packaged app relocates, agent
commands work, and Flatpak/web packaging consumes the shared tree. Publishing and
signing actions remain outside local build/validation.

## Phase 7: Validate from a fresh clone and prepare the cutover

Run the validation matrix below from a new normal clone of the migration branch,
without recursive clone flags. Test a non-default checkout name and a path with
spaces. Keep corpus data, frozen reference binaries and mutable build outputs
outside the tracked source tree.

Do not accept a comparison of only two newly rebuilt consumers as the entire
migration proof: both could share an extraction mistake. Retain the frozen
pre-migration executable as an independent behaviour reference. Invoke old and new
executables on the same new fixture/corpus paths so complete payload comparison
does not need path fields removed. Fix `QT_HASH_SEED=0` as in the existing suite.

| Check | Required outcome |
| --- | --- |
| Git history and tree | Original tip and mapped Swift tip are ancestors; import tree matches; `git fsck` passes; no mode-160000 entries or `.gitmodules` dependency in the final tree. |
| Source/grammar inventory | Moved analysis and compiled grammar inputs retain their baseline hashes; intentional path/build changes are separated from algorithm edits. |
| Helper-only CMake | Builds without Widgets, Quick, KDE or sibling sources. |
| Public CLI on macOS and Linux | Builds; existing public flags, interactive state/selection and agent queries work. |
| Qt GUI and smoke test on Linux | Builds; QML/icons load; headless/offscreen smoke and a real GUI launch pass. |
| SwiftPM | GUI/CLI/core targets build; existing nine integration tests pass after fixture-root updates. |
| Xcode application | Fresh DerivedData build packages the correct shared helper and runtime. |
| Shared fixture manifest | All 101 cases and 625 expectations pass. |
| Contract/live comparison | Original contract validator reports zero issues; all 99 fixture file payloads match the frozen reference. |
| Public corpus | Reproduce the 27 pinned repositories and 6,360 discovered files at the existing manifest revision; compare complete output with zero differences/failures. Explain any changed discovery count. |
| Damage probe | Reproduce the existing seed, candidate selection and settings; retain zero differences/failures across the prior 8,714 clean/mutated comparisons, with equivalent retained-symbol/extra-declaration results. |
| Index/process lifecycle | Cold and warm index/search, reciprocal calls, ambiguity suppression, helper `--index-facts` self-launch, cancellation and project replacement work at the new executable locations. |
| Native macOS GUI | Existing 18 functional assertions pass, with no table/update warnings; screenshot review includes minimum size, search/signatures, damaged, deleted/restored and large-file states. |
| Timing | Record comparable fixture/stress timings. Preserve the known failing 50 ms gate as a reported beta limitation; investigate material regressions introduced by the move. |
| Runtime and launch | Relocated app/CLI works from a neutral directory and path with spaces without developer library paths. Preserve and report the existing failing macOS 14 check. |
| Flatpak, web and agents | Linux package builds/launches where the baseline SDK is available; staging assets and agent commands resolve new paths without deploying or installing hooks. |
| Ordinary checkout | Builds need no submodule/subtree operation, recursive clone, sibling repository, downloaded engine source or Git at application runtime. Dependencies may be installed normally beforehand. |

For ongoing CI, add a Linux engine/CLI contract job, a Linux Qt build/smoke job and
a macOS helper/SwiftPM/Xcode job. Select a macOS runner supported by the runtime
being tested; the current newer-library limitation must not turn skipped analysis
tests into a claimed pass. Run corpus/damage suites for engine or grammar
changes and before this migration merges. Run rendered native GUI audits on a
desktop-capable runner; compile/startup checks on a runner without an interactive
desktop must not be reported as native input coverage. Keep timing and macOS 14
exceptions explicit rather than suppressing arbitrary failures.

**Exit gate:** the migration's final commit has complete reports, both platforms
are checked, and remaining failures are the specifically accepted beta limitations
or documented pre-existing environment limitations, not unexplained regressions.

## Phase 8: Merge and archive

1. Present the migration branch/PR, history mapping, validation evidence and the
   exact source revisions for final review. Recheck neither source has gained
   unimported feature commits during preparation.
2. Merge into `macsplit/lumencode` with a normal merge commit. Do not force-push
   main, squash the migration ancestry, or rewrite existing original commits.
3. Verify a fresh clone of canonical main using the documented build entry points.
4. Add a migration notice to the Swift repository with the canonical repository,
   `apps/macos/` location, documentation and last pre-migration revision. This
   administrative notice may be a post-import commit; it is not new product code
   requiring another history import.
5. Preserve existing Swift releases, tags and commit links; address useful open
   issues/PRs explicitly. Update descriptions/homepage links and disable duplicate
   release automation if any was found during the GitHub inventory.
6. Archive `lumencode-swift` only after canonical main is usable. Do not delete or
   redirect its Git repository by replacing its history.
7. Mark the migration record complete and declare future engine and application
   work to happen in the monorepo.

This phase changes GitHub state. The current request authorises preparing this
plan; it does not itself perform that cutover or archive either repository.

## Commit and review sequence

Use a single migration branch with focused commits and retained test checkpoints:

| Commit | Purpose |
| --- | --- |
| 1 | Merge the path-rewritten Swift history into the unchanged original tree. |
| 2 | Restore relocated Swift builds, test roots, manifest resolution and shared scheme. |
| 3 | Extract the analysis CMake library and relink Qt targets. |
| 4 | Move/link the JSON helper and update native runtime packaging. |
| 5 | Move Qt resources/packaging into `apps/qt/`. |
| 6 | Consolidate grammars, fixtures/tools and provenance; remove copied snapshot and Git links. |
| 7 | Repair public CLI/agent/package entry points, documentation and CI. |
| 8 | Record fresh-clone/platform/corpus/native validation and accepted beta exceptions. |

The history import commit intentionally has two parents. Keep the final canonical
merge non-squashed so imported Swift ancestry survives. Prefer file moves and
build edits in separate commits from any unavoidable source correction; a parser
algorithm change is a reason to stop and handle it outside this migration.

## Rollback and drift handling

Before canonical merge, rollback means discarding the migration branch/clones;
neither source main needs resetting. If a source main advances, capture the new
tip and redo/reconcile the import before publication, with a new mapping and
verification record. Do not rerun history rewriting against published canonical
history to absorb subsequent changes.

After canonical merge, prefer forward fixes for directory/build issues. For an
urgent full rollback, identify the final merge's first parent and prepare a normal
revert using the correct mainline parent, then verify the restored build. This
preserves published history. A merge revert does not erase imported ancestry, so
reintroducing the migration later needs a deliberate revert-of-revert or new
forward changes, not an assumption that remerging the old branch will replay it.
See Git's documented [merge-revert behaviour](https://git-scm.com/docs/git-revert).

Keep the Swift repository writable until the canonical post-merge verification
passes. It can be unarchived later if necessary. Website/release deployment is a
separate action with its own rollback and is not bundled into the repository move.

## Effort and implementation order

Allow roughly 3–5 focused development days, assuming usable macOS and Linux
dependencies. This is a planning estimate, not a delivery date. History import is
the small part; build, packaging and cross-platform validation dominate. Resolving
an unavailable legacy KDE/Flatpak SDK or macOS dependency builds could extend the
schedule and should be tracked separately from source consolidation.

Start with Phase 1 and the history/build checkpoint in Phases 2–3. Only proceed
to duplicate removal after the common library and both integration methods pass.
The first deliverable for implementation should be a reviewable migration branch
with both histories preserved and both existing products building in the combined
checkout, followed by the shared-engine extraction and final validation.

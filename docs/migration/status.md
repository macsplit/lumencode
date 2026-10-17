# Consolidation verification — 17 October 2026

**Status: merged and cutover completed on 17 October 2026.**

Canonical `main` in `macsplit/lumencode` contains a normal
monorepo with one engine and one set of vendored grammars. Qt's original main
history is unchanged. Swift's main history is imported once under `apps/macos`,
with the mapping retained in `swift-commit-map.txt`. No Git links, submodules,
subtrees or engine synchronization steps remain.

The extraction preserves all ten original engine source/header files and all Qt
application source files byte for byte. The 841 vendored inputs are verified
against their provenance. `python3 tools/verify_consolidation.py
--migration-baseline` checks these one-time invariants and both imported ancestors.
Normal development uses the same command without `--migration-baseline`, allowing
engine changes in one location.

## Recorded checks

| Check | Result |
| --- | --- |
| Linux baseline and consolidated Qt GUI, CLI, helper, smoke builds | Passed |
| Linux original/new CLI complete JSON comparison | 99 files, zero differences |
| Linux rendered offscreen regression | 126 files, 895 selections, zero timeouts or QML problems |
| Linux real desktop dark-mode regression | 4 files, 67 selections, zero timeouts or QML problems; production app stayed running |
| Linux agent initialize, tool listing and project summary | Passed |
| Linux Flatpak local build and desktop launch | Passed; stayed running for eight seconds; no installation or publication |
| macOS Swift package | 9 tests passed, including fresh clone with spaces and external scratch directory |
| macOS shared-scheme Xcode app | Fresh clone/DerivedData paths containing spaces: build passed |
| macOS shared fixture expectations | 101 cases, 625 checks passed |
| macOS C++ and Swift full JSON contracts against original CLI | 99 files each, zero differences/issues |
| Pinned public corpus against original CLI | 6,360 comparisons, zero differences/failures |
| Damaged-source sweep against original CLI | 8,714 comparisons, zero differences/failures |
| macOS native/rendered GUI | 18 functional checks passed; no native update warnings |

The full comparisons used frozen pre-migration executables, not another consumer
rebuilt from the extracted engine. Both broad sweeps checked all 27 pinned corpus
revisions. Machine-readable reports and application-only screenshots are in
[evidence](evidence/). Local build logs and verified repository bundles are kept
in the dated migration-backup directories on both development machines.

The Qt warning collector intentionally injects a missing-object QML warning after
the smoke summary to test its own detection. That deliberate self-check is not an
application failure.

## Visual theme

The first isolated Qt desktop run used a light fallback palette. Its functional
checks remain valid, but its screenshots are superseded for visual review by the
[dark-mode capture](evidence/qt-dark.png). Dark validation copies the user's
`kdeglobals` into the audit's isolated settings and selects the KDE platform theme;
it does not change desktop settings or force the application's production theme.

For repeatable desktop captures, run from the repository root in the KDE session:

```sh
cmake --build build --target lumencode-gui-smoke
python3 tools/run_qt_gui_audit.py --build build --output /tmp/lumencode-qt-dark
```

The audit output retains the palette used. The offscreen CI smoke is a functional
rendering gate; its default palette is not the dark-mode visual acceptance gate.

## Fresh checkout verification

Fresh independent clones on both machines use paths containing spaces and no
recursive clone flags. Linux builds all four consumers, passes 101 cases/625
assertions and the 99-file frozen-original contract comparison, and reruns the
dark-mode desktop audit. The workflow YAML parses successfully. macOS builds
the helper, Swift package and shared-scheme Xcode application; all nine Swift
tests pass using an external scratch directory. This caught and fixed package
working-directory runtime discovery. A relocated runtime with a path containing
spaces launches with no external developer-library references.

The [hosted Linux PR workflow](https://github.com/macsplit/lumencode/actions/runs/39034304571)
passed builds, fixture/contracts and the complete rendered Qt smoke. Native macOS
and broad resilience evidence above comes from the completed local runs; their
hosted workflow jobs remain manually triggered.

## Accepted beta limits

This migration does not claim to resolve the accepted product backlog. The native
Debug audit's maximum observed heartbeat gap was approximately 70 ms, above the
optional 50 ms target. These timings measure the test window and model/native
NSEvents, not production-menu, OS HID or frame-presentation latency. Filesystem
refresh, type-link navigation and diagnostic presentation remain development work.

The current packaged Homebrew runtime still contains libraries requiring macOS
26/27 despite a macOS 14 executable target. The explicit target-14 runtime gate
must pass before claiming support there. Release signing/notarization are separate.

## Cutover

The migration is isolated from the original checkouts. The Linux Swift checkout's
local modifications were backed up and left intact. Original repository bundles,
patches, the Linux worktree archive, source maps and hosting inventories exist
before any cutover.

[PR #2](https://github.com/macsplit/lumencode/pull/2) merged with a normal merge
commit, `6a68f9ad253a762b77d4882edfab5ba344d92ea7`, preserving the import ancestry.
A fresh clone of canonical main matched the tested migration tree exactly and
passed history/integrity checks, a helper-only build and all 99 frozen-original
file contracts with zero differences or issues.

The former [lumencode-swift repository](https://github.com/macsplit/lumencode-swift)
now has a migration notice and is archived. Its last product-source revision is
`7523df005031c67c1d494699c57dc03a06283407`; the administrative README notice is
`831edc017041ef6e08393f22b7f486ffeeb0c81e`. Existing tags, releases and history
are preserved.

Future development uses canonical `macsplit/lumencode`. The dedicated
`lumencode-consolidation` checkouts on Mac and Linux track its `main`; the original
checkouts, including local Swift modifications, remain intact. No website
deployment or release was performed.

# Development plan

Updated 17 October 2026. The Swift GUI and CLI use the original LumenCode engine,
pinned at `48cf4db`. Parser, recovery, fallbacks, signatures, indexing, web links
and search must continue to come from those shared sources.

The [16 October integration report](engine-integration.md) records the initial
implementation. The [earlier plan](development-plan-archive-2026-10-16.md) is
historical: its directions to finish the separate Swift parser/resolver no longer
apply.

## Completed foundation

- Original C++ analysis engine runs behind a narrow JSON helper boundary.
- Both Swift entry points preserve the original analysis payload.
- Shared fixtures, original contract validation and live original CLI comparisons
  are repeatable gates; upstream corpus/damage tools are now pinned too.
- Project indexing runs separately from immediate selected-file analysis.
- Search, signatures, recovery notices and relation navigation are presented.
- The sidebar now uses a native table with explicit snapshot/selection updates.
- Native mouse/key events exercise selection, folders, search submission and
  project replacement; rendered audits gate AppKit/SwiftUI update warnings.

## Priority 1: Compatible release runtime

Build or obtain Qt's transitive dependencies with macOS 14 deployment targets.
Keep engine behavior pinned while changing packaging. Run
`tools/verify_runtime.py --target-macos 14.0` against the packaged runtime, then
launch a relocated app on macOS 14 and a clean current system. The explicit
compatibility gate currently fails on six libraries requiring macOS 26/27.

Complete dependency notices, Developer ID signing and notarization before public
distribution. Signing/publishing is separate from local verification.

Acceptance: the compatibility and relocation gates pass, a clean machine can
analyse a project without developer paths, and the supported minimum OS is tested.

## Priority 2: Complete native GUI regression coverage and timing

Add production menu/Command-F/Command-R tests, accessibility-driven navigation,
split dragging, wheel scrolling, multiple windows and actual OS input delivery.
Measure Release p50/p95 with a repeatable workload and retain screenshot review
at minimum and large sizes. Keep the optional 50 ms main-runloop pause gate;
one successful Debug run does not establish sustained Release latency.

Acceptance: final selection remains correct during rapid input/project changes;
no native update warnings; recorded native event-to-data and presentation checks
meet the selected budget without background benchmark contention.
The expanded 17 October Debug audit passes all functional/warning checks but
fails the 50 ms budget (84 ms maximum); isolate repeated SwiftUI updates before
claiming this gate is complete. See the [follow-up report](follow-up-2026-10-17.md).

## Priority 3: Refresh and index lifecycle

Invalidate dependent results after edits and add filesystem change handling.
Refresh indexed enrichment for an active inspection without losing selection or
scroll position. Cover deleted/unreadable files, project replacement and edited
imports in mixed-language projects. Preserve upstream ambiguity and binding rules.

Acceptance: edits refresh affected views and reciprocal relationships without
requiring a project-wide parse on each click or leaving stale symbols visible.

## Priority 4: Presentation completeness

Add type-link navigation and damaged-line gutter markers. Improve inspector and
search density, long declaration/path handling, large snippets and empty states.
Keep structured signatures and source/recovery information visible.

Acceptance: screenshot and interaction review covers damaged, sparse, large and
multilingual projects; useful shared-engine data is reachable from the GUI.

## Ongoing engine updates

Advance the pinned source snapshot explicitly with `tools/sync_engine.py`.
Run shared fixtures/contracts on each update, the public corpus and damage probe
for parser changes, and rendered/native checks for presentation changes. Preserve
complete payload comparisons rather than relaxing them to symbol counts. Retire
unused parser/resolver scaffolding once the wider gates remain stable.

Do not introduce language heuristics in Swift or the JSON transport. Fix analysis
behavior in the original engine and import the reviewed revision.

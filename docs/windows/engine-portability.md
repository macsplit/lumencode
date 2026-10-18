# Shared-engine portability follow-up — 17 October 2026

Continue `windows/native-winui`, following `7e014f7`. The earlier 19-file corpus
discrepancy set was reduced to small reproductions and corrected in the canonical
engine/vendor source. Qt, Swift and WinUI continue to consume the same parser.

## Results

Complete helper JSON was compared for the same 6,297 regular files from the 27
pinned repositories, with matching input SHA-256 values and zero failed analyses.
Only absolute corpus-root path prefixes/separators are normalized. No fields,
array ordering, recovery notices, symbols or call-site data are discarded.

| Comparison against macOS | Before | After |
|---|---:|---:|
| Windows | 10 differing files | **0 differing files** |
| Linux | 9 differing files | **2 differing files**, only PHP include paths |

See [full comparison](evidence/engine-portability-corpus.json),
[focused 19-file reproduction](evidence/engine-portability-focused.json) and
[raw-capture hashes/timings](evidence/engine-portability-captures.json).
The combined strict verdict remains `passed: false` because the two Linux path
differences remain visible. This is not a claim that every possible source or
project index is identical across operating systems.

## Corrections

### Swift external scanner

The `try!` token is external symbol 32. The scanner built its suppression mask
with `1UL << 32` and tested it with `1 << 32`. Windows uses 32-bit `unsigned long`,
and the second expression also shifts a 32-bit integer out of range. Compiler
behaviour therefore changed the syntax tree and downstream call-site facts.

A four-line reproduction showed `try! out.close()` yielding receiver `! out` on
Windows and `out` on macOS. Both shifts now use `UINT64_C(1)`. Grammar tables,
grammar version and the existing call-site extractor are preserved. This is one
shared vendor patch, documented in `THIRD_PARTY_NOTICES.md` and in
`third_party/provenance.json` with both original and patched hashes. All 841
vendored inputs pass the updated provenance check.

The new `swift_try/Calls.swift` fixture asserts receiver, callee and source line
for `try!` and `try?` calls. The receiver assertion rejected the old Windows
binary before the fix; the same assertion passes on all three platforms.

### Deterministic AST repair

`AuthenticationInterceptorTests.swift` exposed a second issue: the 1,500 ms
wall-clock cutoff could stop the greedy repair search at different candidates.
The slow Windows run returned a different damaged-line set and analysis mode.

Repair now completes the existing deterministic, source-size-adjusted trial
budget: at most 8–80 trial parses, 12 blanked lines and 12 insertion trials. Every
trial path checks the common budget, including insertion candidates; reaching
the budget also stops preparing unused variants. The existing file-size limit,
initial-error rejection, stagnation checks, process cancellation and interface
timeouts remain in force. No new recovery or language-selection algorithm was added.

The formerly differing file now reports the same recovered lines `[29, 1516]`
and complete payload on all systems. In the full concurrent corpus run it was
also the slowest file: approximately 1.74 s on macOS, 1.41 s on Linux and 4.61 s
on Windows, below the current Windows standalone-analysis timeout of 10 s. These
are VM/host diagnostic samples, not general latency guarantees.

### Kotlin imports

On a case-insensitive filesystem, `okio.buffer` could be guessed as `Buffer.kt`,
creating a false file resolution/binding. Kotlin identifiers are case-sensitive.
Candidate source files now require exact case for each component below the known
source root, while preserving caller spelling of drive/home paths. The shared
resolver code works on every OS. The `kotlin_case` fixture checks both a rejected
lowercase import and a successfully resolved, correctly cased class import.

### Ruby relative require candidates

The existing ancestor/lib search could probe a path containing a nonexistent
`lib/..`. Windows normalized that path before checking existence; POSIX traversal
required the intermediate directory to exist. Candidates are now cleaned before
the file check. The `ruby_relative` fixture exercises this without creating a
dummy lib directory. Both the Kotlin and Ruby fixtures failed against the old
macOS helper before the fixes.

The existing fixture evaluators now assert call-site records and resolved/
unresolved dependency targets. The damage probe explicitly reads/writes UTF-8
and LF so Windows locale conversion cannot corrupt its generated inputs.

## Remaining PHP paths

The only remaining differences are:

- `CodeIgniter/system/core/Hooks.php`: a candidate spelled `hooks.php`.
- `CodeIgniter/tests/mocks/database/db.php`: a candidate spelled `DB.php`.

Direct filesystem probes confirm that each spelling identifies the same physical
file on the tested macOS/Windows volumes and is absent on Linux. Every other
JSON field in these two analyses matches. No parser override or comparison
allowlist was added to manufacture identical host path resolution. See
[filesystem evidence](evidence/engine-portability-filesystem.json).

## Verification

- macOS Qt CLI, macOS Swift CLI, Linux Qt CLI and Windows Qt CLI each pass all
  **104 fixture cases / 638 assertions / 102 invocations**.
- Each interface's complete 102-file JSON contract comparison has zero contract
  issues and zero differences from its shared helper.
- The macOS Swift package passes all **9 integration tests** using the fixed helper.
- The packaged WinUI frontend with the fixed runtime passes **67 GUI/input checks**.
- The existing malformed-source fixture probe runs **109 mutations** on macOS and
  Windows with no failed analyses. macOS before/after has no symbol-retention
  regressions; mean retention is 96.1%. Earlier missing-closer/structural-parser
  losses remain, so this is not a claim of perfect damage recovery.

Evidence: [fixture/contracts](evidence/engine-portability-regressions.json),
[GUI/input](evidence/engine-portability-gui.json),
[damage summary](evidence/engine-portability-damage.json).
Raw full/focused captures and probe reports are preserved outside Git under
`/home/user/Code/Coding/lumencode-validation-20261017/` on Kubuntu. The excluded
63 duplicate Objective-C header symlink aliases are the same limitation as in
the previous portable corpus snapshot; their ordinary source targets remain covered.

## Remaining work

The reproduced algorithmic corpus differences are addressed. Retain the fixtures
and shared-source approach. Broader damaged-public-corpus and complete project-index
comparisons remain future coverage; this pass compares file analysis and existing
index integration fixtures. Windows rendered/input latency, human Narrator/DPI/
multi-monitor checks, clean-machine acceptance and installer/signing arrangements
remain as described in [resume-validation.md](resume-validation.md).

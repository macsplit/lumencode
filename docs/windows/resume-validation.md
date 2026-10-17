# Windows resume validation — 17 October 2026

This records the earlier `7e014f7` checkpoint. The subsequent shared-engine
[portability follow-up](engine-portability.md) resolves the algorithmic corpus
differences listed here; this document retains the original evidence.

Continue `windows/native-winui`. The shared C++ engine, AST/fallback logic and all
vendored grammars were preserved. This pass followed the agreed order: preview
responsiveness, native Windows behaviour, broader engine comparisons, then
portable distribution preparation. It is a beta checkpoint, not release acceptance.

## Preview responsiveness

The old whole-document RichEdit path stalled badly on generated Unicode input.
The viewer now loads the existing bounded source preview into logical pages of
at most 8,192 UTF-16 characters, preferring newline boundaries and never splitting
a CRLF pair or surrogate pair. Every page is syntax coloured with the existing
display-only rules. Previous/Next have accessible names and keyboard activation.
Copy retains the entire loaded preview; engine analysis still receives the full
file. Line navigation opens the containing page. A version guard prevents an old
asynchronous page-formatting request from overwriting a newer selection.
Colouring remains a lightweight lexical preview; a page beginning inside a
multiline string/comment can lack its earlier lexical context. Engine language
facts do not depend on these display colours.

Three assignment samples per workload on the same VM, at 125% display scale:

| Workload | Old median assignment | Paged median assignment | Maximum later-page assignment |
|---|---:|---:|---:|
| 32K coloured Unicode source | 234 ms | 60 ms | 53 ms |
| 128K Unicode source | 569 ms | 70 ms | 56 ms |
| 256K Unicode source | 1,173 ms | 63 ms | 63 ms |
| 128K single Unicode line | 6,394 ms | 122 ms | 133 ms |

The new operation presents one page; the old one presented the whole loaded
preview. Access to remaining text is retained through paging rather than doing
the same large synchronous insertion. The normal 2,000-symbol file also retains
all engine symbols; measured UI application work dropped from about 123 ms in
the instrumented baseline to around 38–44 ms in subsequent ordinary runs.

Evidence: [before](evidence/preview-profile-before.json),
[after](evidence/preview-profile-after.json),
[162-check profile/input run](evidence/preview-profile-gui.json).
Every page was read back through RichEdit, including Unicode/CRLF boundaries.
Default checks also exercise later-line navigation, final/previous page state,
complete copy source and stale-page cancellation. Core checks cover page bounds,
source reconstruction, CRLF and surrogate boundaries alongside the real adapter.

These are assignment/dispatcher diagnostics, not pixels-on-monitor measurements
or statistical benchmarks. The stress run still recorded a 1,566 ms maximum
dispatcher gap and 746 ms p95 while exercising all pages/round trips. Ordinary
runs have smaller but still significant gaps. A general 50 ms goal remains unmet;
rendered-frame and physical-input profiling must continue.

## Native behaviour

The normal packaged application passed 81 checks combining the existing workflow,
foreground-guarded OS keyboard/pointer tests, Space activation of both paging
buttons, automation names/roles/help, and native behaviour checks.

The audit opened the actual native folder picker owned by its window, cancelled
that exact dialog, and verified that the workspace remained selected. It enabled
real Windows high contrast, checked source text against the system WindowText
colour, captured the window and restored the original setting in `finally`.
Production code listens to `ThemeSettings.Changed` and refreshes the current page
using system text colour in contrast mode. The dividers now expose Thumb peers
with directional keyboard help.

Evidence: [GUI/input/behaviour](evidence/resume-input.json),
[behaviour summary](evidence/windows-behaviour.json),
[contrast screenshot](evidence/windows-high-contrast.png).
The managed RichEdit peer alone does not expose its native child's Text pattern;
the audit checks its named read-only Edit role, not a fabricated managed provider.
An external-client/Narrator review is still required. One VM at 125% scale is not
multiple-DPI, multi-monitor, human accessibility or folder-selection acceptance.

## Cross-platform shared-engine results

All 27 repositories matched `tools/corpus.json`'s pinned revisions. The same 6,297
regular candidate files were compared using complete helper JSON on macOS, Linux
and Windows. Every selected input SHA-256 matched; none failed analysis. The
portable snapshot excludes 63 duplicate Objective-C header symlink aliases, which
are explicitly listed as exclusions in the comparison. Their ordinary source
targets remain covered. The original capture contains 6,360 entries; missing
aliases on Linux/Windows are input-extraction failures, not parser crashes.

Only absolute snapshot-root prefixes and separators are normalized. No JSON
fields or array order are discarded. Strict comparison **does not pass**:

| Comparison against macOS | Differing files |
|---|---:|
| Linux | 9 |
| Windows | 10 |

The union contains 19 files. A second serial capture of all 19 reproduced the
same differences, with no failed inputs. Recursively sorting arrays did not make
the differing payloads equal, so these are not merely output-order differences.

- Windows/macOS: eight Alamofire Swift tests differ in recovery output and/or
  call-site facts; two cJSON Ruby tests differ in resolved dependency paths.
- Linux/macOS: one Alamofire Swift test differs in recovery/provenance output;
  two CodeIgniter PHP files differ in resolved paths; six Okio Kotlin files
  differ in dependency/binding output.
- PHP's case-sensitive/case-insensitive filesystem behaviour explains at least
  one resolved-path difference. Do not assume it explains the other cases.
- The shared AST repair routine has a deterministic trial count plus a 1,500 ms
  safety time cap. Profile the differing Swift cases before attributing them to
  that cap, compiler behaviour or Qt/library versions. Root causes remain open.

Evidence: [full selected comparison](evidence/cross-platform.json),
[serial reproduction](evidence/cross-platform-serial.json),
[19-file manifest](evidence/cross-platform-differing-files.json),
[capture/input hashes and pinned revisions](evidence/cross-platform-inputs.json).
Raw gzip JSON captures and corpus snapshot are preserved outside Git on Kubuntu:
`/home/user/Code/Coding/lumencode-validation-20261017/`.
Windows also retains captures in `C:\Users\leeha\lumencode-cross-*.json.gz`.
The macOS/Linux helper builds used the current canonical sources; the Windows
package's earlier native-build commit has matching current engine source hashes.

Use `tools/cross_platform_audit.py capture --cli ... --root ... --manifest ...
--output capture.json.gz --jobs 1`, then `compare --reference mac.json.gz
linux.json.gz windows.json.gz --output comparison.json`. Raw captures and strict
differences remain available; no parser/grammar changes were made to hide them.
This pass does not compare full project indexes or damaged-source sweeps across OSes.

## Portable distribution

`tools/package_windows.ps1` creates an unsigned, self-contained portable ZIP with
launch instructions, notices/provenance, a per-file SHA-256 manifest and archive
checksum. It refuses to overwrite an existing archive. Metadata records source
commit and whether the checkout was dirty at packaging time.

`tools/test_windows_portable.ps1` verified all 565 packaged file hashes, extracted
the ZIP into a new path containing spaces, and passed 47 default GUI checks with
developer discovery paths removed from the child process. The test archive was
created from the development working tree and records that fact; it is not a
signed release. The user's PATH and environment were not changed.

See [portable result](evidence/portable.json). Windows Sandbox is disabled and
`WindowsSandbox.exe` is absent on this VM; no optional features were enabled and
no reboot was attempted. Installed system components remain present, so the test
does not establish operation on a clean machine. An installer, trusted signing
certificate, SmartScreen behaviour and clean-machine acceptance remain outstanding.

## Resume order

1. Inspect this document, `git status` and the latest CI result. Use the existing
   Windows EXE at `build-windows/package/LumenCode.Windows.exe`.
2. Continue rendered/input latency work; paging reduced the measured insertion
   stall but did not establish the general responsiveness goal.
3. Perform human Narrator/keyboard and real DPI/monitor checks on suitable desktops.
4. Investigate the 19 reproduced corpus files in the canonical engine and add
   minimal fixtures for any correction. Keep host-filesystem differences explicit.
5. Run the portable ZIP on a clean Windows installation before choosing/publishing
   an installer or arranging trusted signing. Nothing is merged or released here.

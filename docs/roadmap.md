# Roadmap: closing the known gaps (from 2026-09-30)

This plan re-examines the gaps listed in the README (Roadmap and Known
Issues) against measurements on the pinned public corpus (`tools/corpus.json`,
~6.4k files), and orders the work so each step unlocks the next.

## What the measurements say

- **Cross-file relations are the structural gap.** Same-file `Calls` /
  `Called By` are close to fully reciprocal (7 of 2,007 sampled edges lacked a
  reverse). Cross-file relations exist only for JS/TS, and only by re-parsing
  up to 24 imported + 48 importing files on each selection (1.5 s budget).
  Everything else on the list — `fetch()` → routes, PHP class calls, Python
  imports, C++ header ↔ source, search, entry-point guessing — needs a
  project-wide view.
- **Coverage holes:** Kotlin (313 corpus files), Ruby (164) and LESS/SCSS (71)
  are unsupported; Objective-C has no relations; Rust and Swift signatures are
  still snippet-derived (24% / 31% of callables have parameters).
- **Shallow output:** 37% of TS/JS files yield no symbols — mostly test files
  (`describe`/`it` not surfaced), barrel/re-export modules and page scripts
  made of event-listener callbacks. 27% of PHP files are templates or config
  arrays with no symbols.
- **Resilience:** damage-probe retention is 97.2% overall; the weak spots are
  unclosed strings (Swift 59%, Java 87%, Rust 91%) and missing closing braces
  (blanking cannot fix them). 19% of valid Swift and 69% of C/C++ files trip
  the grammars (grammar gaps, unexpanded macros).
- **GUI:** the new link types and the known QML binding failures have not been
  checked on screen.

## Phases

Each phase is gated by: fixtures (`tools/regression_sweep.py
--fixtures-only`), `tools/corpus_scan.py --compare`, `tools/damage_probe.py`,
and an AddressSanitizer corpus run; README and implementation log updated
before pushing.

### Phase A — quick wins and a GUI safety net
1. Headless GUI smoke test: load `Main.qml` offscreen and fail on QML
   warnings/binding errors.
2. Shallow TS/JS: test blocks (`describe`/`it`/`test`) as symbols,
   re-exports as symbols linked to their target, top-level event listeners
   as handlers.
3. Rust and Swift signatures from the syntax tree.
4. Objective-C call relations.

Targets: TS/JS files with symbols 63% → >85%; Rust/Swift parameter coverage
>70%; offscreen GUI smoke test green.

### Phase B — project index (keystone)
5. Background, cached per-file fact index (definitions/exports, resolved
   imports, call names, routes, HTTP client calls, DOM references), built
   through the crash-isolated helper and cached by mtime.
6. Cross-file `Calls` / `Called By` for every language from the index,
   replacing the per-click re-parse.
7. Corpus metrics for cross-file relation coverage, cross-file reciprocity
   and index build time.

### Phase C — cross-language links on the index
8. `fetch` / `axios` / `$.ajax` / form actions → backend routes (Express,
   Flask, PHP, Go, ASP.NET, Spring).
9. PHP calls into `use`-resolved classes.
10. C/C++ header declaration ↔ source definition.
11. Python imports resolved to local modules/packages.

### Phase D — resilience round two
12. Repair by insertion (missing `}` at indentation drops).
13. String-aware repair (blank from an unclosed quote to end of line).
14. C/C++ macro pre-pass (export/visibility macros, `Q_OBJECT`-style).
15. Newer vendored Swift grammar; re-measure the false-error rate.

Target: damage-probe retention ≥95% for every language and mutation.

### Phase E — breadth
16. Kotlin and Ruby as structural parsers.
17. LESS / SCSS as stylesheets.
18. Config-shaped files (PHP config arrays, `tsconfig.json`,
    `composer.json`, `appsettings.json`) summarised.

### Phase F — GUI work (needs the desktop)
19. Search / filter across files and symbols, on the index.
20. Visual treatment for the new data: broken-link entries, damaged-line
    markers, SQL relation labels.

## Progress

Progress is recorded per phase in [`implementation-log.md`](implementation-log.md).

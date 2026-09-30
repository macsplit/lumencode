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

Phases A–F were planned from the README's gaps; Phase G was added on
2026-09-30 from a review of LumenCode as an agent tool.

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

### Phase G — LumenCode as a tool for coding agents

The backend is a multi-language structural analysis engine with a headless
twin, `lumencode-cli`, that already emits machine-readable JSON:
`--dump-file <path>` (one file's analysis, ~50 ms for a 1,900-line file),
`-i` (a stateful JSON-command session) and `--debug-ast <path>` (parser
error nodes and repair outcome).

**Why it suits agents.** Agents mostly explore code with grep and whole-file
reads, which wastes tokens and misses structure. LumenCode offers:
1. Compact structure instead of file bodies: symbols, members, kinds, line
   ranges, signatures and dependencies, so an agent can read only the lines
   it needs.
2. A call graph (`Calls` / `Called By`) — "what breaks if I change this?",
   "where is this used?" — cheaper than grep.
3. Route extraction (Express, Flask/FastAPI, Spring, JAX-RS, ASP.NET,
   Laravel, Slim, gin/chi/echo, net/http): "where is the endpoint for X?".
4. Cross-language web links: does this id/class exist, which CSS classes
   are unused, which handler calls which function.
5. SQL relations: FK references; which views/routines read and write which
   tables.
6. Tolerance of broken code with explicit trust levels (`analysisSourceMode`,
   `analysisConfidence`, per-symbol confidence) — suited to agents editing
   half-finished files (damage probe: 97% of other declarations survive an
   injected syntax error).
7. Many languages in one tool, including ones others skip (QML, VB.NET,
   shell).
8. Robust and fast: crash-isolated helper, bounded crawls and file sizes,
   minified files skipped with a notice.

**Forms it could take**

| Form | Fit | Effort |
|---|---|---|
| MCP server wrapping the CLI (`outline_file`, `find_callers`, `list_routes`, `project_summary`, `web_links`) | Best fit — the natural way to give Claude Code and other agents the tools | Medium: a thin Python/Node wrapper over `--dump-file` / `-i` that trims output |
| Claude Code skill (a `SKILL.md` saying when to run `lumencode-cli --dump-file` and how to read the JSON) | Good and cheap; no server, but the binary must be installed | Low: mostly a prompt plus a small jq/Python filter |
| Pre-/post-edit hook (outline before edits, check reciprocal call edges after) | Good complement | Low–medium |
| Regression and safety tooling (`regression_sweep.py`, `damage_probe.py`, the pinned corpus) | Useful for evaluating any parser tooling; not an agent tool itself | None |
| Library embedded in an agent harness | Possible, but the Qt dependency makes it heavier than the CLI route | High |

**Gaps to fix first**
- *Output size.* One file's dump was ~140 KB: snippets repeat, and each call
  edge nests the full symbol. Raw, that defeats the token savings. Add a
  slim outline mode (no snippets; names, lines, signatures, edge names only)
  — as a CLI flag or in the wrapper.
- *No project-wide query.* Agents want "all callers of foo", "all routes".
  A batch mode (`--dump-dir`) or a project-wide symbol index — this is what
  **Phase B** builds; Phase G exposes it.
- *Build and install.* Qt 5.15, KF5 and CMake are needed, with the CLI as a
  separate target. The CLI should not need the KF5 GUI stack: add a CLI-only
  build option and a packaged headless CLI (there is a Flatpak manifest for
  the GUI only).
- *Accuracy caveats.* Calls are name-based, not type-resolved; semantic
  refactoring, LSP and Git are non-goals. Agents should treat edges as strong
  hints, guided by the confidence fields. (Since this was written:
  same-file edges are ~99.7% reciprocal on the corpus, Objective-C has call
  relations, and Phase B/C target cross-file reciprocity.)
- *Overlap with LSP.* For a single mainstream language an LSP server is more
  precise. LumenCode's edge is breadth, tolerance of broken code, speed with
  no project setup, and the cross-language web and SQL views.

**Recommended order.** Start with a skill plus a small filter script that
calls `--dump-file`, drops snippets and nested edges, and prints an outline —
enough to see quickly whether agents benefit. If they do, add an MCP server
on top of the Phase B project index (batch queries: callers, routes,
symbols), which is what would make it worth more than grep. Include a
CLI-only build option alongside.

## Progress

Progress is recorded per phase in [`implementation-log.md`](implementation-log.md).

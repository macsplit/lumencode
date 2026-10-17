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
2026-09-30 from a review of LumenCode as an agent tool, Phase H the
same day from a user report (WebForms pages missing from the tree), and
Phase I the same day from using the agent skill on a real VB.NET / WebForms
task.

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

### Phase H — ASP.NET WebForms markup (added 2026-09-30)

Reported from real use: exploring a WebForms project shows the `.aspx.vb`
code-behind files but not the `.aspx` pages they belong to. Measured on the
code: `.aspx`, `.ascx`, `.master`, `.ashx`, `.asmx` and `.asax` are not in the
supported-suffix lists (`src/filesystemmodel.cpp`, `src/projectindex.cpp`,
`src/symbolparser.cpp`), so they are filtered out of the tree entirely and
the code-behind's markup half is invisible.

1. **Show them.** Add the WebForms suffixes to the file-type lists so the
   tree lists them (a first, structure-free step that already fixes the
   report).
2. **Markup parser** (a structural parser like the VB.NET and SQL ones):
   page directives (`<%@ Page|Control|Master ... CodeBehind / CodeFile /
   Inherits / MasterPageFile %>`, `<%@ Register %>`, `<%@ Import %>`),
   server controls with `runat="server"` as symbols (tag, `id`, type such as
   `asp:GridView`), `<asp:Content>` / `ContentPlaceHolder` regions,
   `<script runat="server">` blocks (VB or C#, handed to the matching
   parser), inline `<% %>` / `<%= %>` / `<%# %>` expressions, and HTML ids
   and class usage as the HTML parser already reports them.
3. **Links to the code-behind.**
   - Page ↔ `.aspx.vb` / `.aspx.cs` / `.designer.*` as related files, both ways.
   - Event attributes (`OnClick="Save_Click"`, `OnItemDataBound`, ...) get
     `Calls` edges to the handler methods, and those methods get
     `Called By` back to the control, like HTML `on*` handlers today.
   - Control ids resolve to the designer fields; ids the code-behind uses
     but the markup lacks (and the reverse) are flagged like broken DOM ids.
   - `MasterPageFile`, `<%@ Register Src=... %>` user controls and
     `Inherits` become dependencies and cross-file links on the project index.
4. **Verification.** A `webforms_basic` fixture (page, master, user control,
   VB and C# code-behind, one broken handler reference), manifest assertions,
   a public WebForms repository in `tools/corpus.json`, damage-probe and
   smoke-test coverage, and README / end-user-guide updates.

Out of scope for now: Razor (`.cshtml` / `.vbhtml`), MVC view resolution and
`Web.config` beyond what the JSON/config outlines already do.

### Phase I — agent-use findings from a real VB.NET / WebForms task (added 2026-09-30)

Source: the `lumencode` skill (`tools/agent/SKILL.md`, `lumencode-cli` from
this checkout) was used to plan a feature in a private multi-project VB.NET /
ASP.NET WebForms code base (two WebForms apps sharing `Shared/` code through
same-named shim classes, a VB class library, and a separate SQL Server schema
folder with one file per stored procedure). The task was to trace an
"end of year" report form down to its stored procedure and list every call
site that a parameter change would touch. The outline, `--find` and `--search`
commands saved several whole-file reads. The call-graph commands were not
reliable enough to replace grep for the impact check, which is the use the
skill recommends them for. Each item below was reproduced on that project;
file names are given so the cases can be rebuilt as fixtures (the project
itself is not public).

**Wrong output (fix first)**
1. **Multi-line VB string literals end the enclosing method early and leak
   their contents as symbols.** A `Function` whose body appends a
   multi-line string containing JavaScript (`html.Append("` ... `")`) is
   reported as spanning only up to the line where the string starts
   (`BuildHtml L18-30`; the real extent is L18–161). The JavaScript
   functions inside the string (`updateHiddenInvoices`,
   `getUncheckedInvoices`, `restoreUncheckedState`, ...) are listed as
   VB.NET methods with invented `calls` / `called by` edges. Expected:
   string contents are blanked by the VB noise pass, the method runs to its
   `End Function`, and nothing inside the literal becomes a symbol. Likely
   in `parseVbNet` noise blanking (VB strings escape only with `""`, and
   since VB 14 may span lines). Fixture: `vbnet_multiline_string`
   (function with a multi-line string literal containing braces, quotes,
   `function` and `End Function` text, followed by a second method).
   Add the same shape to the VB.NET cases of `tools/damage_probe.py`.
2. **Qualified calls to a class that exists in more than one project are
   dropped.** `AccountingHost.GetInvoices(...)` and
   `AccountingHost.GetGarageConfiguration(...)` are called from the shared
   report builder and both apps' pages, yet `--callers` and `--callees` list
   none of them. `AccountingHost` is defined once per app (same name and
   members, different project), so the owner is ambiguous. A qualified call
   to a unique class in the same index (`BatchJob.AllocatePort`) resolves as
   `medium`, which suggests ambiguity suppresses the edge instead of
   producing candidates. (Cause not confirmed; check in
   `src/projectindex.cpp` before relying on it.) Expected: link to every
   candidate owner with a lower confidence, or prefer the candidate in the
   caller's own project (nearest `.vbproj` / `.csproj` / `.sln` ancestor;
   files under `Shared/` that belong to neither match all candidates).
   Same-named facades or shims are a common way to share code between a
   legacy and a new app, so this is not specific to one code base. Fixture:
   two projects each with a `Facade` class of the same name and a shared
   file calling `Facade.Run()`.
3. **A silent empty result reads as "no callers".** `--callers` /
   `--callees` for a name whose calls were dropped (items 1 and 2, or
   unknown receiver types) print only the definition lines. The skill
   documents the caveat, but the tool output gives no sign of it, and an
   agent checking "what breaks if I change this signature" is told nothing
   uses it. Expected: when the name has call sites that were not linked,
   say so in the output (for example `note: 4 unlinked call sites by name;
   confirm with grep`), and offer the name-only list (item 4).

**Missing capability (agent-facing)**
4. **Name-only fallback for callers.** Index textual call sites of a name
   without resolving the receiver, and expose them as a separate,
   clearly low-confidence tier (`--callers <name> --loose`, or appended
   under `unresolved:` in the normal output). Calls are matched by name by
   design; today a call whose receiver cannot be resolved is left out, so
   the agent has to fall back to grep. Whether the per-file call-site facts
   (Phase B) already keep the unbound ones needs checking; if they do, the
   change is to surface them. This is the single change that would have
   made `--callers` usable for the impact check.
5. **VB.NET / C# → SQL stored-procedure links.** All data access in the
   project above goes through `CommandType.StoredProcedure` with the
   procedure name in a string (`objCommand.CommandText =
   "dbo.Invoice_GetByGarageWithDates"`). `--find` locates the procedure
   when the schema folder is indexed, but nothing connects the calling
   method to it, so "who calls this procedure?" and "which procedure does
   this method use?" need grep. Expected: record string literals assigned
   to `CommandText` / `SqlCommand` constructors (and `EXEC`, Dapper-style
   calls) in the facts, and link them to `procedure` symbols by name
   (schema prefix optional), as HTTP calls are linked to routes. Also
   allow more than one project root, because here the schema lives in a
   sibling folder outside the indexed code root (`--index-project` takes one
   root; an `--also-index <dir>` option or a roots file would do). Fixture:
   a VB file plus a `.sql` procedure file.
6. **Overloads are not distinguished.** `--find GetInvoices` returns 15
   definitions across five layers with no signature, and `--callers` merges
   all overloads of the name. Show the signature (or parameter count) on
   `--find` / `--callers` text lines, and narrow edges by argument count
   where the call site's count is known. This matters most in layered VB
   code, where a wrapper has the same name and one extra parameter at each
   layer.

**Smaller defects**
7. **`--search` does not handle multi-word queries.** `--search "end of
   year"` returns nothing although `EndOfYear` exists; `--search EndOfYear`
   finds it. Split on whitespace and match each token against the words of
   camelCase names and path segments, ranking by tokens matched. Print
   `no matches` (stderr) on an empty result so it is distinguishable from an
   index that did not load; it currently prints nothing and exits 0.
8. **SQL signature display loses string defaults.** The procedure parameter
   `@SortName VARCHAR(30) = 'InvoiceDate'` is shown as
   `@SortName: VARCHAR(30) = ' '` (the blanked form of the literal leaks
   into the signature). Use the original text for display.
9. **Markup event links point at line 1.** For `Handles ButtonSubmitEOY.Click`
   and `Me.Load` (Phase H), the markup side of the relation reads
   `ButtonSubmitEOY.Click @ EndOfYear.aspx:1`. Use the control's line in the
   markup (or the `Handles` line for `Me.Load`).

**Agent tooling**
10. **Exercise the edit-check hook on a VB.NET signature change.** The
    hook's caller list comes from `--callers`, so items 2 and 4 limit what
    it can report on this kind of code base. This was not tried in the
    field session (no edits were made). Add a scripted case (change a
    wrapper's signature in a fixture project, run `pre` and `post` with a
    JSON event on stdin, assert the caller is reported) to the automated
    hook tests still listed as open under Phase G.
11. **Installation as a user skill.** The skill only appears to an agent
    once `tools/agent/` is copied or linked into `~/.claude/skills/lumencode/`;
    the README describes the CLI build but not this step, and
    `hooks/settings.example.json` contains `/path/to/lumencode` placeholders.
    Document the copy (or add `tools/agent/install.sh` that copies or
    symlinks the skill and prints the hook settings with the real path), and
    note that a copy does not follow later updates to the checkout.

**Verification.** Fixtures for items 1, 2, 5 and 9 in
`tests/fixtures/baseline/` with manifest assertions; `tools/regression_sweep.py
--fixtures-only`, `tools/corpus_scan.py --compare` and `tools/damage_probe.py`
as in the other phases; re-run the original task (trace a form to its stored
procedure and list the call sites of a layered method) as the acceptance
check, comparing `--callers` against grep.

## Progress

Progress is recorded per phase in [`implementation-log.md`](implementation-log.md).

- **Phase I — in progress (2026-09-30).** Completed: item 1 (multi-line VB
  strings); item 3 (unlinked-site warning); item 4 (`--callers --loose`);
  item 7 (multi-word search and empty-result message); item 8 (SQL string
  defaults); and item 11 (skill installer/docs). Item 2 now prefers a
  same-project qualified target and exposes shared-folder ambiguity through
  `--loose`, but does not yet emit edges to every viable project candidate.
  Item 5 supports CommandText/SqlCommand and common Dapper stored-procedure
  calls across `--also-index` roots; broaden it to further data-access forms
  as they arise. Item 6 displays signatures but still needs argument-count
  disambiguation. Item 9 resolves server controls to their markup line, while
  `Me.Load` still needs a code-behind Handles-line representation. Items 10
  (automated hook test) and the remaining verification/corpus acceptance
  checks are open.

- **Phase H — in progress (2026-09-30).** The initial visibility slice is
  complete: `.aspx`, `.ascx`, `.master`, `.ashx`, `.asmx` and `.asax` are
  included in the explorer, project index, CLI/regression candidates and GUI
  smoke coverage. They currently use the existing HTML-compatible markup
  analysis, plus WebForms directives, server controls, content regions,
  server-side blocks and inline expressions. Markup now links to conventional
  code-behind/designer files, and server-control events bind to matching local
  C# or VB methods in both directions. VB.NET `Handles Control.Event` clauses
  are surfaced on the markup side even when no `On*` attribute is present.
  Missing handler and control/designer diagnostics remain the next work.

- **Phase A — done (2026-09-30).** GUI smoke test green (fixtures and a
  141-file corpus sample, no QML warnings); TS/JS files with symbols
  63% → 93%, JS 65% → 87%, TSX 59% → 94%; Rust and Swift signatures from the
  syntax tree; Objective-C relations 0% → 67% of files.
- **Phase B — done (2026-09-30).** Project index with cross-file `Calls` /
  `Called By` for 14 languages, cached, built in the background; corpus
  numbers in the implementation log. HTTP-client calls and DOM references
  in the facts moved to Phase C, where they are consumed.
- **Phase C — done (2026-09-30).** HTTP client calls → routes (fetch, axios,
  jQuery, XHR, forms; Express mounted routers, Flask views); PHP `use` and
  Java imports bound to classes; C/C++ prototypes paired with bodies;
  Python imports resolved to modules with re-exports followed. DOM
  references were already served by the web link model.
- **Phase D — done (2026-09-30).** Suspect-line repair, lost-brace repair
  (write back / dissolve), re-nesting of clean parses, C/C++ macro
  pre-pass. Target (≥95% retention for every language and mutation) met
  for garbage lines, unclosed calls and strings except TSX unclosed calls
  (88%); the new lost-brace mutation is 76–98% outside Swift (65%). The
  Swift grammar is already at the latest release (0.7.1).
- **Phase E — done (2026-09-30).** Kotlin (327 corpus files, 98% with
  symbols, damage probe 96.5%) and Ruby (164 files, 90%, 98.8%) as
  structural parsers; SCSS/LESS; JSON outlines and composer / tsconfig /
  appsettings summaries; PHP configuration arrays.
- **Phase G — done (2026-09-30).** `--outline` (about an eighth of a full
  dump; JSON or text), project queries on the index (`--find`,
  `--callers`, `--callees`, `--routes`), `tools/agent/SKILL.md`, an MCP
  server (`tools/agent/lumencode_mcp.py`, stdio, no dependencies) and a
  headless `-DLUMENCODE_CLI_ONLY=ON` build that needs only Qt 5 base.
  Later additions: MCP `project_summary`, `web_links` and `search_symbols`
  tools, and edit hooks (`tools/agent/hooks/`: a silent pre-edit snapshot and
  a post-edit check reporting new syntax damage, removed or re-signatured
  functions with their callers). Left open: packaging the CLI (distribution
  packages, a container image) until there is demand, and automated tests
  for the MCP server and the hook script.
- **Phase F — done (2026-09-30).** Search box on the project index
  (`AgentQueries::searchSymbols`, shared with `lumencode-cli --search` and
  the MCP `search_symbols` tool); warning styling for broken links, missing
  classes and unresolved dependencies; health marks on repaired / low
  confidence symbols and a damaged-lines note in the source pane; SQL
  relation verbs. Verified headless (smoke test drives the search field,
  screenshots in `LUMENCODE_SMOKE_SHOTS`); not yet looked at on a real
  desktop session. Found on the way: latent `implicitHeight` binding loops in
  the Overview / Detail cards that only appear with real rendering (see
  README Known Issues).

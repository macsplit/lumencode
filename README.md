# LumenCode

LumenCode is a structural code explorer for local source trees. It focuses on code shape rather than freeform text editing. The application reads a filesystem tree, extracts symbols from supported source files, and lets the user drill from folders to files to symbols to member detail.

LumenCode is intentionally:

- filesystem-aware
- parser-assisted
- non-editing
- non-Git-aware

![Screenshot](screenshots/screenshot.png)

## Guides

- [Docs Index](docs/README.md)
- [End-User Guide](docs/end-user-guide.md)
- [Developer Guide](docs/developer-guide.md)

## Current State

_Last updated: 2026-09-30._

LumenCode is a working desktop explorer (Qt 5.15 / KF5 Kirigami) with a
backend that is now developed and regression-tested almost entirely through
its command-line twin, `lumencode-cli`. The current milestone is still
**stabilization and trustworthiness of the inspection pipeline, before
search**. See [Recent History](#recent-history) for how it got here and
[`docs/implementation-log.md`](docs/implementation-log.md) for the detail.

### The product

- A dense three-pane explorer (tree / overview / permanent detail inspector)
  with a resizable lower source pane, draggable splitters, and a left control
  rail (back, open-in-folder, open-in-editor, settings).
- Drill-down from folders to files to symbols to members, with `Calls` /
  `Called By`, `Parameters` / `Returns`, dependencies, routes and quick links
  in the inspector. Every entry drives the lower pane, including entries that
  point into another file.
- Syntax-coloured snippets with conservative, parser-aware diagnostics
  (truncated previews never produce false errors).
- Project summaries (file-type counts, main entry point) and `package.json`
  summaries.
- Crash isolation and responsiveness: the GUI analyses each file by running
  `lumencode-cli --dump-file` in the background, so a parser crash only kills
  the helper. Directory crawls, file sizes and cross-file relationship scans
  are bounded, and hitting a bound shows a visible notice instead of hanging.
  Minified or bundled assets are skipped deliberately.
- A **project index** built in the background when a folder is opened (see
  [Project index](#project-index-cross-file-relations)): cross-file `Calls` /
  `Called By` in every language with call sites, cached on disk so reopening
  a project is near-instant.

### The analysis engine, per language

| Language | Parser | Syntax errors | Structure | Calls / Called By | Dependencies | Routes | Signatures |
|---|---|---|---|---|---|---|---|
| PHP | Tree-sitter | repair → heuristic | classes, functions, members | same-file, callbacks | `use` (resolved via composer PSR-4), `require`/`include` | Slim / Lumen, Laravel, CodeIgniter | **from the syntax tree** (types, defaults, variadics, promoted properties; declared or inferred returns) |
| JavaScript / JSX | Tree-sitter (heuristic fallback) | repair → heuristic | declarations, IIFE/UMD/AMD bodies, `X.prototype.m`, closure modules, `X.extend({...})` classes | same-file and cross-file (imports, `require` bindings) | `import`, `require` | Express (anywhere in the tree) | **from the syntax tree** (defaults, rest/destructuring; return paths inferred, `Promise<…>` for async) |
| TypeScript / TSX | Tree-sitter | repair → heuristic | as JS, plus interfaces and types | as JS | as JS | as JS | **from the syntax tree** (annotations, optional `?`, defaults, accessibility; declared return types) |
| Python | Tree-sitter | repair (indented blocks) → heuristic | classes, functions, methods, properties | AST walk; cross-file via resolved imports | imports resolved to local modules and packages (with bindings) | Flask / FastAPI | **from the syntax tree**: parameters with types/defaults, annotated or inferred returns per return path |
| Java | Tree-sitter | repair → heuristic | types, members | yes | imports | Spring (`@RequestMapping`, `@GetMapping`, ...), JAX-RS (`@Path` + verbs) | **from the syntax tree** |
| C# | Tree-sitter | repair → heuristic | types, members, top-level programs | yes | `using` | ASP.NET attributes / minimal APIs | **from the syntax tree** (`ref`/`out`/`params`/`this`, defaults, generics) |
| Rust | Tree-sitter | repair → heuristic | items, impls, modules | yes | `use` | — | **from the syntax tree** (patterns, types, return types; `self` receiver omitted) |
| Swift | Tree-sitter | repair | types, extensions, members | yes | `import` / `@testable import` | — | **from the syntax tree** (argument labels, defaults, variadics, attributes, `async`/`throws`) |
| Go | Tree-sitter | repair | structs (fields), interfaces (methods, embeds), types/aliases, functions, receiver methods grouped under their type, consts/vars | yes | imports (incl. aliases) | net/http (`HandleFunc`, Go 1.22 `"GET /x"` patterns), gin, chi, echo | **from the syntax tree** (grouped params, variadics, multiple results) |
| CSS | Tree-sitter | repair → heuristic | rules, custom properties | — | `@import`, `url()` | — | — |
| HTML | tree-sitter-html | tolerant grammar | ids, event handlers, custom elements, forms, inline `<script>` / `<style>` contents | handlers → JS functions | linked assets | — | — |
| QML | heuristic | — | components, properties, signals, functions, signal handlers | functions ↔ handlers (from handler blocks) | imports | — | snippet-derived |
| C / C++ | Tree-sitter (C++ grammar, used for C too; heuristic fallback) | repair → heuristic; macro-noisy files (>25 initial errors) go straight to the merge | namespaces, classes/structs/unions (access levels, bases, fields, methods, ctors/dtors, function-pointer fields), out-of-line `A::f` definitions grouped by scope, enums, typedefs, prototypes | yes | `#include` (resolved for local headers) | — | **from the syntax tree** (qualifiers, pointers/references, defaults) |
| Objective-C | structural parser (comment/string-blanked, bracket-matched) | bodies brace-matched within `@implementation … @end` | classes and categories, full multi-part selectors, `@property` declarations, C functions | yes (message sends matched by full selector; C calls) | `#import` | — | from declarations (`(type)name` keyword parts, return type) |
| VB.NET | structural line parser (block-aware) | unterminated blocks closed at the next declaration, reported | namespaces, classes, modules, structures, interfaces, enums, members, fields, events | yes (incl. `RaiseEvent`) | `Imports` (incl. aliases) | ASP.NET attributes (`<Route>`, `<HttpGet>`, ...) | **exact** from declarations (`ByVal x As T`, `Optional ... = v`, `As T`) |
| SQL (MySQL / MariaDB, SQL Server T-SQL) | statement-aware parser | objects end at the next `CREATE`/`ALTER`, `GO` or `DELIMITER`; strings cannot cross batches | tables (columns, keys), views, procedures, functions, triggers, indexes | table → table *references* (FKs); views/routines/triggers *read* / *write* tables; routines *execute* / *call* routines; triggers *fire on* tables | `USE`, `source` / `\.` / `:r` scripts | — | from declarations (`IN`/`OUT`/`@p ... OUTPUT`, defaults, `RETURNS`) |
| Shell (bash / sh / zsh) | structural parser | unbalanced bodies stop at the next function header; quoting (incl. nested `"$(… "…")"`), comments and here-docs handled | functions (all three forms), exported / readonly variables | yes (command position, incl. inside `$( … )`) | `source` / `.` (directory-prefix idioms resolved) | — | named from `local x="$1"`, else positional; `$@`; exit status / stdout |
| Kotlin | structural parser (comment/string-blanked, brace-matched) | a member's body cannot run past the next sibling declaration, a top-level one past the next column-0 declaration | classes, interfaces, objects (incl. companion), enums with entries, functions (type parameters, extension receivers, expression bodies), properties (incl. constructor `val`/`var`), typealiases | yes; cross-file via the index | `import` (resolved through the package's source root) | Spring mapping annotations, Ktor `routing { route { get { } } }` | from declarations (`name: Type = default`, `vararg`, return type) |
| Ruby | structural parser (`end`-matched lines) | a missing `end` is settled by indentation | modules, classes, methods (instance, `self.`, `class << self`, endless), `attr_*`, constants, RSpec / Minitest blocks | yes; cross-file via the index | `require` / `require_relative` (resolved) | Sinatra-style `get '/x' do` | parameter names, defaults, keyword and splat parameters |
| SCSS / LESS | structural parser | an unbalanced block ends with its parent | nested rules with resolved selectors, mixins, functions, placeholders, keyframes, variables | `@include` / `@extend` / LESS mixin calls, across files via the index | `@import` / `@use` / `@forward` (partials resolved) | — | mixin parameters and defaults |
| JSON | `QJsonDocument` (comments and trailing commas accepted) | — | keys two levels deep; `package.json`, `composer.json`, `tsconfig.json`, `appsettings.json` summarised | — | npm / composer packages, tsconfig `extends` / `references` | OpenAPI operations | — |

"repair → heuristic" is the parser authority model:

1. A clean syntax tree is authoritative.
2. If the tree has errors, a **branch-scoped repair** pass finds the lines the
   parser flags, blanks them (offsets and line numbers unchanged) and
   re-parses, keeping the damage local to those lines instead of losing the
   rest of the class or file. Because tree-sitter often blames an intact
   enclosing line, it also tries *textually suspect* lines (an unclosed
   call or string) and, for a **lost closing brace**, either writes the
   closers back into the next line's indentation or dissolves the unclosed
   block — each accepted only if the whole file then parses cleanly. A
   clean parse is also checked for declarations a lost brace swallowed
   (nested but not indented), which Swift's grammar accepts silently.
   C/C++ is parsed after a **macro pre-pass** (export, attribute and Qt
   macros blanked, `CJSON_PUBLIC(type)` unwrapped), kept only where it
   parses at least as well as the original.
3. Declarations that start on a blanked line (e.g. a half-typed `def` header)
   are recovered by the heuristic parser.
4. Only if the tree is still broken does the heuristic parser supplement it,
   and then only in ranges the tree does not cover.

Every payload says which of these happened (`analysisSourceMode`:
`ast` / `recovered` / `heuristic`, `analysisConfidence`,
`analysisDamagedLines`, per-symbol `sourceMode` / `confidence`).

### Project index (cross-file relations)

Opening a folder builds a project index in the background
(`src/projectindex.{h,cpp}`): per-file *facts* — definitions with their
owning type, resolved imports with their bindings, and call sites with their
receiver — are produced by the crash-isolated helper
(`lumencode-cli --index-facts`, in parallel batches; a crash costs only the
file it happened on) and cached on disk keyed by file size and mtime *and*
the helper build, so a parser upgrade re-indexes. Every call site is then
resolved once, most specific evidence first, and ambiguous names are left
unresolved rather than guessed:

| Evidence | Example | Confidence |
|---|---|---|
| import / include binding | `import { parseBody }`, `use parser::{Tokenizer}` + `Tokenizer::new`, `utils.normalizeType()` with `utils = require(...)`, `#include "geometry.h"` | high |
| receiver names the owning type | `Guard.NotNull`, `Mailer::deliver`, `[PriceFormatter stringForCents:…]`, `TaxRules.VatFor` | medium |
| same directory / package (bare calls and `new` only) | Go same-package calls, Python/Swift siblings, `new Invoice(...)` | medium |
| the only definition of a distinctive, non-generic name | `res.sendFile` → `response.js` | low |

A call on a receiver of unknown type can only reach a method, and only by a
distinctive name; names bound to external packages, standard-library method
names (`unwrap`, `containsKey`, `class`, …) and nested types named from
outside their owner are never linked. Edges are stored in both directions,
so `Calls` and `Called By` agree across files by construction. The open
file's own calls are resolved live against the index, so edits show up
before a re-index. Symbols with more than 400 cross-file callers list the
first 400 and report the total (`calledByTotal`).

Imports feed the first rule directly: JS/TS `import` / `require`, Python
imports resolved to local modules (relative, absolute from above the
enclosing package, `src/` layouts; package `__init__.py` re-exports are
followed), PHP `use` through composer PSR-4 (aliases included), Java imports
through the package's source root (test source sets reach `src/main`), Rust
`use`, C/C++ `#include`. In C/C++ a call that reaches a header prototype
lands on its body (paired by name and owning type, preferring the matching
source file); the prototype shows the body's callers and a `definition`
link, the body a `declaredIn` list, and a callee must be reachable through
the file's includes.

**HTTP calls → routes.** Browser-side calls — `fetch`, axios, jQuery
(`$.ajax`, `$.get`, `$.post`, `$.getJSON`), `XMLHttpRequest.open`,
Angular-style `http.get<T>()` and HTML / PHP `<form action method>` — are
matched to backend routes (Express incl. routers mounted with
`app.use('/prefix', router)`, Flask/FastAPI, Spring/JAX-RS, ASP.NET, PHP
frameworks, Go routers). Dynamic URL parts (`'/users/' + id`,
`` `/items/${id}` ``) and route parameters (`:id`, `<int:id>`, `{id}`) match
any segment; the most literal agreement wins, then a route in the calling
file, then non-test files. The calling function gets the route (and a
decorated view function, e.g. Flask's) in `Calls`; the route gets
`calledFrom`, and the view function `Called By`. Calls to other hosts are
treated as external APIs.

Call sites come from the syntax tree for TS/JS, Python, Java, C#, PHP, Rust,
Swift, Go and C/C++, and from the structural parsers for Objective-C
(message sends by full selector), VB.NET and shell (commands, including
top-level script code). QML, SQL, CSS and HTML are indexed for definitions
only (their links are the web and SQL models above/below).

### Cross-language links (web)

HTML, CSS and JavaScript are analysed as one linked model rather than three
separate files:

- **JS → HTML / CSS**: `getElementById`, `querySelector(All)`, `$('#x')`,
  `classList.*`, jQuery class methods, `className =` and
  `setAttribute('class', ...)` are resolved to the element or CSS rule they
  touch, and flagged when they resolve nowhere (e.g. `#footer — no such
  element in index.html`). Functions called from HTML `on*` handlers get
  `Called By` edges back to the handler. Custom elements registered with
  `customElements.define` link to the pages that use them.
- **HTML → JS / CSS**: handlers have `Calls` into inline or linked-script
  functions; ids list the scripts that use them; class usage is matched
  against linked, inline and sibling stylesheets.
- **CSS → HTML / JS**: consumer pages; classes applied only from JavaScript
  are recognised (`scriptAppliedClasses`) so they are not reported as
  unused; `unusedClasses` lists the rest.
- Consumer pages are found in the asset's folder or the nearest ancestor
  folder that links it. PHP templates count as pages, and PHP files that
  render HTML get the same asset links.

### Backend development without the GUI

The backend is iterated through the CLI. The GUI is only needed for purely
visual work.

```bash
cmake -S . -B build && cmake --build build --target lumencode-cli
./build/bin/lumencode-cli --dump-file path/to/file      # one file's analysis JSON (what the GUI shows)
./build/bin/lumencode-cli --debug-ast path/to/file      # Tree-sitter error nodes + repair outcome
./build/bin/lumencode-cli -i                            # scripted selection session (JSON commands on stdin)
./build/bin/lumencode-cli --index-project path/to/root  # build/refresh the project index, print its statistics
./build/bin/lumencode-cli --index-project root --index-edges            # every cross-file call edge (JSON lines)
./build/bin/lumencode-cli --index-project root --index-relations file   # one file's cross-file Calls / Called By

python3 tools/regression_sweep.py --fixtures-only       # first gate: 96 fixture cases, relation round-trips
python3 tools/fetch_corpus.py                           # pinned public corpus (27 repos, ~6.4k files)
python3 tools/corpus_scan.py --save before.json         # whole-corpus coverage / timing / contract scan
python3 tools/corpus_scan.py --compare before.json      # ... then diff after a change
python3 tools/damage_probe.py                           # how far an injected syntax error spreads
python3 tools/index_metrics.py                          # project index: coverage, build time, reciprocity
python3 tools/regression_sweep.py --corpus-root ~/.cache/lumencode-corpus --max-files 300

# The real GUI, headless: loads Main.qml offscreen and drives every symbol, relation,
# dependency, route and quick link of each file; fails on any QML warning.
cmake --build build --target lumencode-gui-smoke
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./build/bin/lumencode-gui-smoke tests/fixtures/baseline
```

- **Fixtures:** `tests/fixtures/baseline/` holds small projects per language
  cluster. They include broken-code recovery cases, Python return paths, a
  web app (HTML at the root, `css/` and `js/` below) and a composer/Slim PHP
  app, and `cross_file/` mini-projects in 11 languages for index-backed
  `Calls` / `Called By` (plus Python packages, Java source sets, composer
  PHP, and Express / Flask full-stack apps for HTTP → route links). `manifest.json` asserts symbols, relations,
  signatures, provenance, quick-link labels and CSS usage. The CLI builds the
  index before analysing (`--no-index` or `LUMENCODE_NO_INDEX=1` turns it
  off), so the fixture run covers it.
- **Corpus:** `tools/corpus.json` pins 27 public repositories by commit
  (web, JS/TS, Python, PHP, Java, C#, VB.NET, Rust, Swift, Go, C/C++, Ruby,
  Kotlin, Bash, SQL, QML, Objective-C), so sweeps work on any machine or in
  a cloud container. `$LUMENCODE_CORPUS` points the tools at it.
- **Damage probe:** injects a garbage line, an unclosed call or an unclosed
  string into one function of a clean file, and measures how many *other*
  declarations survive; a fourth mutation deletes a closing brace. After
  Phase D, retention per Tree-sitter language is 92–99.5% (Swift 89%);
  garbage lines, unclosed calls and unclosed strings are ≥ 90% everywhere
  except TSX unclosed calls (88%). A lost brace is the hardest case: TS
  98%, Java 94%, Rust and JS 92%, C++ 89%, Go and TSX 82%, C# 76%, Swift
  65%. Where it still fails, it is usually the last member of a type
  (nowhere to write the closer back without changing the file length).

## Build

Typical local build:

```bash
cmake -S . -B build
cmake --build build
```

`build/` is now ignored by git and is no longer tracked in the repository.

If KF5 development packages are missing from the CMake search path, CMake will fail during configuration. That is an environment issue, not an application logic issue.

On Ubuntu 24.04 / Kubuntu the build dependencies are:

```bash
sudo apt install cmake g++ extra-cmake-modules qtbase5-dev qtdeclarative5-dev \
    qtquickcontrols2-5-dev kirigami2-dev libkf5coreaddons-dev
```

For backend work only the CLI target is needed:
`cmake --build build --target lumencode-cli`.

**Headless build (no KDE Frameworks).** The CLI needs only Qt 5 base, so it
can be built on a server or in a container without ECM, Kirigami or QML:

```bash
sudo apt install cmake g++ qtbase5-dev        # Qt Core, Gui, Widgets, Concurrent
cmake -S . -B build-cli -DLUMENCODE_CLI_ONLY=ON
cmake --build build-cli                       # -> build-cli/bin/lumencode-cli
```

## LumenCode for coding agents

`lumencode-cli` doubles as a structural code-navigation tool for coding
agents (Claude Code and others): compact outlines instead of whole-file
reads, and project-wide "who calls / what calls / where is the endpoint"
queries on the project index.

```bash
lumencode-cli --outline src/app.ts --format text                     # symbols, signatures, line ranges, relations
lumencode-cli --index-project . --outline src/app.ts --format text   # ... with cross-file Calls / Called By
lumencode-cli --index-project . --find Cart.add --format text        # definitions (name or Owner.name)
lumencode-cli --index-project . --callers parseBody --format text    # same-file and cross-file callers, with confidence
lumencode-cli --index-project . --callees checkout --format text
lumencode-cli --index-project . --routes --format text               # backend routes <- the browser calls that reach them
```

An outline is roughly an eighth of a full `--dump-file` (a 1,300-line C++
file: 13 KB of JSON instead of 111 KB, less as text). Two ready-made
integrations live in `tools/agent/`:

- `SKILL.md` — a Claude Code skill: when to use which command and how to
  read confidence levels and damaged lines. Copy it to
  `.claude/skills/lumencode/SKILL.md` in a project (or `~/.claude/skills/`).
- `lumencode_mcp.py` — an MCP server (stdio, no third-party packages) with
  `outline_file`, `find_definition`, `find_callers`, `find_callees`,
  `list_routes`, `project_summary`, `web_links` and `index_stats`:
  `claude mcp add lumencode -- python3 /path/to/lumencode/tools/agent/lumencode_mcp.py`
  (`LUMENCODE_CLI` / `LUMENCODE_ROOT` override the binary and default root).

## Recent History

- **2026-03 — bootstrap.** Qt/Kirigami explorer, heuristic parsers for the
  main languages, `lumencode-cli` with one-shot and interactive modes, lower
  source pane with internal highlighting.
- **2026-04 — AST and relations.** Tree-sitter for PHP, TS/TSX, CSS, Python,
  Rust, Java, C# and Swift, with vendored grammars so fresh clones build.
  `Calls` / `Called By` with rehydration of relation clicks. Crash-isolated
  helper process and asynchronous GUI analysis.
- **2026-06 — trust.** Checked-in fixture suite and corpus sweep. Provenance
  and confidence on every payload. Parser-owned callable signatures. The
  deliberate `recovered` mode for broken files (a first parser-wide
  error-node walker was backed out as unstable). CSS/HTML noise reduction and
  a single-pass CSS class index.
- **2026-09 — breadth and resilience** (branch `claude/cool-dijkstra-1pnw8f`):
  - a pinned public corpus, a whole-corpus scanner and the damage probe, so
    work no longer depends on a private `~/Code` tree (these found and fixed
    a null-node crash in the TS parser)
  - branch-scoped AST repair for every Tree-sitter language, with Swift and
    CSS brought into recovery; damage-probe retention went from 91% to 97%
  - plain JS/JSX moved onto the Tree-sitter path, with real-world JS shapes
    (wrappers, prototypes, closure modules, class factories, nested routes)
  - Python signatures from the syntax tree, with return types described
    across all return paths
    ([issue #1](https://github.com/macsplit/lumencode/issues/1))
  - the HTML ↔ CSS ↔ JS link model on tree-sitter-html; HTML files went from
    no symbols to 78% of corpus pages with structure
  - PHP dependencies (PSR-4-resolved `use`, `require`/`include`), framework
    routes and template asset links (previously none)
  - new languages: VB.NET and SQL (MySQL and T-SQL) with purpose-built
    structural parsers, Go on Tree-sitter, and shell scripts; C/C++ moved
    onto Tree-sitter (members and call edges, previously none)
  - signatures from the syntax tree for TS/JS, C#, Java, PHP, Go and C/C++;
    Swift imports, Java Spring/JAX-RS routes and QML call relations

- **2026-09-30 — roadmap phases A and B:**
  - `lumencode-gui-smoke`: the real `Main.qml` driven headless through every
    fixture (and a corpus sample), failing on any QML warning
  - TS/JS structure for test files (`describe`/`it` blocks and hooks),
    barrel/re-export modules and page scripts (event-listener handlers)
  - Rust and Swift signatures from the syntax tree; Objective-C full
    selectors and call relations
  - the project index: cross-file `Calls` / `Called By` for 14 languages
    (previously JS/TS only, by re-parsing neighbours on each click)
  - C# classes inside block-scoped `namespace X { }` were dropped entirely
    (found by the new cross-file fixtures; C# files with symbols 91.7% →
    97.5% on the corpus)
- **2026-09-30 — roadmap phase E:** Kotlin and Ruby (structural parsers
  with routes, imports and index call sites), SCSS/LESS (nested rules,
  mixins across files), and config files (JSON outlines; composer,
  tsconfig and appsettings summaries; PHP configuration arrays). Corpus
  files with symbols: JSON 18% → 84%, PHP 73% → 91%, Kotlin and Ruby
  from none to 98% and 90%.
- **2026-09-30 — roadmap phase D:** repair from textually suspect lines,
  lost-brace repair (write the closers back or dissolve the block),
  re-nesting of clean Swift parses, and a C/C++ macro pre-pass (clean AST
  for 206 of 437 corpus C/C++ files, up from 126; `cJSON.h` 3 → 81
  symbols). Damage-probe retention: TS 95.7 → 98.9%, Swift 88 → 89% with the
  new lost-brace mutation included (97% without), C# 93 → 97% on the
  original mutations.
- **2026-09-30 — roadmap phase C:** Python imports resolved to local
  modules (and package re-exports followed), C/C++ prototypes paired with
  their bodies, PHP `use` and Java imports bound to their classes, and
  browser HTTP calls (`fetch`, axios, jQuery, XHR, forms) linked to backend
  routes

## Roadmap
Next major milestone: stabilization and trustworthiness of the inspection pipeline, before search.

**Current plan:** [`docs/roadmap.md`](docs/roadmap.md) re-examines the gaps below against corpus
measurements and orders the next work in phases A–F: quick wins and a headless GUI check, a
project index for cross-file relations, cross-language links on that index (e.g. `fetch()` →
backend routes), a second round of syntax-error resilience, Kotlin/Ruby/LESS/SCSS, GUI work, and
(Phase G) packaging the engine as a tool for coding agents: a slim outline mode, a skill, an MCP
server on the project index, and a CLI-only build.
The longer-standing phase list follows.

Phase 1. Stabilization

- Remove remaining QML/runtime edge-case binding failures. (2026-09-30: `lumencode-gui-smoke` drives the real GUI headless through all fixtures and a 141-file corpus sample — 3,200+ selections — with no QML warnings; keep it as the gate for QML changes.)
- Harden all selection payloads so every detail section is safe to bind.
- Reduce misleading or noisy structural output on real projects.
- Continue broad CLI-driven regression sweeps: the pinned public corpus (`tools/corpus.json`) plus local projects under `/home/user/Code`.
- Keep growing the baseline fixture suite so each supported language or language-cluster has a small structural repro project checked into the repo.
- Continue AST-backed parity work language by language, using CLI-first verification before GUI iteration.
- ~~Continue the authority/recovery refactor language by language~~ **Done (2026-09):** every Tree-sitter language now goes through one AST-first path with branch-scoped repair; keep driving the damage-probe retention up (worst remaining: Swift and Java unclosed strings).
- Keep tightening the current range-aware recovery model language by language, using fixture-gated refinements rather than broad parser-wide error walkers.
- Move callable signature extraction from parser-layer snippet heuristics to grammar-specific AST fields language by language. **Done (2026-09) for Python (issue #1), TS/JS, PHP, Java, C#, Go, C/C++, Rust and Swift**.
- Keep pragmatic heuristic coverage for valuable local languages such as QML where a dedicated grammar path is not yet integrated, instead of leaving them unsupported.
- Continue converting cross-file relationship work from name/snippet luck into explicit binding-aware or asset-aware models. **Web (HTML/CSS/JS) and PHP (`use` via PSR-4) done (2026-09)**; next: calls into `use`-resolved PHP classes, JS `fetch()` / `axios` calls to backend routes (Express, Flask, PHP), Python package imports.
- Treat surfaced analysis warnings as investigation leads, not just acceptable noise: some will indicate algorithmic or integration weaknesses rather than merely large inputs.

Phase 2. Better source inspection

- Improve snippet diagnostics beyond the current lightweight parser-aware checks.
- Continue improving context selection for dependencies, routes, and quick links in the lower pane.
- Support clearer line-focused navigation and open-in-editor behavior from overview/detail items into the lower pane.
- Rehabilitate the native Tree-sitter-backed parser paths language by language, using the crash-isolated CLI flow and minimized repro files. **Plain JS/JSX done (2026-09)** after the corpus scan found and fixed the underlying crash. Keep fallback and helper isolation in place.
- Tighten relationship extraction so `Calls` and `Called By` behave consistently and reciprocally across supported languages.
- Add more genuinely useful inspector content for sparse symbol types, especially Swift functions, once the backend payloads are stable enough to trust.
- Keep replacing snippet-luck relation detection with proper AST walks where real projects prove the bounded-snippet fallback is too weak, as happened with Python docstring-heavy files.
- Decide what richer right-pane payload should exist for sparse Swift/function selections now that the pane is permanently present and relation/navigation data is becoming more reliable.

Phase 2b. Language breadth and link parity

- ~~Add new languages seen in the corpus: Kotlin and Ruby~~ **Done (2026-09-30)** with structural parsers (their Tree-sitter grammars are ≈23 MB and 15 MB of generated source), plus SCSS/LESS and config files. (Go, AST-backed C/C++, shell, VB.NET and SQL done earlier in 2026-09.)
- ~~Close per-language link gaps: Swift imports, Java routes, Objective-C call relations, QML relations~~ (done 2026-09).
- Use `tools/corpus_scan.py --compare` and `tools/damage_probe.py` as the acceptance gates for each language.

Phase 3. Better usability

- Search/filter across files and symbols.
- Improve visual hierarchy and information density further without sacrificing clarity.
- Continue refining the left global control rail and settings surface.

Phase 4. Broader project understanding

- Improve Node/CommonJS and service-repo structure understanding further.
- Refine HTML/CSS class analysis to reduce noisy matches/mismatches. (2026-09: rebuilt on tree-sitter-html; compound selectors such as `li.completed` fixed.)
- Keep extending the reciprocal web-asset model deliberately rather than forcing HTML/CSS into fake call-graph semantics. The only call-style edges are HTML event handlers → JS functions.
- Improve project-level summaries for package metadata, tests, and API artifacts.
- Continue refining entrypoint selection on broad multi-project roots.

## Known Issues

- Some extracted structure is still shallow or misleading on real projects.
- QML still uses a heuristic parser; VB.NET, SQL, shell, Objective-C, Kotlin, Ruby and SCSS/LESS use purpose-built structural parsers. All other supported languages (now including plain JS/JSX, C/C++ and Go) are Tree-sitter-backed with a fallback.
- C/C++: the macro pre-pass handles export/attribute/Qt macros, but macros that expand to statements or types (`FMT_TYPE_CONSTANT(...)`, `TEST(...)`) still trip the grammar; about half of corpus C/C++ files are analysed as AST + heuristic merge.
- Recovery is AST-first for every Tree-sitter language (including Swift and CSS) via branch-scoped repair. Repairs keep the file length, so a lost brace after the last member of a type (nowhere to write it back) still falls back to the AST+heuristic merge. Repair is bounded (80 trial parses, plus 12 suspect-line and 12 insertion trials), so very large files with grammar gaps (e.g. some valid Swift) may stop repairing early.
- About 19% of valid Swift files in the corpus trip grammar gaps and go through repair. That costs time (Swift p95 is the highest of the languages) but not declarations: a repair is rejected if it would lose any.
- Callable signatures come from the syntax tree (or, for VB.NET, SQL and shell, from the declarations) everywhere except QML, which still uses snippet heuristics.
- QML is now supported as a first-class language in the explorer and CLI, but it currently uses heuristic structural extraction rather than a dedicated AST-backed parser.
- `Calls` / `Called By` are name-based, not type-resolved. Cross-file edges come from the project index with an evidence level (`confidence`: high / medium / low); a call on a receiver whose type is unknown is linked only by a distinctive name, so some real cross-file calls are deliberately left out (most visibly in C/C++, where a callee must also be reachable through the file's includes). C# `using` names namespaces, not files, so C# relies on receiver types and packages. HTTP → route links do not model Flask blueprint `url_prefix`, and relative URLs are taken from the site root.
- The new overview warnings are part of the intended safety model. They mean the app stayed responsive and returned a bounded result, but they should still be treated as prompts to inspect why that bound was hit.
- Some script or stylesheet files are now intentionally skipped as probable minified/bundled assets; that is deliberate product behavior, not a parser failure.
- Some project `mainEntry` guesses are still imperfect on broad mixed-language roots.
- HTML/CSS class comparison can still be noisy on complex HTML documents.
- Web links find consumer pages in the asset's folder or the nearest ancestor folder that links it (up to three levels). Bundler pipelines (webpack/Vite entry points), server-side templates other than PHP, and assets under `node_modules` are not modelled.
- Syntax highlighting is currently an internal lightweight implementation, not a full external highlighter.
- Snippet diagnostics are intentionally conservative for truncated previews and are not a full linter.
- The UI is materially improved and the current pane layout should be treated as the baseline, but still needs a stabilization and polish pass.

Licensing note:

- Vendored parser sources and their retained upstream license files are documented in `THIRD_PARTY_NOTICES.md`

# Implementation Log

## 2026-09-30 (continued): agent tooling additions

- **MCP `project_summary` and `web_links`:** wrapper-only. `project_summary` reads `getProjectSummary` from `-i` plus the root `package.json`; `web_links` condenses `--dump-file` of an HTML / CSS / JS file into broken references, links out, handlers, ids, DOM use and class usage.
- **Edit hooks** (`tools/agent/hooks/lumencode_hook.py`, registration example in `settings.example.json`): `pre` snapshots the file's outline silently, `post` compares and reports new damaged lines, removed or re-signatured functions with callers from the index, and added symbols. A live test showed PreToolUse context reaches the model only with the edit's result, so `pre` no longer prints the outline. Never blocks an edit.
- Not covered by automated tests yet: the MCP server and the hook script (checked by hand and in headless `claude -p` sessions).

## 2026-09-30 (continued): phase F

- **Search:** `AgentQueries::searchSymbols` ranks definition names (`Owner.name`) and file names over the project index: exact, prefix, word/camelCase initials, substring, then a 4+ character subsequence; filters by kind, language and path. Used by `lumencode-cli --search/--kind/--language/--in/--limit`, the MCP `search_symbols` tool and `ProjectController::search`.
- **GUI search:** a text field above the tree (Ctrl+F, debounced 120 ms, disabled until the index is ready) with Files / Functions / Types filters and a result list. `ProjectController::openSearchResult` reveals the file in the tree (`FileSystemModel::revealPath`) and selects the file or the symbol. The GUI targets now compile `agentqueries.cpp`.
- **Degraded-data styling:** `EntryButton` (warning colour and icon for entries that resolve nowhere), `HealthMark` (symbols on repaired lines or of lower confidence), SQL relation verbs in Calls / Called By, and `damagedLines` on symbol snippets (a red note above the source).
- **QML lessons:** a `ComboBox` next to the tree made the Overview card report an `implicitHeight` binding loop (replaced by toggle buttons), and `Layout.*` attached properties on an `AbstractCard`'s `contentItem` upset the Kirigami card; the results list is a plain `ListView` in a `Rectangle`.
- **Smoke test:** finds items in the visual tree (pages pushed on the stack are not `QObject` children), drives the search field, opens results, and with `LUMENCODE_SMOKE_SHOTS=<dir>` saves screenshots via `grabToImage` (`grabWindow` returns an empty image offscreen). Those real frames revealed `implicitHeight` binding loops in the Overview / Detail cards on first display of some files, reproducible with the original UI.

## 2026-09-30 (continued): phase G

- **`--outline <file>`** (`src/agentqueries.{h,cpp}`):
    - Content: names, kinds, line ranges, signatures (with defaults and return types), details, Calls / Called By names (`name @ path:line` across files), imports, routes with their clients, HTTP calls with their routes, and damaged lines. No snippets.
    - Formats: JSON, or `--format text`, an indented form for agents.
    - Size: `src/projectindex.cpp` (1,300 lines) is 13 KB of outline JSON against 111 KB for `--dump-file`.
    - With `--index-project <root>` the outline includes cross-file relations.
- **Project queries** on the index:
    - `--find <name|Owner.name>` lists definitions.
    - `--callers` / `--callees <name>` give the definition file's full relation lists (same-file and cross-file, with confidence and `calledByTotal`).
    - `--routes` lists every route with mount prefixes and the client calls that reach it.
- **Found on the way** (by querying LumenCode's own source):
    - `ProjectIndex::augmentAnalysis(...)` had no callers. Scoped calls (`ns::f`, `Type::new`) recorded only the scope, like an object receiver, so they were treated as method calls on an unknown receiver. Scoped calls are now marked and reach free functions. For C/C++, `#include`s count transitively and the scope must match the callee's namespace; `std::string(...)` had linked to gtest's `string`. All four callers now resolve with high confidence, and fmt went from 1,575 to 796 edges, all sampled correct.
    - C++ prototypes inside a namespace lost their `declaration` marker, so they were never paired with their bodies.
- **`tools/agent/SKILL.md`:** when to use which command, and how to read modes, damaged lines and confidence levels.
- **`tools/agent/lumencode_mcp.py`:** an MCP stdio server with no third-party packages, exposing `outline_file`, `find_definition`, `find_callers`, `find_callees`, `list_routes` and `index_stats`. Output is capped at 60k characters.
- **`-DLUMENCODE_CLI_ONLY=ON`:** builds only `lumencode-cli` against Qt 5 Core/Concurrent/Widgets, without ECM, KF5, QML or Kirigami, into `build*/bin`. The CLI no longer links `KF5::CoreAddons`, which it never used.

## 2026-09-30 (continued): phase E

- **Kotlin** (structural, `parseKotlin`):
    - Parsing: noise blanking handles nested block comments, raw strings and `${}` templates with nested quotes. Declarations are found at depth 0 of each body.
    - Coverage: classes, interfaces, objects (incl. companion), enums with entries, functions (type parameters, extension receivers, expression bodies), properties (incl. constructor `val`/`var`), secondary constructors and typealiases, with signatures from the declarations.
    - Links: call sites for the index (companion members are owned by the enclosing class, so `Cart.empty()` resolves by qualifier); imports through the package's source root; Spring and Ktor routes.
    - Resilience: a member's body or statement cannot run past the next sibling declaration.
    - Corpus: 327 files, 97.9% with symbols; damage probe 96.5% (lost brace 55 → 98% once members were bounded). okio: 1,084 cross-file edges after Kotlin standard-library names joined the generic list.
- **Ruby** (structural, `parseRuby`):
    - Parsing: an `end` stack over noise-blanked lines. Quoted strings end at the line end, so an unterminated quote no longer swallows the file (unclosed-string retention 65 → 100%).
    - Coverage: modules, classes, methods (incl. `self.`, `class << self`, endless and one-line), parameters with defaults, `attr_*`, constants, and RSpec/Minitest blocks as test suites, tests and hooks (files with symbols 71 → 90%).
    - Resilience: a missing `end` is settled by indentation; spec blocks count as declarations there (lost-end retention 58 → 95%). The damage probe now deletes `end` lines for Ruby.
    - Links: `require`/`require_relative` resolve; Sinatra routes.
    - Index: Ruby core method names join the generic list, and a nested type is constructed bare only inside its owner (sinatra `::Logger.new` had matched a nested `Logger`).
- **SCSS / LESS** (`parseStylesheetDialect`):
    - Coverage: nested rules with resolved selectors (`&`, BEM suffixes, parent lists), mixins/functions/placeholders/keyframes/variables, and `@media`-style blocks transparent to their rules.
    - Links: `@include`/`@extend` and LESS mixin calls (`;`-separated arguments) are relations and index call sites, so SCSS mixins in partials link by import and Bootstrap's LESS mixins link across files. `@import`/`@use`/`@forward` resolve `_partials`.
- **Config files:**
    - JSON objects outlined two levels deep, with JSONC accepted.
    - `composer.json`: dependencies, PSR-4 roots and scripts.
    - `tsconfig`/`jsconfig`: `extends` and project references resolved.
    - `appsettings*.json`: sections, with secret-looking values hidden.
    - PHP configuration arrays and CodeIgniter `$config[...]` assignments as settings.
    - Corpus files with symbols: JSON 17.9 → 83.8%, PHP 73.2 → 91.2%.
- **Fixtures:** 88 → 96 (Kotlin model + Ktor routes + companion qualifier, Ruby structure + Sinatra, SCSS nesting + cross-file mixin, PHP config array). GUI smoke green with every new kind.

## 2026-09-30 (continued): phases C and D

- **Phase C — cross-language links on the index:**
    - Python imports resolved to local modules and packages from comment/string-blanked logical lines (docstring examples no longer count). Relative imports resolve by level; absolute imports resolve from above the enclosing package (never the package's own siblings, as in Python 3) and in `src/` layouts. `from pkg import module` becomes a submodule dependency, and the index follows package re-exports (`__init__.py`, JS barrels) up to three hops. flask 452 → 635 cross-file edges, requests 443 → 720.
    - C/C++ prototypes are paired with their bodies, and calls that reach a prototype land on the body. Prototypes show `definition`, bodies `declaredIn`.
    - For C/C++ precision, a callee without import evidence must be reachable through the file's transitive `#include`s, and bare calls only reach the caller's own class. Qt/STL names dominated the misses.
    - Found by `index_metrics`: calls retargeted from a prototype into the calling file were reported as cross-file edges (kirigami ~145 of 234).
    - PHP `use` imports carry bindings, including aliases; Slim has 439 import-backed edges. Java imports resolve to files through the package's source root, including sibling source sets and nested classes; gson went from 0 to 1,018 import-backed edges.
    - HTTP client calls → routes (new `src/httpclients.{h,cpp}`):
        - Clients: fetch, axios, jQuery, XHR and Angular `http.get<T>` in JS/TS, and HTML/PHP forms, with dynamic URL parts as wildcards.
        - Matching is segment-wise. It prefers literal agreement, then a route in the calling file, then non-test files, and applies Express mount prefixes (`app.use('/api', router)`). External hosts are skipped.
        - The calling function gets the route and the decorated view function in Calls. Routes get `calledFrom`, and view functions get Called By.
        - Route extraction no longer treats axios/`$` client calls as routes, and the Express regex fallback ignores comments. hono's JSDoc examples had given 40+ library files phantom routes.
    - Corpus index (27 projects, 5,101 files incl. HTML):
        - 162,408 call sites, 17,933 cross-file edges, 0 failures.
        - Cold 57 s in total, warm 3.3 s.
        - Reciprocity 819/819 sampled edges in both the caller's Calls and the callee's Called By.
- **Phase D — resilience round two:**
    - **Suspect-line repair.** Tree-sitter often blames an intact enclosing line (`describe('x', () => {`) for an unclosed call or string below it. Each repair round first tries up to six textually suspect lines near the error:
        - a line that opens more brackets than it closes without the next line indenting deeper, or
        - a line with an unterminated quote.

      A suspect counts only if the whole file then parses cleanly (12 trials per file). A first version that accepted partial gains doubled Swift p95 and made one Alamofire file's repair worse; the clean-only rule fixed both.
    - **Lost-brace repair.** Indentation drops past a still-open block are candidates. The repair writes the block's closers (reverse of its unmatched openers, plus `;` where a separator is needed) over the next line's indentation. If they don't fit, it blanks the line that opened the block (in Python, its whole indented block), so the body joins the enclosing block. A candidate is accepted only if the file then parses cleanly.
    - **Re-nesting clean parses.** Swift nests everything after a lost brace without any error. A clean parse is checked for declarations nested in another but not indented deeper, and closers are written back if the parse stays clean and the misnesting drops. No clean corpus file changed structure.
    - **C/C++ macro pre-pass.** The source is rewritten at the same length before parsing:
        - Qt macros, `signals:`/`slots` and `emit` are handled.
        - Export and attribute macros are blanked, and wrapping export macros are unwrapped.
        - The rewrite is kept only if it parses at least as well as the original. fmt's `FMT_DEPRECATED operator const string_view&()` cascades into a whole-file error without the macro.
        - `--cpp-prepass` prints the rewritten source, and `--debug-ast` shows its parse.
        - Clean-AST C/C++ files 126 → 206 of 437.
        - The recovered merge had listed every include twice; it's now de-duplicated.
    - **`damage_probe.py`** gains a `missing_close` mutation (the last brace-only line in the target deleted). Retention, same sample:

        | Language | Unclosed call / string (before → after) | Lost brace (before → after) |
        |---|---|---|
        | TS | 95.7 → 98.9% overall | 73 → 98% |
        | TSX | 91.8 → 92.3% | 63 → 82% |
        | JS | — | 69 → 92% |
        | Java | 94.7 → 96.0% | 80 → 94% |
        | C# | 93.1 → 97.1% on the original mutations | 32 → 76% |
        | Rust | — | 80 → 92% |
        | Go | — | 72 → 82% |
        | Swift | unclosed strings 65 → 93% | 44 → 65% |
        | C++ | — | 85 → 89% |

      What still fails is mostly a lost brace after a type's last member (nowhere to write it back at the same length), and TSX unclosed calls (88%).
    - **Grammar:** tree-sitter-swift 0.7.1 is already the latest release.
- **Fixtures:** 72 → 88. New cases cover:
    - a Python package with re-exports
    - Java source sets and composer PHP with an alias
    - Express with a mounted router and fetch/axios/jQuery/form clients, and a Flask API
    - C/C++ prototypes, a C++ macro header
    - TS test-block recovery (unclosed call, lost closer)
    - Swift re-nesting

  The TS and C# recovery fixtures now expect full AST recovery. `regression_sweep` round-trips route relations through `calledFrom` and gained `max_routes`.
- **ASan:** clean over all 374 corpus C/C++ files, 1,036 web/Python/Java files, and in-process index builds of hono, express, flask and the fixtures.

## 2026-09-30

Roadmap phases A and B ([`roadmap.md`](roadmap.md)), CLI-first in the cloud
container, gated by fixtures, `corpus_scan.py --compare`, the damage probe,
ASan and the new headless GUI smoke test.

- **Phase A:**
    - `lumencode-gui-smoke` (not built by default): loads the real `Main.qml` offscreen, opens every file below the given roots and selects every symbol, member, relation, dependency, route and quick link it offers (following cross-file links and coming back). It fails on any QML warning, and a deliberately broken binding checks the detector itself. Green on all fixtures and a 141-file corpus sample.
    - TS/JS: `describe`/`it`/`test` blocks and hooks, re-exports (`export * from`, `module.exports = require(...)`), top-level event listeners as handlers, `export default {}` and AMD return values. Files with symbols: TS 63% → 93%, JS 65% → 87%, TSX 59% → 94%.
    - Rust and Swift signatures from the syntax tree.
    - Objective-C rewritten on comment/string-blanked text: full multi-part selectors, categories, message sends matched by full selector. Files with relations 0% → 67%.
- **Phase B — project index** (`src/projectindex.{h,cpp}`):
    - `applyAstCallSites` records raw call sites (name, receiver, line) per callable, plus `moduleCallSites` for file-level code, for TS/JS, Python, Java, C#, PHP, Rust, Swift, Go and C/C++. `applyTextCallSites` does the same for Objective-C, VB.NET and shell on noise-blanked text.
    - `lumencode-cli --index-facts` turns paths on stdin into one compact JSON line of facts per file. `ProjectIndex::build` runs it in parallel batches of 150 (a crash is retried file by file, so it costs only that file). The cache sits under `~/.cache/lumencode/index/`, keyed by file size + mtime and by the helper binary, so a rebuilt parser re-indexes.
    - Resolution order: import binding (high) → receiver names the owner (medium) → same directory/package, for bare calls and `new` only (medium) → the only definition of a distinctive, non-generic name (low). Anything ambiguous is left unresolved. Edges are stored both ways.
    - Precision work, from reading sampled edges against the source (≈93% correct at the first check, most of the rest fixed after):
        - calls on receivers of unknown type only reach methods, and only by distinctive names
        - names bound to external packages are never linked
        - nested types are not linked from outside their owner
        - `new X()` prefers the type over its constructors
        - stoplists of standard-library / `NSObject` method names
        - `self` calls to another type's method must pass the distinctive-name test
    - The controller builds the index in the background on `setRootPath` and passes the snapshot into each analysis. When the index lands, the open file is re-augmented in place. The CLI builds it synchronously (deterministic), and `--no-index` / `LUMENCODE_NO_INDEX=1` turn it off. The old per-click JS/TS crawl still runs first, and index edges are merged in without duplicates. More than 400 cross-file callers: the first 400 are listed and `calledByTotal` is reported.
    - New CLI: `--index-project <root>` (stats), `--index-edges` (every edge, JSON lines), `--index-relations <file>` (one file's cross-file Calls / Called By).
    - **Bug found on the way:** C# types inside a block-scoped `namespace X { }` were dropped entirely, because the walker did not descend into the namespace's `declaration_list`. Corpus C# files with symbols 91.7% → 97.5%.
    - **Corpus (`tools/index_metrics.py`, 27 projects, 4,557 files):**
        - 161,888 call sites and 17,399 cross-file edges; 0 helper failures.
        - Cold build 48.6 s in total (largest: fmt 11.8 s, C++ repair), warm 2.1 s.
        - Reciprocity through the live per-file view: 819/819 sampled edges listed in the caller's Calls, 817/819 in the callee's Called By (the 2 are truncated lists of a symbol with more than 400 callers).
        - Files with a cross-file edge: Java 82%, Obj-C 87%, TSX 88%, Go 76%, Python 73%, Swift 72%, C# 70%, TS 66%, Rust 63%, PHP 57%, C/C++ 46%, VB.NET 29%, JS 25% (mostly library calls), shell 17%.
    - **Fixtures:** 51 → 72. `cross_file/` has mini-projects in Go, Python, Java, C#, PHP, Rust, C++, Swift, VB.NET, shell and Objective-C, each asserting `Calls` in the caller and `Called By` in the callee. All 20 new index cases fail with the index off. The GUI smoke test now waits for the index, so cross-file navigation is driven too.
    - **ASan:** `--index-facts` over 2,267 corpus files (all Obj-C, VB.NET, shell and C#, a quarter of the rest) and in-process index builds of hono, SDWebImage and dotnet-samples were clean.
    - **Left for Phase C:** Python imports resolved to modules (they resolve by sibling or unique name today), C/C++ calls landing on the header declaration rather than the definition, PHP `use`-resolved receivers, and HTTP-client calls → routes.

## 2026-09-29

Worked entirely through the CLI in a cloud container (Ubuntu 24.04, Qt 5.15 /
KF5 from apt), against a new pinned public corpus instead of `/home/user/Code`.

- **Corpus and measurement tooling:**
    - `tools/corpus.json` + `tools/fetch_corpus.py`: 27 public repos pinned by commit (≈6.4k candidate files, 17 languages incl. VB.NET via a sparse dotnet/samples checkout).
    - `tools/corpus_scan.py`: parallel `--dump-file` over the whole corpus with per-language coverage/timing tables, the regression-sweep contract validator, and `--save` / `--compare` diffs that flag per-file symbol/relation drops.
    - `tools/damage_probe.py`: injects a garbage line / unclosed call / unclosed string into one function of a clean file and measures how many *other* declarations (or, for single-type files, members) survive.
    - `regression_sweep.py --corpus-root` / `$LUMENCODE_CORPUS`; `lumencode-cli --debug-ast` prints Tree-sitter ERROR/MISSING nodes and the repair outcome.
- **Crash fix:** the first corpus scan found a segfault in 3 TS files: `ts_node_type()` on a null field node (`let x: T` has no value). All node-type lookups now go through a null-safe `tsType()`. This was most likely the "stability problem on real-world JS" that had kept plain JS on the heuristic path.
- **Branch-scoped AST repair (all Tree-sitter languages):**
    - On syntax errors, the lines the parser flags are blanked byte-for-byte and the file re-parsed, greedily, while the error shrinks. Candidates are ranked by how much declaration structure they preserve, so an intact `class Foo {` header is not sacrificed. Python blanks the damaged line's indented block.
    - AST parse functions read the repaired bytes via a thread-local override; snippets still come from the original text.
    - Heuristic symbols are admitted only on blanked lines. A repair is rejected if it loses declarations the unrepaired tree had, or keeps under 60% of what the heuristic parser sees; the previous AST+heuristic merge is then used.
    - The budget is deterministic (80 trial parses, 3s safety cap). `parseFile` now has one `analyseWithAst` path, which brought Swift and CSS into recovery.
    - Damage-probe retention: 91.0% → 97.0% overall (C# garbage line 80.5% → ~97%, Java unclosed string 57% → 89%, PHP unclosed string 96% → 100%).
- **JS/JSX on Tree-sitter:**
    - The AST walker now covers wrapper bodies (IIFE/UMD, AMD `define`/`require`, jQuery ready, DOMContentLoaded) and member assignments (`X.prototype.m`, `X.prototype = {}`, `X.m = fn`, `window.f`).
    - It also covers closure-module and constructor-function members, and `X.extend({...})` / `createClass` / `defineComponent` classes.
    - Routes are found anywhere in the tree. `require()` bindings are no longer symbols.
    - Corpus JS vs the old regex path: members 1239 → 1668, routes 303 → 390, retention under damage 98.5%.
- **Python signatures from the syntax tree (issue #1):**
    - Parameters come from the tree, with types, defaults, `*args` and `**kw`.
    - Returns use the `-> T` annotation when present. Otherwise every return path is classified into a type and grouped with its lines, plus implicit `None` on fall-through and `Generator` for yield, e.g. `str | None (3 return paths)`.
    - `mergeSymbolData` treated `returns` as a string (fixed).
- **Web links (new `src/weblinks.{h,cpp}`, vendored tree-sitter-html v0.23.2):**
    - A cached HTML page model built from the syntax tree: ids, classes, handlers, inline script/style blocks, custom elements and local assets. Consumer pages are found in the asset's folder or the nearest ancestor folder that links it.
    - JS DOM references resolve to HTML elements and CSS rules, and are flagged when missing. Handlers give `Called By` edges into JS; custom elements link to the pages that use them.
    - HTML files gained symbols for ids, handlers, custom elements, forms and inline script/style contents, parsed by the real parsers on line-aligned text.
    - CSS gained `scriptAppliedClasses`, `unusedClasses` and `@import` / `url()` dependencies.
    - Fixed: compound class selectors (`li.completed`) were indexed under the whole selector text in four places.
    - Corpus HTML files with symbols: 0% → 77.6%.
- **PHP links:** `use` imports resolved through composer PSR-4 roots; `require`/`include` resolved relative to the file and its ancestors; Slim/Lumen, Laravel and CodeIgniter routes; template asset links (PHP regions blanked before HTML parsing). PHP files with dependencies: 0% → 32.8%.
- **Later the same session: languages, parity and AST signatures:**
    - **VB.NET** (new): a structural, block-aware line parser. It gives exact signatures (`ByVal`/`Optional`/`As T`), members, events, `Handles` clauses, `Imports`, ASP.NET attribute routes and call edges. A missing `End` is closed at the next declaration and reported. Corpus: 470 files; damage probe 100%.
    - **SQL** (new, MySQL/MariaDB and T-SQL): statement-aware. It covers tables/columns/keys, views, procedures, functions, triggers and indexes. Relations are labelled `references`, `reads`, `writes`, `executes`, `calls` and `fires on`. `USE`, `source` and `:r` are dependencies. Objects end at the next `CREATE`/`ALTER`, `GO` or `DELIMITER`, and strings cannot cross batches.
    - **Go** (new, tree-sitter-go v0.23.4): structs, interfaces, receiver methods grouped under their type, exact signatures, imports, and net/http, gin, chi and echo routes. Damage probe 99.4%.
    - **C/C++ on Tree-sitter** (tree-sitter-cpp v0.23.4, used for C too): classes with access levels, out-of-line definitions, enums, typedefs, `#include` resolution, call edges and declarator-derived signatures.
        - Members went from 0 to 5.2k and call edges from 0 to 3.4k on 437 corpus files.
        - Macro-noisy files (more than 25 initial errors) skip the repair pass. The repair budget scales with file size, and the declaration counter uses a per-grammar symbol table (p95 569 → 184 ms).
    - **Shell** (new): functions, named or positional parameters, call edges and `source` resolution. A recursive quoting scanner handles nested `"$(… "…")"` (nvm.sh: 13 → 125 functions).
    - **AST signatures for TS/JS, C#, Java and PHP** via one post-pass:
        - Parameters come with types, defaults and modifiers. Returns use the declared type, or inferred return paths for JS and untyped PHP.
        - Callables with parameters: TS 27% → 85%, C# 23% → 40%.
        - A null-body crash on PHP interface methods was caught by the corpus comparison before commit, and an ASan run over 1,109 files of all 20 types was clean.
    - **Parity:**
        - Swift imports (dependencies 0.9% → 98%).
        - Java Spring and JAX-RS routes.
        - QML call relations between functions and signal handlers (0% → 22% of files).
    - **Damage probe**, all 13 analysed language groups: 97.2% over 1,041 probes. The weakest cases are unclosed strings in Swift (59%), Java (87%) and Rust (91%).
    - **Not done, on purpose:** Kotlin and Ruby. Their Tree-sitter grammars are ≈23 MB and 15 MB of generated source; a structural parser is likely the better trade-off. JS `fetch()` → backend-route links and Objective-C relations remain open too.
- **Fixtures:** 27 → 45 cases (`python_returns`, `web_app` ×3, `php_links` ×2, `java_routes` ×2, `vbnet_basic` ×3, `sql_tsql` ×2, `go_basic`, `cpp_classes` ×3, `shell_basic`). Recovery fixtures now expect `high` confidence for AST-derived symbols in repaired files.
- **Known remaining gaps** (next steps are in the README roadmap, Phase 2b):
    - C/C++, QML and Objective-C are heuristic only, and have no call relations.
    - Swift imports and Java routes are not extracted.
    - Go, Kotlin, Ruby, Bash, VB.NET and SQL are not yet supported.
    - Signatures outside Python are still snippet-derived.

## 2026-06-26

- **Regression sweep hardening: provenance and signature contracts:**
    - Extended `tools/regression_sweep.py` with contract validators for `sourceMode`, `confidence`, `parameters`, and `returns` on every symbol so violations surface immediately.
    - Added duplicate symbol identity detection (restricted to top-level symbols to avoid false positives from member methods with the same relative line).
    - Added stale relation target detection and source-context collection validation for dependencies, routes, quickLinks, and relatedFiles.
    - Tightened the potential relation heuristic in the sweep to require call syntax (`name(`) rather than bare word presence.

- **Callable signature contract fixes:**
    - `enrichCallableSignature` previously returned early without setting `parameters`/`returns` when the snippet was empty or started with `:` (constructor initialiser). Added default fields (`parameters: []`, `returns: [{text:"none"}]`) in both early-return paths so the contract is always satisfied.
    - Fixed JS class methods and PHP heuristic methods that had no snippets: added `snippetFromBraceBlock` to JS `parseClassMembers` and ensured `parseObjectMembers` similarly includes snippets.

- **CSS deduplication fix:**
    - Comma-grouped selectors (e.g. `.a:hover, .a:focus-visible {}`) previously produced duplicate CSS symbols because both selectors resolved to the same `rule_set` line. Added `seenClassLines` QSet deduplication in `parseCssTreeSitter`.

- **Snippet anchoring fixes (line-start regex patterns):**
    - Replaced `^\s*` with `^[ \t]*` in ObjC function, interface/type alias, Python Flask/FastAPI decorator, and other multiline patterns. PCRE `\s` matches `\n`, causing matches at blank lines whose `lineNumberAtOffset` then produced wrong snippet lines.
    - Reduced `contextLines` to 0 on ObjC methods (was 1) and ObjC functions (was 2) to stop snippets from inheriting closing braces of the preceding method.
    - Reduced `contextLines` to 0 on Python route decorators and TS interface/type patterns.

- **HTML parse timeout fix (CSS class index):**
    - The HTML parser was calling `findCssClassSummaryEntry` (which runs a full Tree-sitter CSS parse) once per class name found in each linked CSS file. For `bootstrap.min.css` (~228K, ~500+ classes) this caused 500 sequential Tree-sitter parses and a 20-second timeout.
    - Replaced the per-class approach with a new `buildCssClassIndex` function that does one Tree-sitter parse per CSS file and returns a complete `QMap<QString, QVariantMap>` of all name→entry mappings.
    - Added a minified-file fast path: if the CSS file looks minified (`.min.` in filename or very long lines), `buildCssClassIndex` skips Tree-sitter entirely and uses the regex fallback with stub entries, avoiding even the single Tree-sitter parse on huge vendor bundles.
    - Both the linked-CSS loop and the sibling-CSS loop now use `buildCssClassIndex`.
    - Result: `index.html` linking `bootstrap.min.css` dropped from 20s timeout to 27ms. Full 1200-file sweep passes with `issues_found: 0`.

- **JS heuristic output quality improvements:**
    - Anchored all top-level declaration patterns (`variable`, `arrow`, `objectExport`, `functionExpression`) to `^` so they only match at column 0 with `MultilineOption`. Local variables and nested arrow functions inside function bodies are no longer extracted as top-level symbols; typical async-function files dropped from 30+ symbols to 6–9 meaningful ones.
    - Added `"function"` to the return-type `dropTokens` list in `enrichCallableSignature`. The C-style prefix extractor was stripping `async` but leaving `function` behind, causing `async function foo()` to report `returns: ['function']`; these now correctly report `returns: ['none']`.
    - Suppressed bare open-delimiter return captures (`{`, `(`, `[`) from multi-line object/array literals so they no longer appear as meaningless return values. Single-character literal returns (e.g. `return 1`) are preserved.
    - All 27 fixture cases and the 784-file corpus sweep continue to pass with `issues_found: 0`.

## 2026-06-04
- **Fixture hardening: CSS `:has()` coverage:**
    - Added a dedicated `html_css_has` baseline to cover grouped selectors with `:has()` and confirm the extractor matches nested class selectors instead of string-literal noise.
    - Locked in the expected CSS summary counts so `:has()` handling stays regression-tested alongside the existing pseudo-class coverage.
    - Revalidated the fixture-only harness and confirmed the new fixture output matches the intended class/missing-class split.
- **Fixture hardening: CSS pseudo-class coverage:**
    - Added a dedicated `html_css_pseudos` baseline to cover grouped selectors and pseudo-class wrappers such as `:is()`, `:where()`, and `:not()`.
    - Locked in the expected CSS summary counts for both matched and missing classes so the pseudo-class path stays regression-tested.
    - Revalidated the fixture-only harness and the 160-file corpus sweep with `issues_found: 0`.

- **Fixture hardening: html_css baseline:**
    - Expanded the baseline HTML/CSS fixture to include comment and string-literal selector noise, plus an explicit missing class that exercises the CSS summary path.
    - Updated the manifest expectations so the fixture now locks in the current `matchedClasses` and `missingClasses` counts for the CSS summary.
    - Revalidated the fixture-only harness and the 160-file corpus sweep with `issues_found: 0`.

- **Stabilization pass: CSS selector noise reduction:**
    - Replaced CSS summary class discovery with Tree-sitter-backed selector traversal, with the regex path now acting only as a fallback.
    - Kept selector lookup anchored to the cleaned CSS text so matched class snippets and line numbers stay aligned with the real selector instead of comment noise.
    - Revalidated a focused CSS fixture and the 160-file corpus sweep with `issues_found: 0`.

- **Stabilization pass: HTML/CSS and Node/CommonJS noise reduction:**
    - Stripped HTML comments out of class extraction and linked-asset discovery so comment content no longer creates fake class matches or asset links.
    - Normalized HTML asset targets to drop cache-busting query and fragment suffixes before resolution, matching the real file path instead of the decorated URL.
    - Extended local Node/CommonJS dependency resolution to recognize common `.mjs`, `.cjs`, and `.jsx` forms, including index-file fallbacks, so local imports resolve less noisily.
    - Revalidated the HTML-focused temporary fixture, the Node/CommonJS temporary fixture, the build, and the 160-file corpus sweep with `issues_found: 0`.

- **Stabilization pass: shared inspection contract:**
    - Normalized the remaining Python, Java, C#, and Rust dependency payload builders onto `makeSourceContextItem` so they inherit the same snippet-kind and diagnostics defaults as the rest of the inspection output.
    - Removed the last unused hand-rolled dependency closure from the Python path after the normalization.
    - Revalidated the build, the earlier JS/TS crash reproducer, and the 160-file corpus sweep with `issues_found: 0`.

- **Generated JS corpus coverage:**
    - Added `highlight.js` and `prism` to the shared corpus to stress generated and bundled JavaScript assets.
    - Revalidated the 160-file sweep with the expanded corpus and `issues_found: 0`.

- **Corpus expansion continued:**
    - Added more HTML/CSS and C# corpus repos under `/home/user/Code/Corpus`, including `startbootstrap-freelancer`, `startbootstrap-agency`, and `dotnet-samples`.
    - Revalidated the expanded corpus with a 160-file sweep and `issues_found: 0`.


- **Corpus expansion and stability hardening:**
    - Created `/home/user/Code/Corpus` and seeded it with representative real-world repos for broader regression fodder.
    - Fixed a JS/TS Tree-sitter crash in `jsCallableKeyForNode` by guarding missing child nodes before dereferencing them.
    - Normalized multiline context snippets to `block_excerpt` in `makeSourceContextItem`, so dependency payloads no longer violate the line-excerpt contract.
    - Revalidated the exact crash reproducer and a broader 80-file corpus sweep with `issues_found: 0`.

## 2026-04-04

- **Parser Authority / Recovery Refactor (Phase 1):**
    - Added parser-owned provenance fields at both file and symbol/item level, including `analysisSourceMode`, `analysisConfidence`, `analysisHasAstErrors`, `analysisPartial`, `analysisNotices`, plus per-item `sourceMode` and `confidence`.
    - Changed provenance annotation so mixed analyses preserve item-level authority instead of flattening everything into one fake source mode.
    - Implemented the first deliberate partial-AST recovery path for TS/TSX:
      - keep partial Tree-sitter output even when the tree contains errors
      - supplement it with heuristic recovery instead of replacing the whole file analysis
      - return `analysisSourceMode: recovered` with a warning notice and partial-analysis flag
    - Added a checked-in broken TypeScript fixture to lock down that recovered-analysis contract through the CLI regression harness.
    - Extended `tools/regression_sweep.py` so fixtures can assert `analysisSourceMode`, `analysisPartial`, and analysis notices, not just symbol presence.
    - Extended the same recovered-analysis model to Python and Java, including checked-in broken-code fixtures for both languages.
    - Extended the same recovered-analysis model to C#, including a checked-in broken-code fixture for the current recovery contract.
    - Extended the same recovered-analysis model to Rust and PHP, including checked-in broken-code fixtures for both languages.
    - Added post-merge normalization for recovered analyses so merged symbol trees and relation targets are rewritten against the surviving canonical symbol set, reducing obvious AST/heuristic duplicates and stale reverse-edge targets.
    - Finished the current recovery phase by replacing file-wide heuristic supplementation with AST-uncovered-range gating for the recovered languages. Heuristic structure is now merged only where the AST did not already claim useful symbol ranges, and same-file relation selection waits for async hydration before resolving relation targets.
    - Left the broader authority refactor intentionally incomplete in narrower ways: Swift and CSS still remain outside the deliberate recovered-analysis model, and future work can refine the current uncovered-range model further without reviving the unstable parser-wide error-node walk.

- **Callable Signature Contract Refactor:**
    - Added `parameters` and `returns` to callable symbol payloads as backend-owned fields instead of controller-only UI enrichment.
    - Moved callable signature extraction into `SymbolParser`, so CLI dumps, async GUI analysis, direct symbol selection, and relation rehydration all see the same signature data.
    - Preserved declared parameter types and return types for typed languages where the signature can be derived, while keeping fallback inferred `return ...` expression summaries for untyped cases.
    - Fixed typed C-style signatures so return types are no longer replaced by sampled return expressions, and parameter types now preserve pointer/reference markers.
    - Guarded obvious malformed callable snippets such as constructor initializer-list fragments so they no longer emit nonsense return values.
    - Left the current parser-layer signature logic explicitly transitional: the architecture is now correct, but several languages still derive signatures from snippets/signature heads inside the parser rather than directly from grammar nodes.

- **QML Support:**
    - Added first-class `.qml` file discovery in the filesystem crawler and regression harness.
    - Added pragmatic QML language detection and heuristic parsing for imports, root/inline components, properties, signals, functions, and common `on...` handlers.
    - Wired QML through the existing lower-pane snippet language and keyword-highlighting path so it behaves like a supported language in the current UI.
    - Added a checked-in `qml_basic` fixture and validated it through `lumencode-cli` and the fixture sweep.

- **Cross-File Relation Parity / Web Work:**
    - Improved JS/TS/CommonJS cross-file relation augmentation so local alias bindings are respected instead of relying only on raw name matching.
    - Added binding metadata for named imports, aliased imports, and destructured `require(...)` imports in script dependency payloads.
    - Extended the project-side relationship augmentation to use those bindings when deriving cross-file `Calls` / `Called By` links.
    - Added checked-in alias fixtures for TS module imports and CommonJS destructuring aliases.
    - Added reciprocal web-asset inspector behavior:
      - CSS files now show inbound HTML consumer links.
      - CSS files now show HTML-side matched/missing class usage from linked nearby HTML files.
      - Local script files now show inbound HTML consumer links when linked via `<script src=...>`.
    - Updated the right-pane CSS class navigation so entries can point to HTML or CSS appropriately instead of assuming every class entry is CSS-backed.
    - Added checked-in `html_script` fixture coverage and extended `html_css` fixture expectations so this web reciprocity model is regression-tested.

- **Robustness / Overload Hardening:**
    - Added controller-side budgets for project relationship augmentation, including a time budget and limits on imported and incoming file analyses.
    - Added explicit `analysisNotices` and partial-analysis summaries so bounded work now surfaces as visible warnings instead of silent sparse output.
    - Normalized helper failure cases such as missing helper, helper timeout, helper failure, invalid helper output, and oversized-file refusal into the same surfaced notice model.
    - Moved GUI file and cross-file relation analysis onto an asynchronous background path using `QtConcurrent`, with stale-result protection so rapid navigation does not apply obsolete completions.
    - Added overview-pane loading affordances so users see when analysis is in progress instead of assuming empty or half-filled panes are bugs.
    - Confirmed the fixture baseline still passes after the async hardening and bounded-analysis changes.
    - Added a low-value relationship gate in `ProjectController` so variable-only script files no longer trigger the expensive incoming relationship scan just because they are script-like.
    - This specifically reduces misleading timeout/partial warnings on setup-style files that are unlikely to expose meaningful imported symbol relationships.
    - Added parser-side minified/bundled asset detection for script-like files and CSS, so obvious `.min.*` or newline-starved vendor assets are skipped with explicit warning summaries instead of being explored like first-party source.

- **Relation Navigation / Backend Work:**
    - Fixed relation click handling in `ProjectController` so selecting `Calls` / `Called By` entries rehydrates into the actual destination symbol instead of leaving the inspector on a thin edge payload.
    - This restored reciprocal relation visibility in the common case where the destination symbol already has its own relation data, especially when navigating through Swift files.
    - Tightened relationship target selection so callable declarations are preferred over weaker export/property shadows when multiple symbols share a name.
- **More Relation Parity:**
    - Added same-file relation coverage for Python, Rust, Java, and C# to the checked-in fixture suite instead of only asserting symbol presence for those languages.
    - Added a proper AST walk for Python call relations after real docstring-heavy files proved that bounded snippet-based relation detection was too weak.
    - Changed the snippet-based relation fallback to merge with existing AST-derived edges instead of overwriting them, which fixed missing reverse links during relation round-trips on real class methods.
- **JavaScript Stability Follow-up:**
    - Investigated restoring plain JS/JSX to the native Tree-sitter path.
    - Confirmed there are still stability problems on real-world JS files in that path, so plain JS/JSX were left on the heuristic parser for now.
    - Added same-file call-relation extraction to the heuristic JS/JSX parser so it participates in the same `Calls` / `Called By` contract without regressing crash behavior.
- **Fixture / Regression Work:**
    - Added a checked-in baseline fixture suite under `tests/fixtures/baseline/` with a manifest-driven set of compact structural test projects spanning JS, TS, PHP, Swift, Python, Rust, Java, C#, C++, Objective-C, HTML/CSS, and `package.json`.
    - Extended `tools/regression_sweep.py` with `--fixture-manifest` and `--fixtures-only`.
    - Added relation round-trip checks to the regression harness by driving `selectSymbolByData` through the interactive CLI and verifying reverse edges on the selected destination symbol.
    - Fixed the fixture harness so it uses `--dump-file` as the authoritative file-analysis source and reserves interactive CLI state for controller-backed behaviors such as cross-file augmentation and relation round-trips.
    - Revalidated the current backend with both fixture-only runs and wider local-corpus sweeps under `/home/user/Code`, ending this session with `issues_found: 0` on both passes.
- **Range-Aware Recovery Follow-up:**
    - Started a first parser-wide attempt at AST error-range harvesting as groundwork for range-aware recovery.
    - Backed that attempt out in the same session after it proved unstable across several grammars.
    - Kept the repository on the known-good file-level recovered-analysis behavior and recorded range-aware recovery as the next step, but only via narrower language-specific work and fixture-gated rollout.
- **Known Remaining Gap:**
    - Relation traversal is substantially better, but overall relation completeness still needs work across languages and cross-file shapes.
    - Swift functions still often leave the right inspector feeling sparse; useful additional symbol detail should be added later once backend payloads are more trustworthy.
    - The next iteration should continue the authority/recovery refactor into the remaining Tree-sitter languages beyond C#, while continuing to tighten recovered merge quality where broken-code fixtures expose residual duplication.
    - Surfaced warnings are expected now under bounded degradation, but each one should still be treated as a lead for future optimization or parser/integration investigation rather than dismissed as inevitable.

## 2026-04-03

- **Repository Repair:**
    - Replaced broken parser gitlinks in `third_party/` with fully tracked vendored source trees so fresh clones configure and build correctly.
    - Added bundled Tree-sitter Swift sources to the repository.
- **Parser / Backend Work:**
    - Added Tree-sitter-backed Swift parsing, including top-level symbols, nested members, and bounded snippets.
    - Added intra-file `Calls` / `Called By` extraction for Swift and PHP.
    - Extended project-side relationship augmentation so the detail pane can surface more useful caller/callee context.
    - Updated filesystem scanning to include Swift files and ignore common Swift build artifacts such as `.build`, `.swiftpm`, and `DerivedData`.
- **CLI / Regression Work:**
    - Updated `tools/regression_sweep.py` to use the current repository path dynamically instead of a stale hard-coded checkout.
    - Expanded the sweep to include Swift files and lightweight relation-presence checks.
    - Re-validated the backend against real local Swift and PHP files through `lumencode-cli --dump-file`.
- **Explorer UI Lockdown:**
    - Made the right detail pane permanently visible as the stable inspector surface.
    - Added clickable `Calls` / `Called By` sections in the detail pane.
    - Reworked the center-pane interaction model so top-level symbols are full-card click targets and nested members are individually hoverable/clickable rows.
    - Restored spacing between outer symbol blocks while keeping nested rows visually distinct and denser than the prior card-in-card layout.
- **Known Remaining Gap:**
    - Relationship traversal is improved but not finished; navigating through `Called By` entries does not always produce the corresponding reciprocal `Calls` view yet.

## 2026-03-29

- Checked local environment constraints
- Found working Qt 5.15 and Kirigami usage in `../it-tools-kirigami`
- Chose a first implementation based on Qt5/KF5 Kirigami
- Added `README.md` and `docs/spec.md`
- Vendored official Tree-sitter sources into `third_party/`
- Added `THIRD_PARTY_NOTICES.md` with source provenance and license notes
- Implemented:
  - `FileSystemModel` as a custom `QAbstractItemModel`
  - `ProjectController` as the QML bridge
  - `SymbolParser` with bundled Tree-sitter-backed parsing for PHP, JS/TS/TSX, and CSS plus heuristic support where retained
  - `Main.qml` with a three-column Kirigami explorer
- Extended the explorer for Node/CommonJS service repos:
  - dependency extraction for `require(...)` and `import`
  - export-aware overview for `module.exports` and `exports.*`
  - Express route detection
  - related file/test linking
  - `package.json` parsing and display
- Refined navigation flow:
  - removed the side-by-side drawer/path entry pattern
  - added a dedicated startup path selection screen
  - added a folder picker dialog
  - added a back action from explorer to picker
- Verified configuration and build with CMake
- Performed an offscreen runtime smoke test
- Observed a non-fatal Kirigami platform plugin warning during offscreen launch on this machine

## 2026-03-30

- **CLI Enhancements:**
    - Introduced `lumencode-cli`, a standalone executable mirroring GUI functionality.
    - Implemented one-shot and persistent interactive modes (`-i`) with JSON command support.
    - Added support for relative path resolution in the CLI.
    - Integrated `projectSummary` into CLI output, providing file type counts and main entry point detection.
- **Parser Improvements:**
    - **Tree-sitter Integration:** Significantly upgraded parsing for JavaScript, TypeScript, PHP, and CSS using Tree-sitter for enhanced accuracy.
    - **Export Handling:** Improved detection and surfacing of exported symbols (CommonJS and ES modules).
    - **Dependency and Route Extraction:** Enhanced Tree-sitter based extraction for imports, exports, `require`, and basic Express route detection.
    - **Symbol Details:** Added source snippets to symbols for better context. Marked symbols as 'exported' where applicable.
    - **Data Contract Hardening:** Ensured `ProjectController::selectPath` uses a skeleton for stable data contracts, preventing QML errors.
    - **Code Cleanup:** Removed unused lambdas (`addVariableDeclarator`, `symbolExists`).
- **Documentation Updates:**
    - Updated `README.md` to reflect current capabilities and roadmap progress.
    - Updated `docs/cli-testing-strategy.md` to detail CLI features like snippets and Tree-sitter usage.
    - Updated `docs/spec.md` for enhanced parsing, symbol details, and project summary.
    - Added entries to this log reflecting recent work.
- **Web-Stack Validation & Follow-up Fixes:**
    - Validated parser behavior against local PHP, Node.js, TypeScript, React/TSX, HTML, and CSS projects under `/home/user/Code`.
    - Fixed CLI interactive mode so relative `selectPath` and `toggleExpanded` commands resolve against the active root.
    - Added explicit handling for the `getProjectSummary` interactive command.
    - Corrected TypeScript language detection so `.ts` files use the TypeScript Tree-sitter grammar instead of the JavaScript fallback.
    - Improved project summary entrypoint selection to prefer root/package-driven app entry files over arbitrary deep `index.*` files.
    - Fixed Express `app.use(...)` route labels so non-string middleware arguments are preserved instead of being truncated.
- **GUI Source Pane Investigation:**
    - Confirmed that symbol snippets are generated in `SymbolParser` and present in the data payload.
    - Confirmed that the GUI does **not** yet render those snippets anywhere.
    - Confirmed that the intended lower source pane has **not** yet been implemented.
    - Confirmed that no syntax-highlighting dependency is currently linked in `CMakeLists.txt`.
    - Confirmed that no lint or parser-diagnostic payload is currently exposed through `ProjectController`.

- **Stability, Fallback, and Regression Work:**
    - Preserved explorer scroll position across left-tree redraws and fixed visible child indentation after folder expansion.
    - Updated the overview pane so PHP class members render as nested cards instead of disappearing into the detail pane.
    - Auto-collapsed the right detail pane when the selection has no unique context.
    - Added bounded filesystem crawl safeguards:
      - max tree depth `64`
      - max scanned nodes `50000`
      - max included entries per directory `2000`
      - synthetic `[skipped: ...]` children when limits are hit
    - Added parser-side graceful degradation for large inputs:
      - large files are summarized instead of fully parsed
      - lower-pane file previews are truncated instead of unbounded
      - auxiliary reads such as CSS/HTML support paths are capped
    - Moved GUI file analysis onto a crash-isolated helper path by routing selection through `lumencode-cli --dump-file`.
    - Introduced an explicit lower-pane snippet contract using `snippetKind` and `diagnosticsMode`, replacing ad hoc assumptions about whether snippets are parseable.
    - Marked dependency, route, fallback-symbol, and file-preview payloads with the new snippet contract so diagnostics are only attempted on standalone constructs.
    - Hardened JS fallback parsing to:
      - bypass unstable native parsing for plain JS/JSX where needed
      - emit snippets for variables/functions/classes
      - anchor declaration snippets cleanly
      - avoid treating control-flow keywords as object/class members
    - Tightened fallback snippet extraction in C++, Java, and C# so declarations no longer inherit leading braces or access labels.
    - Anchored PHP class regexes to real declarations so comments do not create fake classes.
    - Added `tools/regression_sweep.py`, a corpus-based CLI harness that samples files under `/home/user/Code`, drives `selectPath`/`selectSymbolByData`, and validates snippet and diagnostics invariants.
    - Used the sweep harness to reproduce and close regressions across:
      - JS fallback snippets with leading `}`
      - false parser warnings on excerpt snippets
      - C# and C++ declaration previews with leaked surrounding context
      - PHP false-positive class detection from comments
      - JS object/member extraction producing fake `if` / `for` members

## 2026-03-31

- **Explorer UI Refactor:**
    - Replaced the fixed three-column-only explorer with resizable split views.
    - Added an embedded lower source pane.
    - Removed the top toolbar/header from the explorer view.
    - Moved the back action into a thin left control rail for better vertical density.
    - Reduced margins, padding, icon sizes, and row heights throughout the main explorer.
    - Changed the filesystem root label from `.` to `/`.
- **Source Pane State & Rendering:**
    - Added explicit snippet-view state to `ProjectController`.
    - Wired file and symbol selection into a dedicated lower-pane payload.
    - Added internal syntax coloring for snippet display.
    - Added lightweight parser-aware diagnostics for supported languages.
    - Suppressed false diagnostics when snippets are intentionally truncated with `...`.
    - Added file preview snippets when selecting a file directly.
    - Reduced code tab width in the lower pane for better density.
- **HTML/CSS Follow-up Work:**
    - Added structured CSS class summary entries instead of plain class-name strings.
    - Surfaced snippets for CSS class matches and misses in the detail pane.
    - Wired CSS class entries so they can populate the lower source pane.
    - Fixed CSS-file class inspection so Tree-sitter-backed CSS symbols carry rule-level snippets instead of bare selector tokens.
- **Repository Hygiene:**
    - Added a repo-root `.gitignore` containing `build/`.
    - Removed the previously tracked `build/` output tree from git history going forward.

## Next Session Starting Point
- Focus next on stabilization and trustworthiness of the inspection pipeline before adding search.

- Continue broadening the CLI regression corpus and assertion set.
- Improve snippet highlighting fidelity or adopt a stronger highlighting dependency if one is available locally later.
- Improve diagnostics beyond the current conservative parser-aware checks.
- Re-run GUI verification on a real display session and capture any remaining QML/runtime issues.
- Continue reducing noisy HTML/CSS and Node/CommonJS output on real repositories.
- Start native parser rehabilitation only after working from minimized crash repros through `lumencode-cli --dump-file`, keeping the current helper isolation and fallbacks in place until each language path is proven stable.

## Wrap-Up State

Current build state:

- The project builds both the GUI (`lumencode`) and CLI (`lumencode-cli`) targets.
- The CLI tool is fully functional, supports stateful interaction, and provides enhanced output including project summaries and symbol snippets.
- Parsing accuracy has been significantly improved through Tree-sitter integration.
- The GUI now includes a lower source pane with internal syntax coloring and lightweight diagnostics.
- The repository no longer tracks `build/` outputs.

Known runtime/UI problems:

- some output remains noisy or underwhelming on real projects
- HTML/CSS class comparison can still be noisy on complex HTML documents
- highlighting is intentionally lightweight and not yet language-complete
- diagnostics are conservative and not equivalent to a full linter
- some lower-pane context sources are still not wired
- some languages still rely on heuristic fallback paths for stability on hostile files

Important product-direction notes for next time:

- the app needs a stabilization pass before more ambitious features
- the next major usability improvement should be broader lower-pane context wiring and better diagnostics/highlighting
- Node/CommonJS structure support exists but needs refinement to become genuinely useful
- documentation now reflects the current project direction instead of the original bootstrap brief
- helper-process isolation and the regression sweep now form part of the intended safety architecture, not just temporary debugging tools

## 2026-03-30 (continued)

- **Core/GUI Parity Follow-up:**
    - Added `selectedSnippet` to CLI state output so CLI payloads stay aligned with the GUI lower-pane contract.
    - Added interactive `selectSymbolByData` support in `lumencode-cli` so lower-pane payloads for routes, dependencies, and quick links can be exercised without the GUI.
    - Wired dependency, route, and quick-link selections in QML into the shared snippet-selection path instead of file-only navigation.
- **Broader Language Support:**
    - Expanded filesystem scanning to include Python, C/C++, Java, and C# files.
    - Added heuristic parsing for Python, C/C++, Java, and C# symbols plus import/include metadata.
    - Added conservative Flask/FastAPI and ASP.NET route extraction.
    - Added related-file support for C/C++ headers and Python/C# test/project neighbors.
- **Project Summary Improvements:**
    - Added a conventional-entrypoint pass so obvious roots like `src/main.cpp`, `app.py`, and `Program.cs` are preferred before generic fallback scoring.
- **Additional Language Coverage:**
    - Added Rust scanning and heuristic parsing for `use`, `mod`, `fn`, `struct`, `enum`, `trait`, and `impl`.
    - Added Objective-C / Objective-C++ scanning and heuristic parsing for `#import`, `@implementation`, and per-class method extraction.
    - Vendored official Tree-sitter Rust and Python grammars and routed both languages through AST-backed symbol extraction before heuristic fallback.
    - Rust snippets now come from AST node bounds, which fixes prior bleed between adjacent functions inside `impl` blocks.
    - Vendored official Tree-sitter Java and C# grammars and routed both languages through AST-backed symbol extraction before heuristic fallback.
    - Tightened Java field extraction so initializer values are no longer surfaced as fake members.
    - Switched Java and C# import / using payloads to AST-backed line and snippet capture.
- **Project Summary Follow-up:**
    - Added recursive Android-oriented conventional entry detection so `MainActivity.java` is preferred over tests or assets on Android app roots.
- **Desktop / Utility Actions:**
    - Added a bundled app icon plus desktop launcher installation through `cmake --install`.
    - Expanded the left control rail with open-in-folder, open-in-editor, and settings actions.
    - Added a persisted preferred-editor setting with `%f` substitution and system-default fallback.

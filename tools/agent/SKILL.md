---
name: lumencode
description: Structural code navigation with lumencode-cli - file outlines (symbols, signatures, line ranges), who-calls / what-calls across a project, and backend routes with the browser calls that reach them. Use it before reading large files or grepping for usages, in any of PHP, JS/TS, Python, Java, C#, Go, Rust, Swift, Kotlin, Ruby, C/C++, Objective-C, VB.NET, SQL, shell, QML, HTML/CSS/SCSS/LESS.
---

# LumenCode for coding agents

`lumencode-cli` analyses source files structurally (Tree-sitter where a
grammar exists, purpose-built parsers elsewhere) and tolerates broken code:
a syntax error costs the lines around it, not the file. It prints compact
JSON or text, so reading structure costs far fewer tokens than reading the
file.

Locate the binary once: `command -v lumencode-cli` or the project's
`build/bin/lumencode-cli` (a headless build needs only Qt 5 base:
`cmake -S . -B build-cli -DLUMENCODE_CLI_ONLY=ON && cmake --build build-cli`).

## When to use which command

| Question | Command |
|---|---|
| What is in this file? (before opening a large file) | `lumencode-cli --outline <file> --format text` |
| ...with cross-file callers / callees | `lumencode-cli --index-project <root> --outline <file> --format text` |
| Where is `name` defined? | `lumencode-cli --index-project <root> --find <name> --format text` (`Owner.name` narrows) |
| What breaks if I change `name`? Who uses it? | `lumencode-cli --index-project <root> --callers <name> --format text` |
| What does `name` depend on? | `lumencode-cli --index-project <root> --callees <name> --format text` |
| Where is the endpoint for `/api/...`? Which frontend code calls it? | `lumencode-cli --index-project <root> --routes --format text` |
| Every cross-file call edge (bulk analysis) | `lumencode-cli --index-project <root> --index-edges` (JSON lines) |

Drop `--format text` for JSON. The first `--index-project` on a project
builds a cached index (seconds for thousands of files); later calls reuse
it and only re-read changed files.

## Reading the output

- Outline lines read `kind name(signature) -> return  L<start>-<end>  [detail]`,
  followed by `calls:` / `called by:`. Use the line range to read only the
  lines you need (e.g. `sed -n 120,180p file`).
- A relation in another file reads `name @ path:line`; one without a path is
  in the same file.
- `mode` is `ast` (clean parse), `recovered` (syntax errors repaired locally;
  `damaged lines` lists them) or `heuristic` (structural parser). Symbols
  away from damaged lines are reliable.
- Calls are matched by name, not by type. Cross-file edges carry a
  confidence: `high` (an import binds the name), `medium` (the receiver names
  the owning type, or same package), `low` (the only definition of a
  distinctive name). Treat `low` as a strong hint and confirm by reading the
  call site. Calls on receivers of unknown type are often left out rather
  than guessed, so an empty `called by` is not proof that nothing calls it -
  fall back to grep for public API.
- `calledByTotal` means the list was truncated (widely used symbol).

## Tips

- Outline first, then read ranges: this replaces most whole-file reads.
- Before renaming or changing a signature: `--callers`, then check each site.
- For a failing request: `--routes` shows the handler and every client
  (`fetch`, axios, jQuery, forms) that reaches it.
- After editing a file, `--outline` again: if `mode` became `recovered`,
  `damaged lines` point at the syntax error you introduced.

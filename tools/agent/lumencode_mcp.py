#!/usr/bin/env python3
"""MCP server (stdio) exposing lumencode-cli to coding agents.

Tools:
  outline_file     compact outline of one file (symbols, signatures, line ranges, relations)
  find_definition  where a name is defined across the project
  search_symbols   ranked fuzzy search over symbol and file names (partial names, camelCase initials)
  find_callers     who calls a name (same-file and cross-file)
  find_callees     what a name calls
  list_routes      backend routes and the browser calls that reach them
  project_summary  file-type counts, main entry point and package.json summary of a project
  web_links        HTML / CSS / JS links of one file: assets, handlers, DOM ids, CSS classes, broken references
  index_stats      project index statistics (files, edges, build time)

Configuration (environment):
  LUMENCODE_CLI    path to lumencode-cli (default: PATH, then ../../build*/bin next to this file)
  LUMENCODE_ROOT   default project root (default: the server's working directory)

Register with Claude Code, for example:
  claude mcp add lumencode -- python3 /path/to/lumencode/tools/agent/lumencode_mcp.py

No third-party packages: the MCP stdio transport is newline-delimited JSON-RPC 2.0.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROTOCOL_VERSION = "2024-11-05"
MAX_OUTPUT_CHARS = 60_000
HERE = Path(__file__).resolve().parent


def find_cli() -> str | None:
    configured = os.environ.get("LUMENCODE_CLI")
    if configured and Path(configured).is_file():
        return configured
    on_path = shutil.which("lumencode-cli")
    if on_path:
        return on_path
    for build in sorted(HERE.parent.parent.glob("build*/bin/lumencode-cli")):
        if build.is_file():
            return str(build)
    return None


def default_root() -> str:
    return os.environ.get("LUMENCODE_ROOT") or os.getcwd()


ROOT_PROPERTY = {"type": "string", "description": "Project root directory (default: LUMENCODE_ROOT or the working directory)."}
FORMAT_PROPERTY = {"type": "string", "enum": ["text", "json"], "description": "text (compact, default) or json."}

TOOLS = [
    {
        "name": "outline_file",
        "description": "Compact outline of a source file: symbols with kinds, signatures, line ranges and Calls / Called By "
                       "(cross-file when a project root is given). Use before reading a large file, then read only the line "
                       "ranges you need. Also reports damaged lines when the file has syntax errors.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File to outline (absolute, or relative to the root)."},
                "root": ROOT_PROPERTY,
                "cross_file": {"type": "boolean", "description": "Include cross-file relations from the project index (default true)."},
                "format": FORMAT_PROPERTY,
            },
            "required": ["path"],
        },
    },
    {
        "name": "find_definition",
        "description": "Where a function, method, class or other symbol is defined in the project. Accepts 'name' or 'Owner.name'.",
        "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "root": ROOT_PROPERTY, "format": FORMAT_PROPERTY},
                        "required": ["name"]},
    },
    {
        "name": "search_symbols",
        "description": "Find symbols or files when you only half-remember the name: ranked search over definition names "
                       "(Owner.name) and file names - exact, prefix, camelCase initials ('spp' finds SymbolParser.parseFile), "
                       "substring, then fuzzy subsequence. Filter by kind (function, class, method, route, table, file...), "
                       "language or folder. Not a text search: use grep for file contents.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string"},
                "kind": {"type": "string", "description": "Comma-separated kinds, e.g. 'function,method' or 'file'."},
                "language": {"type": "string"},
                "in_path": {"type": "string", "description": "Only paths containing this text."},
                "limit": {"type": "integer", "description": "Maximum results (default 30)."},
                "root": ROOT_PROPERTY,
                "format": FORMAT_PROPERTY,
            },
            "required": ["query"],
        },
    },
    {
        "name": "find_callers",
        "description": "Everything that calls a symbol, same-file and cross-file, with file:line and confidence "
                       "(high = import-bound, medium = owner type / package, low = unique distinctive name). "
                       "Use before changing a signature or behaviour.",
        "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "root": ROOT_PROPERTY, "format": FORMAT_PROPERTY},
                        "required": ["name"]},
    },
    {
        "name": "find_callees",
        "description": "Everything a symbol calls, same-file and cross-file, with file:line and confidence.",
        "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "root": ROOT_PROPERTY, "format": FORMAT_PROPERTY},
                        "required": ["name"]},
    },
    {
        "name": "list_routes",
        "description": "Backend routes in the project (Express, Flask/FastAPI, Spring, ASP.NET, PHP frameworks, Go routers, "
                       "Ktor, Sinatra) with the browser-side calls (fetch, axios, jQuery, forms) that reach each.",
        "inputSchema": {"type": "object", "properties": {"root": ROOT_PROPERTY, "format": FORMAT_PROPERTY}},
    },
    {
        "name": "index_stats",
        "description": "Build or refresh the project index and report its statistics (files, definitions, call sites, "
                       "cross-file edges by evidence, build time).",
        "inputSchema": {"type": "object", "properties": {"root": ROOT_PROPERTY}},
    },
    {
        "name": "project_summary",
        "description": "Orientation for an unfamiliar project: file counts by type, the main entry point, and (when the root has "
                       "a package.json) its name, version, scripts and dependency counts. Cheap; use it first.",
        "inputSchema": {"type": "object", "properties": {"root": ROOT_PROPERTY, "format": FORMAT_PROPERTY}},
    },
    {
        "name": "web_links",
        "description": "How an HTML, CSS or JavaScript file is wired to the rest of a web front end: linked stylesheets and "
                       "scripts, pages and forms; HTML handlers and the functions they call; ids and the scripts that use them; "
                       "CSS classes used, unused, applied from JavaScript, or defined nowhere; and broken references "
                       "(e.g. getElementById('x') with no such element). Check it after editing markup, class names or ids.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "HTML, CSS or JS file (absolute, or relative to the root)."},
                "root": ROOT_PROPERTY,
                "format": FORMAT_PROPERTY,
            },
            "required": ["path"],
        },
    },
]


def run_cli(args: list[str]) -> tuple[str, bool]:
    cli = find_cli()
    if not cli:
        return ("lumencode-cli not found: set LUMENCODE_CLI or put it on PATH "
                "(headless build: cmake -S . -B build-cli -DLUMENCODE_CLI_ONLY=ON && cmake --build build-cli)", True)
    try:
        result = subprocess.run([cli, *args], capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return ("lumencode-cli timed out", True)
    output = result.stdout.strip()
    if result.returncode != 0:
        return (output + "\n" + result.stderr.strip()[-2000:], True)
    if len(output) > MAX_OUTPUT_CHARS:
        output = output[:MAX_OUTPUT_CHARS] + f"\n... (truncated, {len(output)} characters in total)"
    return (output or "(no results)", False)


def rel(path: str, root: str) -> str:
    try:
        return str(Path(path).resolve().relative_to(root))
    except (ValueError, OSError):
        return path


def run_cli_json(args: list[str]):
    """Run the CLI and parse its JSON output; returns (data, error_text)."""
    cli = find_cli()
    if not cli:
        return None, run_cli([])[0]
    try:
        result = subprocess.run([cli, *args], capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None, "lumencode-cli timed out"
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if result.returncode != 0 or not lines:
        return None, (result.stdout.strip() + "\n" + result.stderr.strip()[-2000:]).strip() or "no output"
    try:
        return json.loads(result.stdout), None
    except json.JSONDecodeError as error:
        return None, f"unparseable lumencode-cli output: {error}"


def project_summary(root: str, output_format: str) -> tuple[str, bool]:
    if not Path(root).is_dir():
        return (f"not a directory: {root}", True)
    cli_input = '{"command":"getProjectSummary"}\n'
    cli = find_cli()
    if not cli:
        return run_cli([])
    try:
        result = subprocess.run([cli, root, "-i"], input=cli_input, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return ("lumencode-cli timed out", True)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        return (result.stderr.strip()[-2000:] or "no output", True)
    try:
        summary = json.loads(lines[-1]).get("projectSummary") or {}
    except json.JSONDecodeError as error:
        return (f"unparseable lumencode-cli output: {error}", True)
    summary = {
        "root": root,
        "totalFiles": summary.get("totalFiles", 0),
        "fileTypes": summary.get("fileTypes", {}),
        "mainEntry": rel(summary["mainEntry"], root) if summary.get("mainEntry") else None,
    }
    package_file = Path(root) / "package.json"
    if package_file.is_file():
        data, _ = run_cli_json(["--dump-file", str(package_file)])
        package = (data or {}).get("packageSummary") or {}
        if package:
            summary["package"] = {key: package[key] for key in ("name", "version", "main", "scripts", "dependencyCount") if key in package}
    if output_format == "json":
        return (json.dumps(summary, indent=2), False)
    types = ", ".join(f"{name} {count}" for name, count in sorted(summary["fileTypes"].items(), key=lambda item: -item[1]))
    out = [f"{summary['root']}: {summary['totalFiles']} files ({types})",
           f"main entry: {summary['mainEntry'] or '(none found)'}"]
    package = summary.get("package")
    if package:
        out.append(f"package: {package.get('name', '?')} {package.get('version', '')}".rstrip()
                   + (f", main {package['main']}" if package.get("main") else "")
                   + f", {package.get('dependencyCount', 0)} dependencies")
        for name, command in (package.get("scripts") or {}).items():
            out.append(f"  script {name}: {command}")
    return ("\n".join(out), False)


def web_links(path: Path, root: str, output_format: str) -> tuple[str, bool]:
    if not path.is_file():
        return (f"not a file: {path}", True)
    data, error = run_cli_json(["--dump-file", str(path)])
    if error:
        return (error, True)
    language = data.get("language")
    if language not in ("html", "css", "script", "php", "scss", "less"):
        return (f"{rel(str(path), root)} is a {language} file; web_links covers HTML, CSS and JavaScript "
                "(PHP templates that render HTML also work)", True)
    css = data.get("cssSummary") or {}
    groups: dict[str, list[str]] = {}

    def add(group: str, text: str) -> None:
        groups.setdefault(group, []).append(text)

    for link in data.get("quickLinks") or []:
        kind = link.get("type", "")
        where = f"{rel(link.get('path') or link.get('targetPath') or '', root)}:{link.get('line', '')}".rstrip(":")
        label = link.get("label", "")
        if kind.endswith("-missing") or link.get("exists") is False:
            add("BROKEN", f"{label}  [{link.get('detail', kind)}]")
        elif kind in ("stylesheet", "script", "page", "form", "import"):
            add("links out", f"{kind} {link.get('target', label)}  (L{link.get('line', '?')})")
        elif kind == "consumer":
            add("referenced by HTML", f"{rel(link.get('path', ''), root)}  (L{link.get('line', '?')})")
        elif kind in ("dom-id", "dom-id-use"):
            add("DOM ids", f"{label}  [{link.get('detail', '')}]")
        elif kind == "script-class" and language == "css":
            continue  # listed under "classes applied by JS"
        elif kind in ("css-class", "script-class"):
            add("CSS classes via JS", f"{label}  [{link.get('detail', '')}]")
        else:
            add(kind or "other", f"{label}  [{link.get('detail', '')}]" if label else where)
    for symbol in data.get("symbols") or []:
        if data.get("language") == "html" and symbol.get("kind") in ("handler", "element", "component", "form", "style", "script"):
            calls = ", ".join(f"{c.get('name')} @ {rel(c['path'], root) + ':' + str(c.get('line')) if c.get('path') else 'inline'}"
                              for c in symbol.get("calls") or [])
            suffix = f" -> {calls}" if calls else ""
            add({"handler": "handlers", "element": "ids"}.get(symbol["kind"], symbol["kind"] + "s"),
                f"L{symbol.get('line')} {symbol.get('name')}  {symbol.get('detail', '')}{suffix}".rstrip())
        elif language in ("script", "php"):
            called_by = [c for c in symbol.get("calledBy") or [] if c.get("language") == "html"]
            for caller in called_by:
                add("called from HTML", f"{symbol.get('name')} <- {caller.get('name')} @ {rel(caller.get('path', ''), root)}:{caller.get('line')}")
    for entry in css.get("missingClasses") or []:
        add("classes used in HTML but not in linked CSS", f"{entry.get('name')} ({rel(entry.get('path', ''), root)}:{entry.get('line')})")
    if language == "css":
        if css.get("unusedClasses"):
            add("unused classes", ", ".join(css["unusedClasses"]))
        for entry in css.get("scriptAppliedClasses") or []:
            add("classes applied by JS", f"{entry.get('name')} <- {rel(entry.get('path', ''), root)}:{entry.get('line')} ({entry.get('via')})")
    payload = {"file": rel(str(path), root), "language": language, "summary": data.get("summary", ""), **groups}
    if output_format == "json":
        return (json.dumps(payload, indent=2), False)
    order = ["BROKEN", "classes used in HTML but not in linked CSS", "links out", "referenced by HTML", "handlers", "ids", "called from HTML",
             "DOM ids", "CSS classes via JS", "classes applied by JS", "unused classes"]
    keys = order + [key for key in groups if key not in order]
    out = [f"{payload['file']} ({language}): {payload['summary']}"]
    for key in keys:
        if key in groups:
            out.append(f"{key}:")
            out.extend(f"  {item}" for item in groups[key])
    if len(out) == 1:
        out.append("(no web links found)")
    return ("\n".join(out), False)


def call_tool(name: str, arguments: dict) -> tuple[str, bool]:
    root = str(Path(arguments.get("root") or default_root()).resolve())
    output_format = arguments.get("format") or "text"
    if name == "outline_file":
        path = Path(arguments["path"])
        if not path.is_absolute():
            path = Path(root) / path
        args = ["--outline", str(path), "--format", output_format]
        if arguments.get("cross_file", True):
            args = ["--index-project", root, *args]
        return run_cli(args)
    if name == "find_definition":
        return run_cli(["--index-project", root, "--find", arguments["name"], "--format", output_format])
    if name == "search_symbols":
        args = ["--index-project", root, "--search", arguments["query"], "--format", output_format,
                "--limit", str(int(arguments.get("limit") or 30))]
        for flag, key in (("--kind", "kind"), ("--language", "language"), ("--in", "in_path")):
            if arguments.get(key):
                args += [flag, str(arguments[key])]
        return run_cli(args)
    if name == "find_callers":
        return run_cli(["--index-project", root, "--callers", arguments["name"], "--format", output_format])
    if name == "find_callees":
        return run_cli(["--index-project", root, "--callees", arguments["name"], "--format", output_format])
    if name == "list_routes":
        return run_cli(["--index-project", root, "--routes", "--format", output_format])
    if name == "project_summary":
        return project_summary(root, output_format)
    if name == "web_links":
        path = Path(arguments["path"])
        if not path.is_absolute():
            path = Path(root) / path
        return web_links(path, root, output_format)
    if name == "index_stats":
        return run_cli(["--index-project", root])
    return (f"unknown tool: {name}", True)


def respond(message_id, result=None, error=None) -> None:
    payload = {"jsonrpc": "2.0", "id": message_id}
    if error is not None:
        payload["error"] = error
    else:
        payload["result"] = result
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


def handle(message: dict) -> None:
    method = message.get("method")
    message_id = message.get("id")
    if message_id is None:
        return  # notification (e.g. notifications/initialized)
    if method == "initialize":
        respond(message_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "lumencode", "version": "0.1.0"},
        })
    elif method == "ping":
        respond(message_id, {})
    elif method == "tools/list":
        respond(message_id, {"tools": TOOLS})
    elif method == "tools/call":
        params = message.get("params") or {}
        try:
            text, is_error = call_tool(params.get("name", ""), params.get("arguments") or {})
        except KeyError as missing:
            text, is_error = (f"missing argument: {missing}", True)
        respond(message_id, {"content": [{"type": "text", "text": text}], "isError": is_error})
    else:
        respond(message_id, error={"code": -32601, "message": f"method not found: {method}"})


def main() -> int:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            respond(None, error={"code": -32700, "message": "parse error"})
            continue
        if isinstance(message, list):
            for item in message:
                handle(item)
        else:
            handle(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())

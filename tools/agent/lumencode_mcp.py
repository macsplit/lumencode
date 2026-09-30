#!/usr/bin/env python3
"""MCP server (stdio) exposing lumencode-cli to coding agents.

Tools:
  outline_file     compact outline of one file (symbols, signatures, line ranges, relations)
  find_definition  where a name is defined across the project
  find_callers     who calls a name (same-file and cross-file)
  find_callees     what a name calls
  list_routes      backend routes and the browser calls that reach them
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
    if name == "find_callers":
        return run_cli(["--index-project", root, "--callers", arguments["name"], "--format", output_format])
    if name == "find_callees":
        return run_cli(["--index-project", root, "--callees", arguments["name"], "--format", output_format])
    if name == "list_routes":
        return run_cli(["--index-project", root, "--routes", "--format", output_format])
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

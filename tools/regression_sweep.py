#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path("/home/user/Code/OpenSource/lumencode")
CLI_PATH = REPO_ROOT / "build" / "bin" / "lumencode-cli"
CODE_ROOT = Path("/home/user/Code")

SUPPORTED_EXTENSIONS = {
    ".php", ".js", ".jsx", ".ts", ".tsx", ".py", ".java", ".cs", ".rs",
    ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".m", ".mm",
    ".html", ".css", ".json",
}

EXCLUDED_PARTS = {
    ".git", "node_modules", "dist", "build", "bin", "obj", ".gradle", ".idea",
    ".vscode", "coverage", "vendor", "third_party", "Archive", "venv",
    "site-packages", "__pycache__", ".claude", "article_cache",
}

PRIMARY_KINDS = {"function", "method", "constructor", "class", "variable", "property", "module"}
PRIMARY_SNIPPET_KINDS = {"line_excerpt", "block_excerpt", "exact_construct"}
INVALID_MEMBER_NAMES = {"if", "for", "while", "switch", "catch", "function", "return", "else", "do", "try"}


def run_cli_dump(file_path: Path) -> dict:
    proc = subprocess.run(
        [str(CLI_PATH), "--dump-file", str(file_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=20,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"dump failed rc={proc.returncode}: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def run_cli_commands(commands: list[dict]) -> dict:
    proc = subprocess.run(
        [str(CLI_PATH), "--interactive"],
        input="\n".join(json.dumps(command) for command in commands) + "\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=20,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"interactive failed rc={proc.returncode}: {proc.stderr.strip()}")
    states = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    if not states:
        raise RuntimeError("interactive returned no states")
    return states[-1]


def relative_parts(path: Path) -> tuple[str, ...]:
    return path.relative_to(CODE_ROOT).parts


def project_key(path: Path) -> str:
    parts = relative_parts(path)
    return "/".join(parts[:2]) if len(parts) >= 2 else str(path.parent)


def is_candidate(path: Path) -> bool:
    if path.suffix.lower() not in SUPPORTED_EXTENSIONS:
        return False
    parts = set(path.parts)
    if parts & EXCLUDED_PARTS:
        return False
    lower_name = path.name.lower()
    if lower_name.endswith(".min.js") or lower_name == "package-lock.json":
        return False
    if lower_name.startswith("."):
        return False
    return True


def extension_priority(path: Path) -> tuple[int, str]:
    priority = {
        ".php": 0,
        ".js": 1,
        ".ts": 2,
        ".tsx": 3,
        ".py": 4,
        ".cs": 5,
        ".java": 6,
        ".cpp": 7,
        ".h": 8,
        ".rs": 9,
        ".html": 10,
        ".css": 11,
        ".json": 12,
    }
    return (priority.get(path.suffix.lower(), 99), str(path))


def discover_candidates(limit_per_project: int = 6, max_files: int = 100) -> list[Path]:
    grouped: dict[str, list[Path]] = defaultdict(list)
    for path in CODE_ROOT.rglob("*"):
        if not path.is_file():
            continue
        if not is_candidate(path):
            continue
        grouped[project_key(path)].append(path)

    selected: list[Path] = []
    for key in sorted(grouped):
        chosen = sorted(grouped[key], key=extension_priority)[:limit_per_project]
        selected.extend(chosen)
        if len(selected) >= max_files:
            break
    return selected[:max_files]


def file_root_for(path: Path) -> Path:
    return path.parent if path.parent != CODE_ROOT else CODE_ROOT


def add_issue(issues: list[dict], issue_type: str, file_path: Path, **payload) -> None:
    entry = {"type": issue_type, "file": str(file_path)}
    entry.update(payload)
    issues.append(entry)


def validate_selected_snippet(file_path: Path, state: dict, issues: list[dict], context: str) -> None:
    snippet = state.get("selectedSnippet", {})
    kind = snippet.get("kind", "")
    snippet_kind = snippet.get("snippetKind", "")
    diagnostics_mode = snippet.get("diagnosticsMode", "")
    snippet_text = snippet.get("snippet", "")
    diagnostics = snippet.get("diagnostics", [])

    if not snippet_kind:
        add_issue(issues, "missing_snippet_kind", file_path, context=context, kind=kind)
    if not diagnostics_mode:
        add_issue(issues, "missing_diagnostics_mode", file_path, context=context, kind=kind)
    if diagnostics_mode == "none" and diagnostics:
        add_issue(issues, "diagnostics_present_when_disabled", file_path, context=context, kind=kind, diagnostics=diagnostics[:2])
    if snippet_kind == "line_excerpt" and "\n" in snippet_text:
        add_issue(issues, "line_excerpt_multiline", file_path, context=context, kind=kind, snippet=snippet_text[:200])
    if snippet_kind in {"block_excerpt", "exact_construct"} and snippet_text.lstrip().startswith("}"):
        add_issue(issues, "snippet_starts_with_closing_brace", file_path, context=context, kind=kind, snippet=snippet_text[:200])
    if snippet_kind in {"block_excerpt", "exact_construct"}:
        leading_line = snippet_text.lstrip().splitlines()[0].strip() if snippet_text.strip() else ""
        if leading_line in {
            "public:", "private:", "protected:", "signals:",
            "public slots:", "private slots:", "protected slots:",
        }:
            add_issue(issues, "snippet_starts_with_access_label", file_path, context=context, kind=kind, snippet=snippet_text[:200])


def inspect_file(file_path: Path, issues: list[dict]) -> None:
    try:
        parsed = run_cli_dump(file_path)
    except Exception as exc:
        add_issue(issues, "parse_failure", file_path, message=str(exc))
        return

    symbols = parsed.get("symbols", []) or []
    dependencies = parsed.get("dependencies", []) or []
    routes = parsed.get("routes", []) or []

    for symbol in symbols[:6]:
        kind = symbol.get("kind", "")
        if kind in PRIMARY_KINDS and not symbol.get("snippet", ""):
            add_issue(issues, "empty_primary_symbol_snippet", file_path, kind=kind, name=symbol.get("name", ""))
        if kind in PRIMARY_KINDS and symbol.get("snippetKind") in PRIMARY_SNIPPET_KINDS and not symbol.get("diagnosticsMode"):
            add_issue(issues, "symbol_missing_contract", file_path, kind=kind, name=symbol.get("name", ""))

        try:
            state = run_cli_commands([
                {"command": "setRootPath", "params": {"path": str(file_root_for(file_path))}},
                {"command": "selectPath", "params": {"path": str(file_path)}},
                {"command": "selectSymbolByData", "params": symbol},
            ])
            validate_selected_snippet(file_path, state, issues, context=f"symbol:{symbol.get('name', '')}")
        except Exception as exc:
            add_issue(issues, "symbol_selection_failure", file_path, kind=kind, name=symbol.get("name", ""), message=str(exc))

        members = symbol.get("members", []) or []
        for member in members:
            if member.get("name", "") in INVALID_MEMBER_NAMES:
                add_issue(issues, "invalid_member_name", file_path, kind=member.get("kind", ""), name=member.get("name", ""))
        for member in members[:3]:
            try:
                state = run_cli_commands([
                    {"command": "setRootPath", "params": {"path": str(file_root_for(file_path))}},
                    {"command": "selectPath", "params": {"path": str(file_path)}},
                    {"command": "selectSymbolByData", "params": member},
                ])
                validate_selected_snippet(file_path, state, issues, context=f"member:{member.get('name', '')}")
            except Exception as exc:
                add_issue(issues, "member_selection_failure", file_path, kind=member.get("kind", ""), name=member.get("name", ""), message=str(exc))

    for dep in dependencies[:4]:
        if dep.get("snippet") and dep.get("diagnosticsMode") == "" and dep.get("skipDiagnostics") is False:
            add_issue(issues, "dependency_missing_contract", file_path, name=dep.get("label", dep.get("target", "")))
        try:
            dep_payload = {
                "kind": "dependency",
                "name": dep.get("label") or dep.get("target") or "",
                "sourcePath": parsed.get("path", str(file_path)),
                "sourceLanguage": parsed.get("language", ""),
                "line": dep.get("line", 0),
                "snippet": dep.get("snippet", ""),
                "detail": dep.get("detail", "dependency"),
            }
            if "snippetKind" in dep:
                dep_payload["snippetKind"] = dep["snippetKind"]
            if "diagnosticsMode" in dep:
                dep_payload["diagnosticsMode"] = dep["diagnosticsMode"]
            state = run_cli_commands([
                {"command": "setRootPath", "params": {"path": str(file_root_for(file_path))}},
                {"command": "selectPath", "params": {"path": str(file_path)}},
                {"command": "selectSymbolByData", "params": dep_payload},
            ])
            validate_selected_snippet(file_path, state, issues, context=f"dependency:{dep_payload['name']}")
        except Exception as exc:
            add_issue(issues, "dependency_selection_failure", file_path, name=dep.get("label", dep.get("target", "")), message=str(exc))

    for route in routes[:2]:
        try:
            route_payload = {
                "kind": "route",
                "name": route.get("label") or route.get("path") or "",
                "sourcePath": parsed.get("path", str(file_path)),
                "sourceLanguage": parsed.get("language", ""),
                "line": route.get("line", 0),
                "snippet": route.get("snippet", ""),
                "detail": route.get("detail", "route"),
            }
            if "snippetKind" in route:
                route_payload["snippetKind"] = route["snippetKind"]
            if "diagnosticsMode" in route:
                route_payload["diagnosticsMode"] = route["diagnosticsMode"]
            state = run_cli_commands([
                {"command": "setRootPath", "params": {"path": str(file_root_for(file_path))}},
                {"command": "selectPath", "params": {"path": str(file_path)}},
                {"command": "selectSymbolByData", "params": route_payload},
            ])
            validate_selected_snippet(file_path, state, issues, context=f"route:{route_payload['name']}")
        except Exception as exc:
            add_issue(issues, "route_selection_failure", file_path, name=route.get("label", route.get("path", "")), message=str(exc))


def main() -> int:
    parser = argparse.ArgumentParser(description="Run lumencode regression probes across sampled projects.")
    parser.add_argument("--limit-per-project", type=int, default=6)
    parser.add_argument("--max-files", type=int, default=100)
    args = parser.parse_args()

    if not CLI_PATH.exists():
        print(json.dumps({"error": f"CLI not found at {CLI_PATH}"}))
        return 1

    files = discover_candidates(limit_per_project=args.limit_per_project, max_files=args.max_files)
    issues: list[dict] = []
    for file_path in files:
        inspect_file(file_path, issues)

    summary = {
        "files_tested": len(files),
        "limit_per_project": args.limit_per_project,
        "max_files": args.max_files,
        "issues_found": len(issues),
        "files": [str(path) for path in files],
        "issues": issues,
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

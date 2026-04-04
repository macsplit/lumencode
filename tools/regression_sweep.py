#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CLI_PATH = REPO_ROOT / "build" / "bin" / "lumencode-cli"
CODE_ROOT = Path("/home/user/Code")
DEFAULT_FIXTURE_MANIFEST = REPO_ROOT / "tests" / "fixtures" / "baseline" / "manifest.json"

SUPPORTED_EXTENSIONS = {
    ".php", ".js", ".jsx", ".ts", ".tsx", ".py", ".java", ".cs", ".rs",
    ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".m", ".mm",
    ".html", ".qml", ".css", ".json", ".swift",
}

EXCLUDED_PARTS = {
    ".git", "node_modules", "dist", "build", "bin", "obj", ".gradle", ".idea",
    ".vscode", "coverage", "vendor", "third_party", "Archive", "venv",
    "site-packages", "__pycache__", ".claude", "article_cache", ".swiftpm",
    ".build",
    "DerivedData",
}

PRIMARY_KINDS = {"function", "method", "constructor", "class", "variable", "property", "module"}
PRIMARY_SNIPPET_KINDS = {"line_excerpt", "block_excerpt", "exact_construct"}
INVALID_MEMBER_NAMES = {"if", "for", "while", "switch", "catch", "function", "return", "else", "do", "try"}
RELATION_EXPECTED_LANGUAGES = {"javascript", "typescript", "tsx", "php", "swift", "python", "rust", "java", "csharp"}
CALLABLE_KINDS = {"function", "method", "constructor"}


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
        ".swift": 4,
        ".py": 5,
        ".cs": 6,
        ".java": 7,
        ".cpp": 8,
        ".h": 9,
        ".rs": 10,
        ".html": 11,
        ".qml": 12,
        ".css": 13,
        ".json": 14,
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


def count_relations(symbols: list[dict]) -> tuple[int, int]:
    calls = 0
    called_by = 0
    stack = list(symbols)
    while stack:
        symbol = stack.pop()
        calls += len(symbol.get("calls", []) or [])
        called_by += len(symbol.get("calledBy", []) or [])
        stack.extend(symbol.get("members", []) or [])
    return calls, called_by


def count_callable_symbols(symbols: list[dict]) -> int:
    total = 0
    stack = list(symbols)
    while stack:
        symbol = stack.pop()
        if symbol.get("kind", "") in CALLABLE_KINDS:
            total += 1
        stack.extend(symbol.get("members", []) or [])
    return total


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


def select_relation_state(file_path: Path, parsed: dict, relation: dict) -> dict:
    payload = {
        "kind": relation.get("kind", "symbol"),
        "name": relation.get("name", ""),
        "path": relation.get("path", relation.get("sourcePath", "")),
        "sourcePath": relation.get("path", relation.get("sourcePath", "")),
        "language": relation.get("language", relation.get("sourceLanguage", "")),
        "sourceLanguage": relation.get("language", relation.get("sourceLanguage", "")),
        "line": relation.get("line", 0),
        "snippet": relation.get("snippet", ""),
        "snippetKind": relation.get("snippetKind", ""),
        "diagnosticsMode": relation.get("diagnosticsMode", ""),
        "detail": relation.get("detail", ""),
    }
    return run_cli_commands([
        {"command": "setRootPath", "params": {"path": str(file_root_for(file_path))}},
        {"command": "selectPath", "params": {"path": str(file_path)}},
        {"command": "selectSymbolByData", "params": payload},
    ])


def relation_names(symbol: dict, field: str) -> set[str]:
    return {
        entry.get("name", "")
        for entry in (symbol.get(field, []) or [])
        if entry.get("name", "")
    }


def iter_symbols(symbols: list[dict]):
    stack = list(symbols)
    while stack:
        symbol = stack.pop()
        yield symbol
        stack.extend(symbol.get("members", []) or [])


def find_symbol(symbols: list[dict], name: str, kind: str | None = None) -> dict | None:
    for symbol in iter_symbols(symbols):
        if symbol.get("name", "") != name:
            continue
        if kind and symbol.get("kind", "") != kind:
            continue
        return symbol
    return None


def validate_relation_roundtrip(file_path: Path, parsed: dict, owner: dict, issues: list[dict], context: str) -> None:
    owner_name = owner.get("name", "")
    if not owner_name:
        return

    for field, reverse_field in (("calls", "calledBy"), ("calledBy", "calls")):
        relations = owner.get(field, []) or []
        for relation in relations[:2]:
            try:
                state = select_relation_state(file_path, parsed, relation)
            except Exception as exc:
                add_issue(issues, "relation_selection_failure", file_path, context=context, relation_type=field, name=relation.get("name", ""), message=str(exc))
                continue

            selected_symbol = state.get("selectedSymbol", {}) or {}
            if not selected_symbol:
                add_issue(issues, "relation_selection_empty", file_path, context=context, relation_type=field, name=relation.get("name", ""))
                continue

            reverse_names = relation_names(selected_symbol, reverse_field)
            if owner_name not in reverse_names:
                add_issue(
                    issues,
                    "relation_roundtrip_missing_reverse",
                    file_path,
                    context=context,
                    relation_type=field,
                    reverse_type=reverse_field,
                    owner=owner_name,
                    target=relation.get("name", ""),
                    target_line=selected_symbol.get("line", 0),
                )


def inspect_file(file_path: Path, issues: list[dict]) -> None:
    try:
        parsed = run_cli_dump(file_path)
    except Exception as exc:
        add_issue(issues, "parse_failure", file_path, message=str(exc))
        return

    symbols = parsed.get("symbols", []) or []
    dependencies = parsed.get("dependencies", []) or []
    routes = parsed.get("routes", []) or []
    language = (parsed.get("language", "") or "").lower()

    if language in RELATION_EXPECTED_LANGUAGES and symbols:
        calls_count, called_by_count = count_relations(symbols)
        callable_count = count_callable_symbols(symbols)
        if callable_count >= 2 and calls_count == 0 and called_by_count == 0:
            add_issue(issues, "missing_relations_for_language", file_path, language=language, symbol_count=len(symbols), callable_count=callable_count)

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

        if language in RELATION_EXPECTED_LANGUAGES:
            validate_relation_roundtrip(file_path, parsed, symbol, issues, context=f"symbol:{symbol.get('name', '')}")

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
            if language in RELATION_EXPECTED_LANGUAGES:
                validate_relation_roundtrip(file_path, parsed, member, issues, context=f"member:{member.get('name', '')}")

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


def inspect_fixture_case(case: dict, issues: list[dict]) -> None:
    root = (REPO_ROOT / case["root"]).resolve()
    file_path = (root / case["file"]).resolve()
    case_name = case.get("name", str(file_path))

    try:
        state = run_cli_commands([
            {"command": "setRootPath", "params": {"path": str(root)}},
            {"command": "selectPath", "params": {"path": str(file_path)}},
        ])
    except Exception as exc:
        add_issue(issues, "fixture_selection_failure", file_path, case=case_name, message=str(exc))
        return

    parsed = state.get("selectedFileData", {}) or {}
    symbols = parsed.get("symbols", []) or []
    expected_symbols = case.get("expectations", [])
    file_expectations = case.get("file_expectations", {})

    if not symbols and expected_symbols:
        add_issue(issues, "fixture_no_symbols", file_path, case=case_name)
        return

    if "quick_links" in file_expectations:
        actual = len(parsed.get("quickLinks", []) or [])
        if actual < int(file_expectations["quick_links"]):
            add_issue(issues, "fixture_missing_quick_links", file_path, case=case_name, expected=file_expectations["quick_links"], actual=actual)

    if "dependencies" in file_expectations:
        actual = len(parsed.get("dependencies", []) or [])
        if actual < int(file_expectations["dependencies"]):
            add_issue(issues, "fixture_missing_dependencies", file_path, case=case_name, expected=file_expectations["dependencies"], actual=actual)

    if "routes" in file_expectations:
        actual = len(parsed.get("routes", []) or [])
        if actual < int(file_expectations["routes"]):
            add_issue(issues, "fixture_missing_routes", file_path, case=case_name, expected=file_expectations["routes"], actual=actual)

    if "css_matched_classes" in file_expectations:
        css_summary = parsed.get("cssSummary", {}) or {}
        actual = len(css_summary.get("matchedClasses", []) or [])
        if actual < int(file_expectations["css_matched_classes"]):
            add_issue(issues, "fixture_missing_css_matches", file_path, case=case_name, expected=file_expectations["css_matched_classes"], actual=actual)

    if "css_missing_classes" in file_expectations:
        css_summary = parsed.get("cssSummary", {}) or {}
        actual = len(css_summary.get("missingClasses", []) or [])
        if actual < int(file_expectations["css_missing_classes"]):
            add_issue(issues, "fixture_missing_css_gaps", file_path, case=case_name, expected=file_expectations["css_missing_classes"], actual=actual)

    if "package_scripts" in file_expectations:
        package_summary = parsed.get("packageSummary", {}) or {}
        actual = len((package_summary.get("scripts", {}) or {}).keys())
        if actual < int(file_expectations["package_scripts"]):
            add_issue(issues, "fixture_missing_package_scripts", file_path, case=case_name, expected=file_expectations["package_scripts"], actual=actual)

    for expectation in expected_symbols:
        symbol = find_symbol(symbols, expectation["name"], expectation.get("kind"))
        if not symbol:
            add_issue(issues, "fixture_missing_symbol", file_path, case=case_name, name=expectation["name"], kind=expectation.get("kind", ""))
            continue

        for field in ("calls", "calledBy"):
            expected_names = set(expectation.get(field, []))
            if not expected_names:
                continue
            actual_names = relation_names(symbol, field)
            missing = sorted(expected_names - actual_names)
            if missing:
                add_issue(
                    issues,
                    "fixture_missing_relation",
                    file_path,
                    case=case_name,
                    symbol=expectation["name"],
                    relation_type=field,
                    missing=missing,
                )

        validate_relation_roundtrip(file_path, parsed, symbol, issues, context=f"fixture:{case_name}:{expectation['name']}")


def load_fixture_cases(manifest_path: Path) -> list[dict]:
    if not manifest_path.exists():
        return []
    return json.loads(manifest_path.read_text()).get("cases", [])


def main() -> int:
    parser = argparse.ArgumentParser(description="Run lumencode regression probes across sampled projects.")
    parser.add_argument("--limit-per-project", type=int, default=6)
    parser.add_argument("--max-files", type=int, default=100)
    parser.add_argument("--fixture-manifest", type=Path, default=DEFAULT_FIXTURE_MANIFEST)
    parser.add_argument("--fixtures-only", action="store_true")
    args = parser.parse_args()

    if not CLI_PATH.exists():
        print(json.dumps({"error": f"CLI not found at {CLI_PATH}"}))
        return 1

    issues: list[dict] = []
    fixture_cases = load_fixture_cases(args.fixture_manifest)
    for case in fixture_cases:
        inspect_fixture_case(case, issues)

    files: list[Path] = []
    if not args.fixtures_only:
        files = discover_candidates(limit_per_project=args.limit_per_project, max_files=args.max_files)
        for file_path in files:
            inspect_file(file_path, issues)

    summary = {
        "fixture_manifest": str(args.fixture_manifest),
        "fixture_cases": len(fixture_cases),
        "files_tested": len(files),
        "limit_per_project": args.limit_per_project,
        "max_files": args.max_files,
        "issues_found": len(issues),
        "repo_root": str(REPO_ROOT),
        "cli_path": str(CLI_PATH),
        "files": [str(path) for path in files],
        "issues": issues,
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

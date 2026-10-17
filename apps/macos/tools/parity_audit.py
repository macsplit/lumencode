#!/usr/bin/env python3
"""Check the Swift CLI against the original repository's baseline contract.

Uses expected behavior from the checked-in manifest, not a live original CLI.
Nonzero exit means parity gaps remain. Timings include process launch and JSON.
"""
import argparse
import collections
import json
import pathlib
import subprocess
import time


def flatten(symbols):
    for symbol in symbols:
        yield symbol
        yield from flatten(symbol.get("members", []))


def evaluate(case, analysis):
    symbols = list(flatten(analysis.get("symbols", [])))
    checks = []

    def check(label, actual, expected, subset=False, minimum=False):
        passed = actual >= expected if minimum else (all(value in actual for value in expected) if subset else actual == expected)
        checks.append(dict(label=label, actual=actual, expected=expected, passed=passed))

    for expected in case["expectations"]:
        matches = [s for s in symbols if s.get("kind") == expected["kind"] and s.get("name") == expected["name"]]
        label = expected["kind"] + " " + expected["name"]
        check(label + " exists", bool(matches), True)
        if not matches:
            continue
        symbol = matches[0]
        for key in ("calls", "calledBy"):
            if key in expected:
                check(label + " " + key, [x["name"] for x in symbol.get(key, [])], expected[key], subset=True)
        signature = symbol.get("signature") or {}
        for key in ("sourceMode", "confidence"):
            if key in expected:
                check(label + " " + key, symbol.get(key), expected[key])
        if "parameters" in expected:
            raw_parameters = symbol.get("parameters", signature.get("parameters", []))
            parameters = [p.get("name", "") + (": " + p["type"] if p.get("type") else "") for p in raw_parameters]
            check(label + " parameters", parameters, expected["parameters"])
        if "returns" in expected:
            returns = symbol.get("returns", [signature["returnType"]] if signature.get("returnType") else [])
            check(label + " returns", [x.get("text", "") if isinstance(x, dict) else x for x in returns], expected["returns"])
        if "parameter_defaults" in expected:
            check(label + " defaults", [p.get("default", p.get("defaultValue", "")) for p in symbol.get("parameters", signature.get("parameters", []))], expected["parameter_defaults"])

    file_values = {
        "analysis_source_mode": analysis.get("analysisSourceMode"),
        "analysis_confidence": analysis.get("analysisConfidence"),
        "analysis_partial": bool(analysis.get("analysisPartial", False)),
        "analysis_has_ast_errors": bool(analysis.get("analysisHasAstErrors", False)),
        "analysis_notices": len(analysis.get("analysisNotices", [])),
        "symbol_source_modes": dict(collections.Counter(s.get("sourceMode", "missing") for s in symbols)),
        "symbol_confidences": dict(collections.Counter(s.get("confidence", "missing") for s in symbols)),
        "dependencies": len(analysis.get("dependencies", [])),
        "routes": len(analysis.get("routes", [])),
        "quick_links": len(analysis.get("quickLinks", [])),
        "quick_link_labels": [x.get("label") for x in analysis.get("quickLinks", [])],
        "package_scripts": len(analysis.get("packageSummary", {}).get("scripts", {})),
    }
    for key, expected in case.get("file_expectations", {}).items():
        if key.startswith("css_"):
            field = {"css_matched_classes": "matchedClasses", "css_missing_classes": "missingClasses", "css_unused_classes": "unusedClasses", "css_script_applied_classes": "scriptAppliedClasses"}[key]
            values = analysis.get("cssSummary", {}).get(field, [])
            actual = [x.get("name") if isinstance(x, dict) else x for x in values]
            check(key, len(actual) if isinstance(expected, int) else sorted(actual), expected if isinstance(expected, int) else sorted(expected), minimum=isinstance(expected, int))
        elif key == "max_routes":
            check(key, file_values["routes"] <= expected, True)
        elif key in ("symbol_source_modes", "symbol_confidences"):
            for value, count in expected.items():
                check(key + " " + value, file_values[key].get(value, 0), count, minimum=True)
        else:
            check(key, file_values[key], expected, subset=key == "quick_link_labels", minimum=key in ("dependencies", "routes", "quick_links", "analysis_notices", "package_scripts"))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=pathlib.Path, required=True)
    parser.add_argument("--original", type=pathlib.Path, default=pathlib.Path("../lumencode"))
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    original = args.original.resolve()
    manifest = json.loads((original / "tests/fixtures/baseline/manifest.json").read_text())
    cache, results = {}, []
    for case in manifest["cases"]:
        root = original / case["root"]
        path = root / case["file"]
        key = str(path)
        if key not in cache:
            start = time.perf_counter()
            try:
                proc = subprocess.run([str(args.cli.resolve()), str(root), "--path", str(path)], capture_output=True, text=True, timeout=args.timeout)
                if proc.returncode:
                    raise RuntimeError(f"exit {proc.returncode}: {proc.stderr[:400]}")
                state = json.loads(proc.stdout)
                cache[key] = dict(analysis=state["selectedFileData"], elapsed_ms=(time.perf_counter()-start)*1000, scanned_paths=[x["path"] for x in state["fileSystem"] if not x["isDirectory"]])
            except (subprocess.TimeoutExpired, RuntimeError, ValueError, KeyError) as error:
                cache[key] = dict(error=str(error), elapsed_ms=(time.perf_counter()-start)*1000)
        run = cache[key]
        checks = evaluate(case, run["analysis"]) if "analysis" in run else []
        result = dict(name=case["name"], file=str(path.relative_to(original)), elapsed_ms=run["elapsed_ms"], checks=checks, passed=bool(checks) and all(x["passed"] for x in checks))
        if "error" in run:
            result["error"] = run["error"]
        else:
            result.update(language=run["analysis"]["language"], scanned=str(path) in run["scanned_paths"], symbols=len(list(flatten(run["analysis"].get("symbols", [])))), signatures=sum(bool((s.get("signature") or {}).get("parameters") or (s.get("signature") or {}).get("returnType") or s.get("parameters") or s.get("returns")) for s in flatten(run["analysis"].get("symbols", []))), call_sites=len((run["analysis"].get("indexFacts") or {}).get("callSites", [])) + len(run["analysis"].get("moduleCallSites", [])) + sum(len(s.get("callSites", [])) for s in flatten(run["analysis"].get("symbols", []))))
        results.append(result)
    checks = [x for r in results for x in r["checks"]]
    summary = dict(cases=len(results), cases_passed=sum(r["passed"] for r in results), assertions=len(checks), assertions_passed=sum(x["passed"] for x in checks), unique_invocations=len(cache), errors=sum("error" in r for r in results))
    output = dict(method="Swift project-mode CLI versus original checked-in fixture expectations; no live original binary", summary=summary, results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if summary["cases_passed"] == summary["cases"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

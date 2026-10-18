#!/usr/bin/env python3
"""Build and package the shared engine with relocatable Qt runtime libraries.

Requires CMake, a C/C++ compiler and Qt 5 Core/Concurrent (brew install qt@5).
No sibling checkout is used. The output can be copied into an app's Resources.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, default=repo / ".build/engine")
parser.add_argument("--runtime", type=Path, default=repo / ".build/EngineRuntime")
parser.add_argument("--qt", type=Path, default=Path(os.environ.get("QT5_PREFIX", "/opt/homebrew/opt/qt@5")))
parser.add_argument("--verify-only", action="store_true")
parser.add_argument("--architectures", nargs="+", choices=("arm64", "x86_64"),
                    default=os.environ.get("LUMENCODE_MACOS_ARCHITECTURES", "").split() or None,
                    help="macOS slices; also settable with LUMENCODE_MACOS_ARCHITECTURES")
args = parser.parse_args()
vendor = json.loads((repo / "third_party/provenance.json").read_text())
for record in vendor["files"]:
    if hashlib.sha256((repo / record["path"]).read_bytes()).hexdigest() != record["sha256"]:
        raise SystemExit(f"Vendored input changed: {record['path']}; review its provenance update")
if args.verify_only:
    print(f"Verified {len(vendor['files'])} vendored source inputs")
    raise SystemExit(0)
if not (args.qt / "lib/cmake/Qt5/Qt5Config.cmake").exists():
    raise SystemExit("Qt 5 not found. Install qt@5 or provide --qt /path/to/qt5")
build, runtime = args.build.resolve(), args.runtime.resolve()
architecture_options = ["-DCMAKE_OSX_ARCHITECTURES=" + ";".join(args.architectures)] if args.architectures else []
subprocess.run(["cmake", "-S", str(repo), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", "-DLUMENCODE_BUILD_QT_APP=OFF", "-DLUMENCODE_BUILD_CLI=OFF", "-DLUMENCODE_BUILD_ENGINE_HELPER=ON", "-DCMAKE_PREFIX_PATH=" + str(args.qt), "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", *architecture_options], check=True)
subprocess.run(["cmake", "--build", str(build), "--parallel", "6"], check=True)
(runtime / "bin").mkdir(parents=True, exist_ok=True)
(runtime / "lib").mkdir(exist_ok=True)
binary = runtime / "bin/lumencode-engine"
shutil.copy2(build / "bin/lumencode-engine", binary)


def dependencies(file):
    # Universal binaries repeat dependencies beneath an architecture heading.
    # Only tab-indented entries are load commands; headings are not libraries.
    return list(dict.fromkeys(line.strip().split(" (compatibility")[0]
                for line in subprocess.check_output(["otool", "-L", str(file)], text=True).splitlines()
                if line.startswith("\t")))


def resolve(reference, owner):
    if reference.startswith("@rpath/"):
        tail = reference[len("@rpath/"):]
        candidates = [args.qt / "lib" / tail, owner.parent / tail]
    elif reference.startswith("@loader_path/"):
        candidates = [owner.parent / reference[len("@loader_path/"):]]
    elif reference.startswith("/"):
        candidates = [Path(reference)]
    else:
        raise RuntimeError(f"Unsupported library reference: {reference}")
    match = next((p.resolve() for p in candidates if p.exists()), None)
    if match is None:
        raise RuntimeError(f"Cannot resolve {reference} from {owner}; tried {candidates}")
    return match


copied = {}


def package(source, target, executable=False):
    source = source.resolve()
    if not executable:
        copied[source] = target
        shutil.copy2(source, target)
        target.chmod(0o755)
        subprocess.run(["install_name_tool", "-id", "@loader_path/" + target.name, str(target)], check=True)
    for reference in dependencies(source):
        if reference.startswith(("/System/", "/usr/lib/")):
            continue
        dependency = resolve(reference, source)
        if dependency == source:
            continue
        if dependency not in copied:
            destination = runtime / "lib" / dependency.name
            if destination in copied.values():
                raise RuntimeError(f"Library filename collision: {dependency}")
            package(dependency, destination)
        replacement = ("@executable_path/../lib/" if executable else "@loader_path/") + copied[dependency].name
        subprocess.run(["install_name_tool", "-change", reference, replacement, str(target)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(target)], check=True)


package(build / "bin/lumencode-engine", binary, executable=True)
revision = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True)
source_info = dict(monorepo_commit=revision.stdout.strip() if revision.returncode == 0 else None,
                   historical_origins=json.loads((repo / "engine/origin.json").read_text()),
                   source_sha256={str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in sorted((repo / "engine").rglob("*")) if p.suffix in (".cpp", ".h")},
                   third_party=vendor)
(runtime / "source-provenance.json").write_text(json.dumps(source_info, indent=2) + "\n")
licenses = runtime / "licenses"
licenses.mkdir(exist_ok=True)
shutil.copy2(repo / "LICENSE", licenses / "LumenCode-GPL-3.0.txt")
for source in args.qt.glob("LICENSE*"):
    if source.is_file():
        shutil.copy2(source, licenses / source.name)
for source in (repo / "third_party").rglob("*"):
    if source.is_file() and source.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT")):
        target = licenses / "third_party" / source.relative_to(repo / "third_party")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
shutil.copy2(repo / "THIRD_PARTY_NOTICES.md", licenses / "THIRD_PARTY_NOTICES.md")
# Qt and transitive library formula receipts preserve source/version/license information.
runtime_info = {}
private_qt_receipt = args.qt / "lumencode-qt-build.json"
if private_qt_receipt.is_file():
    runtime_info["private-qt"] = json.loads(private_qt_receipt.read_text())
    if (args.qt / "licenses").is_dir():
        shutil.copytree(args.qt / "licenses", licenses / "qt-source", dirs_exist_ok=True)
for source in copied:
    if "/Cellar/" in str(source):
        parts = source.parts
        cell = parts.index("Cellar")
        prefix = Path(*parts[:cell+3])
        receipt = prefix / "INSTALL_RECEIPT.json"
        formula = prefix / ".brew" / (parts[cell+1] + ".rb")
        if receipt.exists():
            runtime_info[parts[cell+1]] = json.loads(receipt.read_text())
        if formula.exists():
            shutil.copy2(formula, licenses / formula.name)
        for notice in prefix.iterdir():
            if notice.is_file() and notice.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT")):
                shutil.copy2(notice, licenses / (parts[cell+1] + "-" + notice.name))
(runtime / "runtime-dependencies.json").write_text(json.dumps(runtime_info, indent=2) + "\n")
minimum_versions = {}
for file in [binary, *copied.values()]:
    output = subprocess.check_output(["vtool", "-show-build", str(file)], text=True)
    minimums = [line.split()[1] for line in output.splitlines() if line.strip().startswith("minos ")]
    minimum = max(minimums, key=lambda value: tuple(map(int, value.split(".")))) if minimums else "unknown"
    minimum_versions[str(file.relative_to(runtime))] = minimum
(runtime / "minimum-macos.json").write_text(json.dumps(minimum_versions, indent=2) + "\n")
print(f"Engine ready: {binary} ({len(copied)} bundled libraries)")

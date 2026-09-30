#!/usr/bin/env python3
"""Claude Code hooks: outline a file before an edit, check its structure after.

  lumencode_hook.py pre    PreToolUse  (Edit|Write|MultiEdit)
  lumencode_hook.py post   PostToolUse (Edit|Write|MultiEdit)

pre   Takes a structural snapshot of the file (`lumencode-cli --outline`) and
      saves it for the post hook. Files with at least LUMENCODE_HOOK_MIN_LINES
      lines (default 150) also get their outline offered as additional context.
post  Outlines the file again and compares it with the snapshot. It says
      nothing when the edit is structurally unremarkable. It speaks up when
        - the file now has syntax errors LumenCode had to repair around
          (mode became recovered / heuristic, or new damaged lines),
        - symbols were removed or their signatures changed, listing their
          callers from the project index (the check that pays for itself),
        - symbols were added (one line, so new code is visible).

Never blocks and never fails an edit: any problem exits 0 silently.

Environment:
  LUMENCODE_CLI              path to lumencode-cli (default: PATH, then build*/bin next to this repo)
  LUMENCODE_HOOK_MIN_LINES   outline threshold for the pre hook (default 150)
  LUMENCODE_HOOK_CALLERS     0 disables the caller lookup (it may build the project index)
  LUMENCODE_HOOK_DISABLE     1 turns both hooks off
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAX_FILE_BYTES = 1_000_000
MAX_CONTEXT_CHARS = 6_000
MAX_CALLER_SYMBOLS = 5
MAX_CALLER_LINES = 8
CALLER_TIMEOUT = 25
STATE_MAX_AGE = 24 * 3600
EDIT_TOOLS = {"Edit", "Write", "MultiEdit"}
NOISY_KINDS = {"property", "module", "variable", "field", "import", "export"}


def find_cli() -> str | None:
    configured = os.environ.get("LUMENCODE_CLI")
    if configured and Path(configured).is_file():
        return configured
    on_path = shutil.which("lumencode-cli")
    if on_path:
        return on_path
    for build in sorted(HERE.parents[2].glob("build*/bin/lumencode-cli")):
        if build.is_file():
            return str(build)
    return None


def run(cli: str, args: list[str], timeout: int = 15) -> str | None:
    try:
        result = subprocess.run([cli, *args], capture_output=True, text=True, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None
    return result.stdout if result.returncode == 0 else None


def outline(cli: str, path: str, output_format: str) -> str | None:
    return run(cli, ["--outline", path, "--format", output_format])


def outline_json(cli: str, path: str) -> dict | None:
    text = outline(cli, path, "json")
    try:
        data = json.loads(text) if text else None
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict) or data.get("language") in (None, "", "text", "folder"):
        return None
    if str(data.get("summary", "")).startswith("Unable to read"):
        return None
    return data


def state_file(session_id: str, path: str) -> Path:
    directory = Path(tempfile.gettempdir()) / f"lumencode-hooks-{os.getuid()}"
    directory.mkdir(mode=0o700, exist_ok=True)
    now = time.time()
    for old in directory.glob("*.json"):  # keep the directory from growing
        try:
            if now - old.stat().st_mtime > STATE_MAX_AGE:
                old.unlink()
        except OSError:
            pass
    key = hashlib.sha1(f"{session_id}\0{path}".encode()).hexdigest()
    return directory / f"{key}.json"


def edited_file(event: dict) -> str | None:
    if event.get("tool_name") not in EDIT_TOOLS:
        return None
    path = (event.get("tool_input") or {}).get("file_path")
    if not path:
        return None
    path = str(Path(event.get("cwd") or ".") / path) if not os.path.isabs(path) else path
    return path


def flatten(symbols: list[dict], prefix: str = "") -> dict[str, dict]:
    """name (Owner.name for members) -> symbol; a repeated name keeps the first."""
    flat: dict[str, dict] = {}
    for symbol in symbols or []:
        name = prefix + str(symbol.get("name", ""))
        flat.setdefault(name, symbol)
        flat.update({key: value for key, value in flatten(symbol.get("members") or [], name + ".").items()
                     if key not in flat})
    return flat


def emit(event_name: str, text: str) -> None:
    print(json.dumps({"hookSpecificOutput": {"hookEventName": event_name, "additionalContext": text}}))


def pre(event: dict) -> None:
    path = edited_file(event)
    if not path or not os.path.isfile(path) or os.path.getsize(path) > MAX_FILE_BYTES:
        return  # a new file has nothing to outline
    cli = find_cli()
    if not cli:
        return
    snapshot = outline_json(cli, path)
    if snapshot is None:
        return
    state_file(str(event.get("session_id", "")), path).write_text(json.dumps(snapshot))
    try:
        lines = sum(1 for _ in open(path, "rb"))
    except OSError:
        return
    if lines < int(os.environ.get("LUMENCODE_HOOK_MIN_LINES", "150")):
        return
    text = outline(cli, path, "text")
    if text and text.strip():
        if len(text) > MAX_CONTEXT_CHARS:
            text = text[:MAX_CONTEXT_CHARS] + "\n... (outline truncated)"
        emit("PreToolUse", f"LumenCode outline of {path} before this edit ({lines} lines):\n{text.strip()}")


def callers_of(cli: str, root: str, name: str) -> list[str]:
    bare = name.rsplit(".", 1)[-1]
    text = run(cli, ["--index-project", root, "--callers", bare, "--format", "text"], timeout=CALLER_TIMEOUT)
    if not text:
        return []
    return [line.strip() for line in text.splitlines() if line.strip().startswith("<-")]


def post(event: dict) -> None:
    path = edited_file(event)
    if not path or not os.path.isfile(path):
        return
    cli = find_cli()
    if not cli:
        return
    saved = state_file(str(event.get("session_id", "")), path)
    before = None
    if saved.is_file():
        try:
            before = json.loads(saved.read_text())
        except (OSError, json.JSONDecodeError):
            before = None
    after = outline_json(cli, path)
    if after is None:
        return
    saved.write_text(json.dumps(after))  # the next edit compares against this one

    notes: list[str] = []
    old_damage = set((before or {}).get("damagedLines") or [])
    new_damage = sorted(set(after.get("damagedLines") or []) - old_damage)
    if new_damage:
        shown = ", ".join(str(line) for line in new_damage[:12])
        notes.append(f"Syntax problem: LumenCode had to repair around line(s) {shown} "
                     f"(analysis mode: {after.get('mode')}). Check the edit for a missing or extra brace, quote or bracket.")
    elif before and before.get("mode") == "ast" and after.get("mode") not in ("ast", None):
        notes.append(f"Analysis mode fell from ast to {after.get('mode')}: the file no longer parses cleanly.")

    if before is not None:
        old = {name: sym for name, sym in flatten(before.get("symbols") or []).items() if sym.get("kind") not in NOISY_KINDS}
        new = {name: sym for name, sym in flatten(after.get("symbols") or []).items() if sym.get("kind") not in NOISY_KINDS}
        removed = [name for name in old if name not in new]
        changed = [name for name in old if name in new and old[name].get("signature") != new[name].get("signature")
                   and old[name].get("kind") in ("function", "method", "constructor")]
        added = [name for name in new if name not in old]
        if added:
            notes.append("Added: " + ", ".join(added[:10]) + (f" (+{len(added) - 10} more)" if len(added) > 10 else ""))
        damaged = " (the file is damaged, so some of these may be parse artifacts)" if new_damage else ""
        for label, names in (("Removed", removed), ("Signature changed", changed)):
            if not names:
                continue
            notes.append(f"{label}: " + ", ".join(names[:10]) + damaged)
        if (removed or changed) and os.environ.get("LUMENCODE_HOOK_CALLERS", "1") != "0":
            root = str(event.get("cwd") or Path(path).parent)
            looked_up: set[str] = set()
            for name in (removed + changed)[:MAX_CALLER_SYMBOLS]:
                if name.rsplit(".", 1)[-1] in looked_up:
                    continue
                looked_up.add(name.rsplit(".", 1)[-1])
                callers = callers_of(cli, root, name)
                if callers:
                    shown = callers[:MAX_CALLER_LINES]
                    more = f"\n  ... {len(callers) - len(shown)} more" if len(callers) > len(shown) else ""
                    notes.append(f"Callers of {name} (check they still fit; name-based, low confidence means verify):\n  "
                                 + "\n  ".join(shown) + more)
    if notes:
        emit("PostToolUse", f"LumenCode check of {path}:\n" + "\n".join(notes))


def main() -> int:
    if os.environ.get("LUMENCODE_HOOK_DISABLE") == "1" or len(sys.argv) < 2 or sys.argv[1] not in ("pre", "post"):
        return 0
    try:
        event = json.load(sys.stdin)
        (pre if sys.argv[1] == "pre" else post)(event)
    except Exception:  # a hook must never break an edit
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

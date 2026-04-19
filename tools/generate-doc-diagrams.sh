#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIAGRAM_DIR="$ROOT_DIR/docs/diagrams"
OUTPUT_DIR="$ROOT_DIR/docs/generated"

if ! command -v mmdc >/dev/null 2>&1; then
    echo "mmdc not found in PATH" >&2
    echo "Install Mermaid CLI or make it available before generating diagrams." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

shopt -s nullglob
for source in "$DIAGRAM_DIR"/*.mmd; do
    name="$(basename "$source" .mmd)"
    target="$OUTPUT_DIR/$name.svg"
    echo "Generating $target"
    mmdc -i "$source" -o "$target" -t neutral -b transparent
done

echo "Generated diagrams in $OUTPUT_DIR"

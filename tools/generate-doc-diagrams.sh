#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIAGRAM_DIR="$ROOT_DIR/docs/diagrams"
OUTPUT_DIR="$ROOT_DIR/docs/generated"
PUPPETEER_CONFIG="$(mktemp /tmp/lumencode-puppeteer-config.XXXXXX.json)"

if ! command -v mmdc >/dev/null 2>&1; then
    echo "mmdc not found in PATH" >&2
    echo "Install Mermaid CLI or make it available before generating diagrams." >&2
    exit 1
fi

if ! command -v convert >/dev/null 2>&1; then
    echo "convert not found in PATH" >&2
    echo "Install ImageMagick or make it available before generating PNG outputs." >&2
    exit 1
fi

detect_browser() {
    local candidate
    for candidate in \
        "${PUPPETEER_EXECUTABLE_PATH:-}" \
        "$HOME/.cache/puppeteer/chrome-headless-shell/linux-"*/chrome-headless-shell-linux64/chrome-headless-shell \
        /usr/bin/chromium-browser \
        /snap/bin/chromium; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

BROWSER_PATH="$(detect_browser || true)"
if [[ -z "$BROWSER_PATH" ]]; then
    echo "No usable Chromium/Chrome executable found for Mermaid CLI." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
export PUPPETEER_EXECUTABLE_PATH="$BROWSER_PATH"
trap 'rm -f "$PUPPETEER_CONFIG"' EXIT

cat > "$PUPPETEER_CONFIG" <<EOF
{
  "executablePath": "$BROWSER_PATH",
  "args": ["--no-sandbox", "--disable-setuid-sandbox"]
}
EOF

shopt -s nullglob
for source in "$DIAGRAM_DIR"/*.mmd; do
    name="$(basename "$source" .mmd)"
    svg_target="$OUTPUT_DIR/$name.svg"
    png_target="$OUTPUT_DIR/$name.png"
    echo "Generating $svg_target"
    mmdc -p "$PUPPETEER_CONFIG" -i "$source" -o "$svg_target" -t neutral -b transparent
    echo "Generating $png_target"
    convert "$svg_target" "$png_target"
done

echo "Generated diagrams in $OUTPUT_DIR"

# Third-Party Notices

LumenCode vendors parser sources directly in `third_party/` so the application can build without requiring system-installed Tree-sitter packages or grammar packages.

## Bundled Components

### Tree-sitter Core

- Source: https://github.com/tree-sitter/tree-sitter
- Vendored path: `third_party/tree-sitter`
- Revision: `0535b0ca378a3f13a2b469f6c090a0c1b90904b7`
- License: MIT
- License file retained at: `third_party/tree-sitter/LICENSE`

### Tree-sitter JavaScript Grammar

- Source: https://github.com/tree-sitter/tree-sitter-javascript
- Vendored path: `third_party/tree-sitter-javascript`
- Revision: `58404d8cf191d69f2674a8fd507bd5776f46cb11`
- License: MIT
- License file retained at: `third_party/tree-sitter-javascript/LICENSE`

### Tree-sitter TypeScript Grammar

- Source: https://github.com/tree-sitter/tree-sitter-typescript
- Vendored path: `third_party/tree-sitter-typescript`
- Revision: `75b3874edb2dc714fb1fd77a32013d0f8699989f`
- License: MIT
- License file retained at: `third_party/tree-sitter-typescript/LICENSE`

### Tree-sitter PHP Grammar

- Source: https://github.com/tree-sitter/tree-sitter-php
- Vendored path: `third_party/tree-sitter-php`
- Revision: `3f2465c217d0a966d41e584b42d75522f2a3149e`
- License: MIT
- License file retained at: `third_party/tree-sitter-php/LICENSE`

### Tree-sitter HTML Grammar

- Source: https://github.com/tree-sitter/tree-sitter-html
- Vendored path: `third_party/tree-sitter-html`
- Revision: `73a3947324f6efddf9e17c0ea58d454843590cc0`
- License: MIT
- License file retained at: `third_party/tree-sitter-html/LICENSE`

### Tree-sitter CSS Grammar

- Source: https://github.com/tree-sitter/tree-sitter-css
- Vendored path: `third_party/tree-sitter-css`
- Revision: `dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f`
- License: MIT
- License file retained at: `third_party/tree-sitter-css/LICENSE`

## Additional Upstream Notices

The vendored Tree-sitter core also retains upstream Unicode-related notice files inside its source tree, including:

- `third_party/tree-sitter/lib/src/unicode/LICENSE`

Those upstream notice files remain in place as part of the vendored source.

## Integration Notes

- LumenCode links bundled Tree-sitter runtime and grammar sources directly from `third_party/`
- No external Tree-sitter runtime package is required at build time or run time
- Current AST-backed parsing is wired for JavaScript, TypeScript, TSX, PHP, and CSS
- HTML quick-link extraction remains heuristic for now, even though the official HTML grammar is also vendored and documented here

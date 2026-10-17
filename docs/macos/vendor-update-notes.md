# Vendor Update Notes

## Tree-sitter update workflow

When refreshing the Swift parser stack:

1. Update the vendored Tree-sitter runtime from `tree-sitter/tree-sitter`.
2. Update the vendored Swift grammar from `alex-pinkus/tree-sitter-swift`.
3. Refresh generated Swift parser sources if needed.
4. Rebuild both:
   - `swift build`
   - Xcode app target
5. Run the CLI harness against representative Swift files before trusting GUI behavior.

## Keep this split clear

Runtime layer:

- generic Tree-sitter C runtime
- language-agnostic

Grammar layer:

- Swift-specific generated parser
- Swift-specific scanner

Bridge layer:

- our handwritten extraction code
- turns AST nodes into `FileAnalysis` JSON

## Update checklist

- keep upstream license files available
- record upstream source repo and revision in commit messages when possible
- do not require local parser generation for normal development
- treat generated parser files as vendored build inputs, not user-generated artifacts

# macOS distribution progress — 18 October 2026

A universal Release application is prepared with both Intel and Apple Silicon
slices in the app, engine helper and both bundled Qt libraries. All slices target
macOS 14. The private Qt Base 5.15.19 build removes the six incompatible Homebrew
transitive libraries, leaving only Core and Concurrent beside the helper. No
language-analysis or grammar changes were needed.

Validation:

- Both packaged engine slices analyse all 6,297 pinned regular corpus files with
  zero complete-JSON differences against the accepted macOS engine, zero input
  differences and zero failures. Array ordering and every analysis field remain
  part of the comparison.
- The Intel engine under Rosetta passes 104 fixture cases / 638 assertions. The
  102-file complete contract comparison also matches the native engine exactly.
- Nine Swift integration tests pass using the packaged Intel engine.
- The actual production app launches in both forced architectures, remains alive
  for five seconds, and emits no diagnostics. These are startup checks.
- Runtime relocation and architecture/minimum-OS gates pass. These are not a
  substitute for a test on macOS 14 or physical Intel hardware.

See [machine-readable evidence](evidence/universal-runtime.json), the
[full corpus comparison](evidence/universal-corpus.json), and the
[repeatable build/sign/notarize workflow](distribution.md).

The initial single-architecture runtime was used only to establish the private
Qt build and was superseded by the universal candidate. Universal packaging also
required fixing the existing `otool -L` dependency reader: architecture headings
must not be interpreted as imported libraries. Minimum-OS manifests now retain
the highest requirement across slices.

The user's Developer ID signing probe succeeds after fixing the private-key
partition rule. The assistant's execution context still cannot access the login
Keychain, so final signing/notarization may need to run from the user's local
Terminal. No notarized macOS download has been published, and no store submission
has been made. The website remains unchanged by this work.

The prepared version is 0.1.0, build 20261018. Build tools refuse existing output
paths, keep credentials in Keychain, sign nested code explicitly, check the
Developer ID team on all native code, and require Apple acceptance, stapling and
Gatekeeper verification before producing the distribution ZIP.

Raw build and corpus reports are preserved on Kubuntu at
`/home/user/Code/Coding/lumencode-validation-20261018-macos`.

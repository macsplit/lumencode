# Vendored parser updates

The active runtime and all grammars are ordinary checked-in sources in root
`third_party/`, compiled once through the shared CMake engine target. The former
Swift bridge is excluded from builds and has no independent parser stack to update.

Update the relevant upstream inputs, preserve generated parsers and license files,
and record the reviewed revision and hashes in `third_party/provenance.json` and
root `THIRD_PARTY_NOTICES.md`. Normal builds must not regenerate or download them.

Run both CLI fixture/contract checks, the pinned public corpus and damaged-source
comparisons, then rebuild Qt, SwiftPM and Xcode. Use an independent pre-change
executable when validating a parser update. See the [engine guide](../../engine/README.md)
for the commands and [migration evidence](../migration/status.md) for the baseline.

# Direct-download macOS distribution

For direct downloads, package LumenCode as a Developer ID-signed, notarized
universal application.
The release workflow prepares a local package; it does not publish to the website,
create a GitHub release or submit to the Mac App Store.

## Universal runtime

The app executable, engine helper, Qt Core and Qt Concurrent must each contain
both `x86_64` and `arm64` slices, targeting macOS 14. A universal SwiftUI shell
alone is insufficient: an Intel Mac also needs the Intel engine and libraries.

Homebrew's current Qt runtime imports transitive libraries requiring macOS 26/27.
For distribution, build a private Qt Base 5.15.19 kit using the pinned source URL
and SHA256 in `tools/build_macos_qt.py`. This builds Core/Concurrent with bundled
PCRE and without GLib, gettext or zstd. Existing Homebrew packages are untouched.
The helper still uses the canonical shared analysis sources and grammar inputs.

From the repository root on an Apple Silicon build Mac with Xcode and CMake:

```sh
python3 tools/build_macos_qt.py \
  --work /path/to/new/qt-build \
  --prefix /path/to/new/qt-kit

python3 tools/package_macos.py prepare \
  --qt /path/to/new/qt-kit \
  --scratch /path/to/xcode-build \
  --output /path/to/new/release \
  --version 0.1.0 --build-number 20261018
```

Use fresh work/prefix/output directories. The Qt build downloads about 50 MB,
verifies the archive, builds both architectures and retains licence texts and a
source/configuration receipt. `build_engine.py` carries that receipt and those
notices into the packaged runtime. Download tooling needs network access, but
application/engine builds never download or regenerate grammar inputs.

`prepare` explicitly enables both Xcode architectures and sets
`LUMENCODE_MACOS_ARCHITECTURES="x86_64 arm64"` for the engine build phase. The same
engine setting is available as `build_engine.py --architectures x86_64 arm64`.
Ordinary development builds retain the native-architecture default.

Preparation fails if any bundled Mach-O file lacks either architecture or any
slice declares a newer minimum OS than macOS 14. It also verifies relocated
engine analysis without external developer-library paths. These metadata and
relocation checks do not replace testing on macOS 14 or real Intel hardware.

## Signing and notarization

Create a Developer ID Application certificate with its private key on the signing
Mac. Store notarization credentials locally with:

```sh
xcrun notarytool store-credentials lumencode-notary
```

Run the remaining stages in a GUI Terminal session that can access the login
Keychain:

```sh
python3 tools/package_macos.py sign \
  --app /path/to/new/release/LumenCode.app \
  --identity 'Developer ID Application: Your Name (TEAMID)'

python3 tools/package_macos.py notarize \
  --app /path/to/new/release/LumenCode.app \
  --profile lumencode-notary \
  --output /path/to/new/release/LumenCode-macOS-universal.zip
```

Signing explicitly signs nested native code inside out, with hardened runtime
and secure timestamps, then signs the enclosing app. Verification requires a
Developer ID Application signature and matching team for every Mach-O file.
`--deep` is used for verification, not signing. No broad hardened-runtime
exceptions or App Sandbox entitlement are added.

Notarization uploads a temporary ZIP to Apple, waits for acceptance, staples the
ticket to the app, validates it and checks Gatekeeper before producing the final
ZIP. Keep `build.json`, runtime reports, `signing.json`, `notarization.json` and
`distribution.json` beside the app as release evidence. The final report includes
the downloadable archive's SHA256. Do not edit the signed bundle afterward.

If interrupted after upload, the submission ID is retained in
`notarization-submission.json`. Repeat the notarize command with
`--submission-id ID` to resume waiting and stapling without another upload.

For an Apple rejection, inspect `notarization.json` and the downloaded
`notarization-log.json`. A successful signing probe does not establish
notarization or clean-machine acceptance.

## Keychain troubleshooting

The 18 October setup found a valid certificate and an existing `codesign` ACL,
but signing failed with `errSecInternalComponent`; security logs showed
`errSecInteractionNotAllowed`. Updating the partition access rule for the one
matching private key resolved signing from the user's Terminal:

```sh
security set-key-partition-list -S 'apple:' -t private \
  -l 'EXACT PRIVATE KEY LABEL' ~/Library/Keychains/login.keychain-db
```

The trailing colon in `apple:` is required. Target the exact private-key label
shown in Keychain Access; it may differ from the certificate name. Enter any
Keychain password locally at the prompt. Do not put it in scripts or chat.

The assistant execution context continued reporting a locked Keychain after
interactive Terminal access succeeded. Use the local Terminal for signing and
notarization if that persists; do not change unrelated keys or weaken all-app
access. See [Apple's diagnosis](https://developer.apple.com/forums/thread/712005).

## Validation before publishing

Run the shared fixture/contract checks against the packaged helper, compare
analysis results with the accepted engine, and launch the relocated production
app in both architectures. `tools/launch_app_smoke.py --architecture arm64` and
`--architecture x86_64` select the slice; the latter uses Rosetta on Apple Silicon
and should be labelled accordingly in the evidence.

A final distribution check should download/extract the notarized ZIP and open it
with normal Gatekeeper handling on a clean machine. Keep the macOS 14 / physical
Intel validation status explicit. Store submission and automatic updates can be
handled separately from direct downloads.

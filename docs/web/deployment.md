# Website and Linux beta distribution

The public website is tracked in root `web/`. The live directory
`nuc:/var/www/lumencode` is a deployed copy, not a Git checkout. From the current
Mac, reach nuc through Kubuntu (`ssh kubuntu`, then `ssh nuc`).

The 18 October 2026 refresh keeps the existing static dark page and Linux install
flow, adds real Linux/macOS/Windows screenshots, and describes the native ports
as working betas with source build instructions. Signed macOS/Windows downloads
and store releases are not yet offered. No Cloudflare or nginx changes are
needed for this deployment.

## Flatpak refresh

The old live bundle and repository were from April 2026. The replacement uses
main commit `8f5241f9e905bd50dd7b81dcab9b58990f0eea8a` for application/engine sources,
with the manifest runtime changed from KDE 5.15/21.08 to 5.15/25.08. Application ID
`app.lumencode.LumenCode`, x86_64 architecture, `master` channel and signing key
fingerprint `7509299D8A459F46B6F66C91A34D2DA12519F468` are preserved.

- Flatpak commit: `4b94132ad6f278c1cfe444273f9828be25d795b9521eee88d0d799821dbb209b`.
- Standalone bundle SHA256: `dde51005fee618d3f8b575e2df00097a9e788b00fc0acd292a11ec4ccca27f7e`.
- The tracked `.flatpakref` and `.flatpakrepo` now use `https://lumencode.app`
  instead of the internal `http://nuc:33335` URL. The deployment script also
  defaults to the public HTTPS origin for future generated metadata.
- The update repository preserves old published objects/history. A fresh OSTree
  client imported the existing public key and pulled the app with commit and
  summary signature verification enabled.

Existing clients whose remote still points to the old internal host can repair
that URL without reinstalling:

```sh
flatpak remote-modify --user --url=https://lumencode.app/repo/ lumencode
flatpak update --user app.lumencode.LumenCode
```

Use `--system` instead of `--user` if it was installed system-wide, and substitute
the remote name shown by `flatpak remotes` if it was renamed.

## Validation and screenshot provenance

- The actual Flatpak CLI passed 104 shared cases / 638 assertions in the build
  sandbox and again against the end-user Platform runtime. A separate smoke launched its GUI against the Platform runtime for
  ten seconds with isolated settings; this is not a clean-machine installation
  test. The build-directory launch emitted a host font-directory warning.
- The Qt GUI audit used a fresh Release build with the dark KDE palette:
  131 files, 905 selections, zero timeouts and zero QML problems. The website's
  Linux image is its completed `handleKey` focused selection in `web_app`.
- The first screenshot-only build lacked `lumencode-cli`. It displayed parser
  helper errors despite the harness reporting success with zero selections.
  Those captures were discarded; `run_qt_gui_audit.py` now requires both the
  harness and CLI before launch.
- macOS image: fresh `run_gui_audit.py` capture `06-web-js.png`, using the shared
  engine at the merged revision. This is app content hosted by the audit window.
- Windows image: fresh `run_windows_gui_audit.ps1` capture `windows-dark.png`,
  from the previously validated native package at `8f67b7c` (same app sources as
  the merge). The default GUI suite passed.
- Chromium browser checks at widths 1440, 768, 390 and 320 verified all three
  images, local links, no script errors and no horizontal page overflow.
  Desktop and mobile renders were visually reviewed.

The user observed tests switching before loading appeared visually complete.
Some transitions are intentional stress cases; passing functional checks do not
establish visual settling or smooth frame presentation. Audit timing changes
are deferred. A later GUI audit improvement should verify stable displayed
selection and source content before every acceptance screenshot.

Raw build, signature, browser and GUI reports are retained under
`/home/user/Code/Coding/lumencode-validation-20261018-web` on Kubuntu.

## Deployment

Back up the current live directory outside the web root. Copy the prepared
repository objects first, then refs and signed summary, and copy the explicit
website files with `index.html` last. Ensure static files are web-readable (0644),
including assets copied from Windows over SSH. Preserve old assets and repository objects;
do not run a broad `rsync --delete`. Verify the public HTTPS page and downloads.

The legacy `tools/deploy-web.sh` also rewrites nginx and combines signing with
broad directory synchronization; it was not used for this focused file-only
refresh. A future cleanup can separate static deployment from package publishing.

For the next Flatpak build, from an ordinary checkout on Linux:

```sh
flatpak-builder --user --force-clean --state-dir=/path/to/staging/state \
  --repo=/path/to/staging/repo --gpg-sign=EXISTING_KEY_ID \
  /path/to/staging/build apps/qt/packaging/app.lumencode.LumenCode.yml
```

Keep state/build directories on the same filesystem. Use a separate staging
repository, preserve the live signing identity, validate before publishing, and
set the bundle's repository URL to `https://lumencode.app/repo/`.

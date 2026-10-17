# LumenCode for macOS

The native SwiftUI interface uses the monorepo's shared C++ engine through a
packaged JSON helper. Start with the [root build instructions](../../README.md)
and [engine guide](../../engine/README.md).

Open `apps/macos/lumencode-swift.xcodeproj` from the repository root, choose the
shared `lumencode-swift` scheme, and Run. Xcode builds and embeds the helper.
For SwiftPM, run these commands from the repository root:

```sh
python3 tools/build_engine.py
swift test --package-path apps/macos
swift run --package-path apps/macos lumencode-swift-cli /path/to/project
```

`Sources/LumencodeCore` provides models, workspace scheduling and helper transport;
`Sources/LumencodeSwiftApp` contains presentation; `Sources/LumencodeCLI` is the
validation harness. Parser implementations, fallbacks and grammars live in root
`engine/` and `third_party/`. The earlier native bridge is excluded from builds.
There is no copied engine or source synchronization step.

The executable's deployment target does not establish macOS 14 runtime support:
current bundled Homebrew dependencies require macOS 26/27. See the [beta follow-up
report](../../docs/macos/follow-up-2026-10-17.md), [development
plan](../../docs/macos/development-plan.md), and [consolidation
status](../../docs/migration/status.md) for current evidence and accepted limits.

![Screenshot](Screenshots/screenshot.png)

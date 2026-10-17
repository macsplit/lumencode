// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "lumencode-swift",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .library(
            name: "LumencodeCore",
            targets: ["LumencodeCore"]
        ),
        .executable(
            name: "lumencode-swift-cli",
            targets: ["LumencodeCLI"]
        ),
        .executable(
            name: "lumencode-swift",
            targets: ["LumencodeSwiftApp"]
        )
    ],
    targets: [
        .target(
            name: "LumencodeCore",
            path: "Sources/LumencodeCore"
        ),
        .executableTarget(
            name: "LumencodeCLI",
            dependencies: ["LumencodeCore"],
            path: "Sources/LumencodeCLI"
        ),
        .testTarget(name: "LumencodeCoreTests", dependencies: ["LumencodeCore"]),
        .executableTarget(
            name: "LumencodeSwiftApp",
            dependencies: ["LumencodeCore"],
            path: "Sources/LumencodeSwiftApp",
            resources: [
                .process("Assets.xcassets"),
                .process("Resources")
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)

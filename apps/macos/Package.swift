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
            name: "LumencodeNativeBridge",
            path: "Sources/LumencodeNativeBridge",
            exclude: [
                "Vendor/tree-sitter-runtime/lib/src/alloc.c",
                "Vendor/tree-sitter-runtime/lib/src/get_changed_ranges.c",
                "Vendor/tree-sitter-runtime/lib/src/language.c",
                "Vendor/tree-sitter-runtime/lib/src/lexer.c",
                "Vendor/tree-sitter-runtime/lib/src/node.c",
                "Vendor/tree-sitter-runtime/lib/src/parser.c",
                "Vendor/tree-sitter-runtime/lib/src/point.c",
                "Vendor/tree-sitter-runtime/lib/src/query.c",
                "Vendor/tree-sitter-runtime/lib/src/stack.c",
                "Vendor/tree-sitter-runtime/lib/src/subtree.c",
                "Vendor/tree-sitter-runtime/lib/src/tree.c",
                "Vendor/tree-sitter-runtime/lib/src/tree_cursor.c",
                "Vendor/tree-sitter-runtime/lib/src/wasm_store.c"
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("Vendor/tree-sitter-runtime/lib/include"),
                .headerSearchPath("Vendor/tree-sitter-runtime/lib/src"),
                .headerSearchPath("Vendor/tree-sitter-swift/include"),
                .headerSearchPath("Vendor/tree-sitter-swift/src"),
                .headerSearchPath("Vendor/tree-sitter-java/src"),
                .headerSearchPath("Vendor/tree-sitter-c-sharp/src"),
                .headerSearchPath("Vendor/tree-sitter-javascript/src"),
                .headerSearchPath("Vendor/tree-sitter-typescript/typescript/src"),
                .headerSearchPath("Vendor/tree-sitter-typescript/tsx/src"),
                .headerSearchPath("Vendor/tree-sitter-php/php/src"),
                .headerSearchPath("Vendor/tree-sitter-css/src"),
                .headerSearchPath("Vendor/tree-sitter-python/src"),
                .headerSearchPath("Vendor/tree-sitter-rust/src"),
                .headerSearchPath("Vendor/tree-sitter-html/src")
            ],
            cxxSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("Vendor/tree-sitter-runtime/lib/include"),
                .headerSearchPath("Vendor/tree-sitter-runtime/lib/src"),
                .headerSearchPath("Vendor/tree-sitter-swift/include"),
                .headerSearchPath("Vendor/tree-sitter-swift/src"),
                .headerSearchPath("Vendor/tree-sitter-java/src"),
                .headerSearchPath("Vendor/tree-sitter-c-sharp/src"),
                .headerSearchPath("Vendor/tree-sitter-javascript/src"),
                .headerSearchPath("Vendor/tree-sitter-typescript/typescript/src"),
                .headerSearchPath("Vendor/tree-sitter-typescript/tsx/src"),
                .headerSearchPath("Vendor/tree-sitter-php/php/src"),
                .headerSearchPath("Vendor/tree-sitter-css/src"),
                .headerSearchPath("Vendor/tree-sitter-python/src"),
                .headerSearchPath("Vendor/tree-sitter-rust/src"),
                .headerSearchPath("Vendor/tree-sitter-html/src")
            ]
        ),
        .target(
            name: "LumencodeCore",
            dependencies: ["LumencodeNativeBridge"],
            path: "Sources/LumencodeCore"
        ),
        .executableTarget(
            name: "LumencodeCLI",
            dependencies: ["LumencodeCore"],
            path: "Sources/LumencodeCLI"
        ),
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

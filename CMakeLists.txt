cmake_minimum_required(VERSION 3.16)
project(LumenCode VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(ECM 5.80 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Quick QuickControls2 Widgets)
find_package(KF5 REQUIRED COMPONENTS Kirigami2 CoreAddons)

configure_file(lumencode.desktop.in lumencode.desktop @ONLY)

add_library(vendor-tree-sitter STATIC
    third_party/tree-sitter/lib/src/lib.c
)
target_include_directories(vendor-tree-sitter
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter/lib/src
)

add_library(vendor-tree-sitter-javascript STATIC
    third_party/tree-sitter-javascript/src/parser.c
    third_party/tree-sitter-javascript/src/scanner.c
)
target_include_directories(vendor-tree-sitter-javascript
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-javascript/src
)

add_library(vendor-tree-sitter-typescript STATIC
    third_party/tree-sitter-typescript/typescript/src/parser.c
    third_party/tree-sitter-typescript/typescript/src/scanner.c
)
target_include_directories(vendor-tree-sitter-typescript
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-typescript/typescript/src
)

add_library(vendor-tree-sitter-tsx STATIC
    third_party/tree-sitter-typescript/tsx/src/parser.c
    third_party/tree-sitter-typescript/tsx/src/scanner.c
)
target_include_directories(vendor-tree-sitter-tsx
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-typescript/tsx/src
)

add_library(vendor-tree-sitter-php STATIC
    third_party/tree-sitter-php/php/src/parser.c
    third_party/tree-sitter-php/php/src/scanner.c
)
target_include_directories(vendor-tree-sitter-php
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-php/php/src
)

add_library(vendor-tree-sitter-css STATIC
    third_party/tree-sitter-css/src/parser.c
    third_party/tree-sitter-css/src/scanner.c
)
target_include_directories(vendor-tree-sitter-css
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-css/src
)

add_library(vendor-tree-sitter-rust STATIC
    third_party/tree-sitter-rust/src/parser.c
    third_party/tree-sitter-rust/src/scanner.c
)
target_include_directories(vendor-tree-sitter-rust
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-rust/src
)

add_library(vendor-tree-sitter-python STATIC
    third_party/tree-sitter-python/src/parser.c
    third_party/tree-sitter-python/src/scanner.c
)
target_include_directories(vendor-tree-sitter-python
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-python/src
)

add_library(vendor-tree-sitter-swift STATIC
    third_party/tree-sitter-swift/src/parser.c
    third_party/tree-sitter-swift/src/scanner.c
)
target_include_directories(vendor-tree-sitter-swift
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-swift/src
        third_party/tree-sitter-swift/include
)

add_library(vendor-tree-sitter-java STATIC
    third_party/tree-sitter-java/src/parser.c
)
target_include_directories(vendor-tree-sitter-java
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-java/src
)

add_library(vendor-tree-sitter-c-sharp STATIC
    third_party/tree-sitter-c-sharp/src/parser.c
    third_party/tree-sitter-c-sharp/src/scanner.c
)
target_include_directories(vendor-tree-sitter-c-sharp
    PUBLIC
        third_party/tree-sitter/lib/include
    PRIVATE
        third_party/tree-sitter-c-sharp/src
)

add_executable(lumencode
    src/main.cpp
    src/filesystemmodel.cpp
    src/filesystemmodel.h
    src/projectcontroller.cpp
    src/projectcontroller.h
    src/symbolparser.cpp
    src/symbolparser.h
    src/resources.qrc
)

target_link_libraries(lumencode
    Qt5::Core
    Qt5::Quick
    Qt5::QuickControls2
    Qt5::Widgets
    KF5::Kirigami2
    KF5::CoreAddons
    vendor-tree-sitter
    vendor-tree-sitter-javascript
    vendor-tree-sitter-typescript
    vendor-tree-sitter-tsx
    vendor-tree-sitter-php
    vendor-tree-sitter-css
    vendor-tree-sitter-rust
    vendor-tree-sitter-python
    vendor-tree-sitter-swift
    vendor-tree-sitter-java
    vendor-tree-sitter-c-sharp
)

add_executable(lumencode-cli
    src/lumencode-cli.cpp
    src/filesystemmodel.cpp
    src/filesystemmodel.h
    src/projectcontroller.cpp
    src/projectcontroller.h
    src/symbolparser.cpp
    src/symbolparser.h
)

target_link_libraries(lumencode-cli
    Qt5::Core
    Qt5::Widgets
    KF5::CoreAddons
    vendor-tree-sitter
    vendor-tree-sitter-javascript
    vendor-tree-sitter-typescript
    vendor-tree-sitter-tsx
    vendor-tree-sitter-php
    vendor-tree-sitter-css
    vendor-tree-sitter-rust
    vendor-tree-sitter-python
    vendor-tree-sitter-swift
    vendor-tree-sitter-java
    vendor-tree-sitter-c-sharp
)

install(TARGETS lumencode lumencode-cli ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lumencode.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES src/icons/lumencode.svg DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps RENAME lumencode.svg)

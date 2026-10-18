if(LUMENCODE_BUILD_WEB)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
    set(LUMENCODE_ENGINE_QT Qt6::Core)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Concurrent)
    set(LUMENCODE_ENGINE_QT Qt5::Core Qt5::Concurrent)
endif()

add_library(vendor-tree-sitter STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/src/lib.c
)
target_include_directories(vendor-tree-sitter
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/src
)

add_library(vendor-tree-sitter-javascript STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-javascript/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-javascript/src/scanner.c
)
target_include_directories(vendor-tree-sitter-javascript
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-javascript/src
)

add_library(vendor-tree-sitter-typescript STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/typescript/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/typescript/src/scanner.c
)
target_include_directories(vendor-tree-sitter-typescript
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/typescript/src
)

add_library(vendor-tree-sitter-tsx STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/tsx/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/tsx/src/scanner.c
)
target_include_directories(vendor-tree-sitter-tsx
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-typescript/tsx/src
)

add_library(vendor-tree-sitter-php STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-php/php/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-php/php/src/scanner.c
)
target_include_directories(vendor-tree-sitter-php
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-php/php/src
)

add_library(vendor-tree-sitter-css STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-css/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-css/src/scanner.c
)
target_include_directories(vendor-tree-sitter-css
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-css/src
)

add_library(vendor-tree-sitter-rust STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-rust/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-rust/src/scanner.c
)
target_include_directories(vendor-tree-sitter-rust
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-rust/src
)

add_library(vendor-tree-sitter-python STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-python/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-python/src/scanner.c
)
target_include_directories(vendor-tree-sitter-python
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-python/src
)

add_library(vendor-tree-sitter-swift STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-swift/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-swift/src/scanner.c
)
target_include_directories(vendor-tree-sitter-swift
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-swift/src
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-swift/include
)

add_library(vendor-tree-sitter-java STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-java/src/parser.c
)
target_include_directories(vendor-tree-sitter-java
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-java/src
)

add_library(vendor-tree-sitter-c-sharp STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-c-sharp/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-c-sharp/src/scanner.c
)
target_include_directories(vendor-tree-sitter-c-sharp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-c-sharp/src
)

add_library(vendor-tree-sitter-html STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-html/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-html/src/scanner.c
)
target_include_directories(vendor-tree-sitter-html
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-html/src
)

add_library(vendor-tree-sitter-go STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-go/src/parser.c
)
target_include_directories(vendor-tree-sitter-go
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-go/src
)

add_library(vendor-tree-sitter-cpp STATIC
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-cpp/src/parser.c
    ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-cpp/src/scanner.c
)
target_include_directories(vendor-tree-sitter-cpp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/tree-sitter-cpp/src
)

add_library(lumencode-analysis STATIC
    src/symbolparser.cpp src/symbolparser.h
    src/projectindex.cpp src/projectindex.h
    src/weblinks.cpp src/weblinks.h
    src/httpclients.cpp src/httpclients.h
    src/agentqueries.cpp src/agentqueries.h
)
add_library(LumenCode::Analysis ALIAS lumencode-analysis)
target_include_directories(lumencode-analysis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lumencode-analysis PUBLIC ${LUMENCODE_ENGINE_QT} PRIVATE
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
    vendor-tree-sitter-html
    vendor-tree-sitter-go
    vendor-tree-sitter-cpp
)
# Absolute include prefix also works when the checkout is reached through /tmp.
set_property(TARGET lumencode-analysis PROPERTY AUTOMOC_MOC_OPTIONS "-p;${CMAKE_CURRENT_SOURCE_DIR}/src")
if(LUMENCODE_BUILD_ENGINE_HELPER)
    add_executable(lumencode-engine helper/main.cpp)
    target_link_libraries(lumencode-engine PRIVATE LumenCode::Analysis)
    install(TARGETS lumencode-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

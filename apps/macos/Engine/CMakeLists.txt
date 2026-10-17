cmake_minimum_required(VERSION 3.20)
project(LumencodeEngine LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version")
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Concurrent)

set(VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/LumencodeNativeBridge/Vendor")
add_library(tree-sitter STATIC "${VENDOR}/tree-sitter-runtime/lib/src/lib.c")
target_include_directories(tree-sitter PUBLIC "${VENDOR}/tree-sitter-runtime/lib/include" PRIVATE "${VENDOR}/tree-sitter-runtime/lib/src")
set(GRAMMARS)
foreach(NAME swift javascript typescript tsx php css rust python java c-sharp html go cpp)
    set(ROOT "${VENDOR}/tree-sitter-${NAME}")
    if(NAME STREQUAL "go" OR NAME STREQUAL "cpp")
        set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Upstream/third_party/tree-sitter-${NAME}")
    elseif(NAME STREQUAL "typescript")
        set(ROOT "${VENDOR}/tree-sitter-typescript/typescript")
    elseif(NAME STREQUAL "tsx")
        set(ROOT "${VENDOR}/tree-sitter-typescript/tsx")
    elseif(NAME STREQUAL "php")
        set(ROOT "${VENDOR}/tree-sitter-php/php")
    endif()
    set(INPUTS "${ROOT}/src/parser.c")
    if(EXISTS "${ROOT}/src/scanner.c")
        list(APPEND INPUTS "${ROOT}/src/scanner.c")
    endif()
    add_library(grammar-${NAME} STATIC ${INPUTS})
    target_include_directories(grammar-${NAME} PRIVATE "${ROOT}/src" "${VENDOR}/tree-sitter-runtime/lib/include")
    list(APPEND GRAMMARS grammar-${NAME})
endforeach()

add_executable(lumencode-engine main.cpp
    Upstream/src/symbolparser.cpp Upstream/src/symbolparser.h
    Upstream/src/projectindex.cpp Upstream/src/projectindex.h
    Upstream/src/weblinks.cpp Upstream/src/weblinks.h
    Upstream/src/httpclients.cpp Upstream/src/httpclients.h
    Upstream/src/agentqueries.cpp Upstream/src/agentqueries.h)
target_include_directories(lumencode-engine PRIVATE Upstream/src)
# Absolute includes avoid Qt 5 moc's relative-path issue through macOS /tmp symlinks.
set_property(TARGET lumencode-engine PROPERTY AUTOMOC_MOC_OPTIONS "-p;${CMAKE_CURRENT_SOURCE_DIR}/Upstream/src")
target_link_libraries(lumencode-engine PRIVATE Qt5::Core Qt5::Concurrent tree-sitter ${GRAMMARS})
set_target_properties(lumencode-engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

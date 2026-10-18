# A callable, headless module: no Qt GUI, native helper or server dependency.
add_executable(lumencode-web-engine engine.cpp)
target_link_libraries(lumencode-web-engine PRIVATE LumenCode::Analysis)
set_target_properties(lumencode-web-engine PROPERTIES
    AUTOMOC OFF AUTORCC OFF
    OUTPUT_NAME "engine" SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")
target_link_options(lumencode-web-engine PRIVATE
    --bind --no-entry
    -sMODULARIZE=1 -sEXPORT_NAME=createLumenCodeEngine
    -sENVIRONMENT=worker,node -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=67108864 -sMAXIMUM_MEMORY=2147483648
    "-sEXPORTED_RUNTIME_METHODS=['FS']"
    -lworkerfs.js)
foreach(asset index.html app.js worker.js style.css sw.js manifest.webmanifest icon.svg)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${asset}"
        "${CMAKE_BINARY_DIR}/web/${asset}" COPYONLY)
endforeach()

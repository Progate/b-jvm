cmake_minimum_required(VERSION 3.15...3.31)
project(bjvm VERSION 0.1
        DESCRIPTION "Portable JVM implementation"
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)

list(APPEND CMAKE_MODULE_PATH ${bjvm_SOURCE_DIR}/cmake)

include(CTest)
include(AsanSupport)
include(LtoSupport)
include(PostprocessWasm)
include(WasiSupport)

if (CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(BJVM_WASI TRUE)
    configure_wasi_build()
endif ()

if(NOT EMSCRIPTEN)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

add_subdirectory(vendor)
add_subdirectory(vm)
add_subdirectory(natives)

# gen_natives is a build-time tool, so there is no point cross-compiling it to wasm.
if (NOT BJVM_WASI)
    add_subdirectory(codegen)
endif ()

if (BUILD_TESTING)
    # Fetches doctest, so keep it out of configure when the tests are not being built.
    include(Doctest)
    add_subdirectory(test)
endif ()

add_library(bjvm_static STATIC)
# This keeps names fixed for the post-processor
target_link_libraries(bjvm_static PUBLIC vm PRIVATE natives)

if (NOT EMSCRIPTEN AND NOT BJVM_WASI)
    add_library(bjvm SHARED)
    target_link_libraries(bjvm PUBLIC vm PRIVATE natives)
endif()

if (BJVM_WASI)
    add_executable(bjvm_wasi vm/bin/main.c vm/wasi_pthread_stub.c)
    target_link_libraries(bjvm_wasi PRIVATE bjvm_static)
    configure_wasi_reactor(bjvm_wasi)
endif ()

if (EMSCRIPTEN)
    target_link_options(bjvm_static PUBLIC -sEXPORTED_FUNCTIONS=_malloc,_free,_main
            -sSTACK_SIZE=5000000
            -sWASM_BIGINT
            -sFORCE_FILESYSTEM=1
            -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1
            -sWARN_ON_UNDEFINED_SYMBOLS=0
            -sEXPORTED_RUNTIME_METHODS=wasmMemory,addFunction,removeFunction,wasmTable,FS,UTF8ToString,getValue,setValue
            )

    add_executable(bjvm_main vm/bin/main.c)  # for now
    run_emscripten_postprocess(bjvm_main)

    target_link_libraries(bjvm_main PRIVATE bjvm_static)
    target_link_options(bjvm_main PUBLIC -sWASMFS -sENVIRONMENT=web,worker -sMODULARIZE -sEXPORT_ES6=1 --emit-tsd bjvm_main.d.ts)
endif ()
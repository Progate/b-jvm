# Build settings for the wasm32-unknown-linux-musl target (BrowserOS's musl libc).
#
# This is a third target alongside native and Emscripten. Compared to Emscripten it drops the JS glue
# entirely: the module is a *reactor* (no _start; the host calls _initialize and then the exported
# ffi_* functions), and every syscall it makes goes through browser_os_linux.syscall, which the host
# implements. argv/env still come over wasi_snapshot_preview1 (crt_start.h reads them there), so the
# module keeps those four imports. See vm/types.h for the BJVM_WASM macro that separates
# "targeting wasm" from "running under the Emscripten runtime".

function(configure_linux_musl_build)
    add_compile_options(
            # classfile.c parses with setjmp/longjmp, which on wasm is lowered onto the exception
            # handling proposal. Both flags are required; musl's setjmp.h declares the wasm SjLj
            # helpers (__wasm_setjmp / __wasm_longjmp) that libc.a provides when compiled with them.
            -fwasm-exceptions
            -mllvm -wasm-enable-sjlj
            # interpreter2.c uses __builtin_wasm_trunc_saturate_*, which requires the nontrapping-fptoint
            # target feature (on by default in engines; clang leaves it off for older runtimes).
            -mnontrapping-fptoint
    )
endfunction()

# Link TARGET as a static reactor, exporting the ffi_* surface for the host.
#
# BJVM_LINUX_MUSL_CRT_OBJECTS must name the reactor crt objects (musl's crt1-reactor.c plus
# init_libc_export.c, compiled by scripts/build-wasm.mjs): the sysroot only ships the
# _start/main entry objects, and _initialize is what runs __browseros_init_libc and the
# constructors here.
function(configure_linux_musl_reactor TARGET)
    target_sources(${TARGET} PRIVATE ${BJVM_LINUX_MUSL_CRT_OBJECTS})
    target_link_options(${TARGET} PRIVATE
            # Static link; no crt1.o/Scrt1.o (those end in _start). The reactor crt above
            # provides _initialize, which is the entry.
            -static
            -nostartfiles
            -Wl,--entry=_initialize
            # The fork's clang defaults to libunwind for wasm exceptions, but this sysroot has
            # none (setjmp/longjmp lower to the SjLj intrinsics in libc.a instead).
            --unwindlib=none
            # The ffi_* entry points are marked EMSCRIPTEN_KEEPALIVE, which on this target expands to
            # used + default visibility. --export-dynamic is what turns that into actual wasm exports.
            -Wl,--export-dynamic
            # The host allocates the argument buffers it hands to ffi_*, so it needs the VM's allocator.
            -Wl,--export=malloc
            -Wl,--export=free
            # jit.c: the host appends compiled functions to the function table (and the compiled code calls the
            # runtime helpers through it)
            -Wl,--export-table
            -Wl,--growable-table
            # The interpreter recurses deeply; the 64KB default overflows immediately.
            -Wl,-z,stack-size=8388608
    )
    set_target_properties(${TARGET} PROPERTIES SUFFIX ".wasm")
endfunction()

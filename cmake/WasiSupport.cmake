# Build settings for the wasm32-wasip1 target (wasi-sdk).
#
# This is a third target alongside native and Emscripten. Compared to Emscripten it drops the JS glue
# entirely: the module is a WASI *reactor* (no _start; the host calls _initialize and then the exported
# ffi_* functions), and every syscall it makes goes through wasi_snapshot_preview1, which the host
# implements. See vm/types.h for the BJVM_WASM macro that separates "targeting wasm" from
# "running under the Emscripten runtime".

function(configure_wasi_build)
    add_compile_options(
            # classfile.c parses with setjmp/longjmp, which on wasm is lowered onto the exception
            # handling proposal. Both flags are required; wasi-libc's setjmp.h #errors without them.
            -fwasm-exceptions
            -mllvm -wasm-enable-sjlj
            # bjvm.c calls munmap when freeing Unsafe allocations. wasi-libc's emulation is malloc-backed,
            # which is all we need since nothing depends on the mapping being lazy.
            -D_WASI_EMULATED_MMAN
    )
    add_link_options(
            -fwasm-exceptions
            -lwasi-emulated-mman
            # Provides __wasm_setjmp / __wasm_longjmp, the runtime half of the SjLj lowering above.
            -lsetjmp
    )
endfunction()

# Link TARGET as a WASI reactor, exporting the ffi_* surface for the host.
function(configure_wasi_reactor TARGET)
    target_link_options(${TARGET} PRIVATE
            -mexec-model=reactor
            # The ffi_* entry points are marked EMSCRIPTEN_KEEPALIVE, which on this target expands to
            # used + default visibility. --export-dynamic is what turns that into actual wasm exports.
            -Wl,--export-dynamic
            # The host allocates the argument buffers it hands to ffi_*, so it needs the VM's allocator.
            -Wl,--export=malloc
            -Wl,--export=free
            # The interpreter recurses deeply; the wasi-sdk default of 64KB overflows immediately.
            -Wl,-z,stack-size=8388608
    )
    set_target_properties(${TARGET} PROPERTIES SUFFIX ".wasm")
endfunction()

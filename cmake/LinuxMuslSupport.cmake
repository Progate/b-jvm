# Build settings for the wasm32-unknown-linux-musl target (BrowserOS's musl libc).
#
# This is a third target alongside native and Emscripten. Compared to Emscripten it drops the JS glue
# entirely, and every syscall it makes goes through browser_os_linux.syscall, which the host
# implements. The output is a *dynamic* executable (`-pie` with `NEEDED libc.so`): the BrowserOS
# loader (browser-dyld) creates the linear memory, the function table and the stack, resolves
# libc.so, applies relocations, and the host calls `_start` once to run libc setup and all module
# constructors. After that the host drives the exported ffi_* functions (see vm/bin/main.c — main
# itself is only a marker that initialization finished). See vm/types.h for the BJVM_WASM macro
# that separates "targeting wasm" from "running under the Emscripten runtime".

function(configure_linux_musl_build)
    add_compile_options(
            # classfile.c parses with setjmp/longjmp, which on wasm is lowered onto the exception
            # handling proposal. Both flags are required; musl's setjmp.h declares the wasm SjLj
            # helpers (__wasm_setjmp / __wasm_longjmp) that libc.so provides when compiled with them.
            -fwasm-exceptions
            -mllvm -wasm-enable-sjlj
            # interpreter2.c uses __builtin_wasm_trunc_saturate_*, which requires the nontrapping-fptoint
            # target feature (on by default in engines; clang leaves it off for older runtimes).
            -mnontrapping-fptoint
    )
endfunction()

# Link TARGET as a dynamic `-pie` executable for the BrowserOS loader.
#
# The plain driver link is what produces the wanted shape: `wasm32-unknown-linux-musl` defaults to
# `-pie` with `Scrt1.o` and `NEEDED libc.so`, so there is no linker flag to write for any of it.
# What is left here are only the deviations from that default.
function(configure_linux_musl_pie TARGET)
    target_link_options(${TARGET} PRIVATE
            # The fork's clang defaults to libunwind for wasm exceptions, but this sysroot has
            # none (setjmp/longjmp lower to the SjLj intrinsics in libc.so instead).
            --unwindlib=none
            # The ffi_* entry points are marked EMSCRIPTEN_KEEPALIVE, which on this target expands to
            # used + default visibility. -rdynamic is what turns that into actual wasm exports, and
            # also leaves the rest of b-jvm visible so the JIT's compiled code and the loader can
            # resolve runtime helpers by name.
            -rdynamic
            # Objects with FP ops carry an undefined reference to the __fe_status wasm global;
            # import-dynamic turns it into an env.__fe_status import, which the loader resolves to the
            # same shared global libc.so imports (same convention as the other -pie packages).
            -Wl,--unresolved-symbols=import-dynamic
    )
    set_target_properties(${TARGET} PROPERTIES SUFFIX ".wasm")
endfunction()

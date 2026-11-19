# Build settings for the wasm32-unknown-linux-musl target (BrowserOS's musl libc).
#
# This is a third target alongside native and Emscripten. Compared to Emscripten it drops the JS glue
# entirely, and every syscall it makes goes through browser_os_linux.syscall, the BrowserOS kernel's
# Linux syscall entry. The output is a *dynamic* executable (`-pie` with `NEEDED libc.so`) that the
# kernel starts with execve(2), like any Linux program: the loader (browser-dyld) creates the linear
# memory, the function table and the stack, resolves libc.so and libz.so, and `_start` runs `main`
# (vm/bin/main.c), which runs the VM to the end. See vm/types.h for the BJVM_WASM macro that
# separates "targeting wasm" from "running under the Emscripten runtime".

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
    # Undefined names stay link errors, as on Linux. The two names the loader provides to every
    # module, the exception tag __c_longjmp (setjmp/longjmp) and the FP exception flag global
    # __fe_status, need nothing here: wasm-ld imports them by name in PIC links.
    target_link_options(${TARGET} PRIVATE
            # Export b-jvm's symbols, as a Linux program linked with -rdynamic does, so that a debugger,
            # a profiler and dladdr(3) see them by name.
            -rdynamic
    )
    set_target_properties(${TARGET} PROPERTIES SUFFIX ".wasm")
endfunction()

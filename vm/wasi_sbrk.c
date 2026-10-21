// sbrk for the wasm32-wasi build, replacing wasi-libc's.
//
// wasi-libc's malloc (dlmalloc) asks sbrk for memory in small steps, and wasi-libc's sbrk turns every one of them
// into a memory.grow. Growing a wasm memory is not cheap for the host (the old ArrayBuffer is detached and every
// instance sharing the memory is updated), and loading the JDK's and kotlinc's classes grows it tens of thousands of
// times: about a third of kotlinc's run time went to sbrk. This one grows the memory in large chunks and hands out
// the rest on later calls. Like wasi-libc's, the break only moves forward and is contiguous, which is what dlmalloc
// relies on.

#ifdef __wasi__

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE 65536
// Grow the memory by at least this much at a time
#define GROW_CHUNK (64u << 20)

void *sbrk(intptr_t increment) {
  static uintptr_t brk, end; // the current break and the end of the memory
  if (!brk)
    brk = end = __builtin_wasm_memory_size(0) * PAGE_SIZE;
  if (increment == 0)
    return (void *)brk;
  if (increment < 0) {
    // wasi-libc's sbrk cannot shrink the memory either
    errno = EINVAL;
    return (void *)-1;
  }
  uintptr_t old = brk;
  if ((uintptr_t)increment > end - brk) {
    uintptr_t needed = ((uintptr_t)increment - (end - brk) + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t chunk = needed < GROW_CHUNK / PAGE_SIZE ? GROW_CHUNK / PAGE_SIZE : needed;
    if (__builtin_wasm_memory_grow(0, chunk) == SIZE_MAX) {
      // Near the 4GB limit: grow by exactly what is needed
      chunk = needed;
      if (__builtin_wasm_memory_grow(0, chunk) == SIZE_MAX) {
        errno = ENOMEM;
        return (void *)-1;
      }
    }
    end += chunk * PAGE_SIZE;
  }
  brk += increment;
  return (void *)old;
}

#endif

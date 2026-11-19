// Loads the modules the JIT compiles (jit.c) with dlopen(3).
//
// A module is a wasm shared object: a side module with a dylink.0 section that imports the process's memory and
// function table and exports one function per compiled method as `f0`, `f1`, ... It is written to a temporary file and loaded with dlopen,
// the way a JIT on Linux that cannot write executable memory loads the code it generates. dlsym gives the function
// pointer, which on wasm is an index into the function table. The file is removed once it is loaded: the loader keeps
// the module, not the file.

#include "jit.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__wasm__) && !defined(EMSCRIPTEN)
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

static struct {
  u32 modules, loaded, failed;
} jit_stats;

bool jit_load(const u8 *bytes, u32 len, int count, s32 *indices) {
  const char *tmp = getenv("TMPDIR");
  char path[4096];
  snprintf(path, sizeof(path), "%s/bjvm-jit-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  int fd = mkstemp(path);
  bool written = fd >= 0;
  for (u32 done = 0; written && done < len;) {
    ssize_t n = write(fd, bytes + done, len - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      written = false;
    else
      done += (u32)n;
  }
  if (fd >= 0)
    close(fd);
  void *handle = written ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;
  if (fd >= 0)
    unlink(path);
  if (!handle) {
    // Java still runs correctly (the methods stay interpreted), so say why once when asked to
    if (jit_stats.failed == 0 && getenv("BJVM_JIT_STATS"))
      fprintf(stderr, "bjvm: jit: cannot load compiled methods: %s\n",
              written ? dlerror() : "cannot write them to a temporary file");
    jit_stats.failed += count;
    return false;
  }
  jit_stats.modules++;
  for (int i = 0; i < count; i++) {
    char name[16];
    snprintf(name, sizeof(name), "f%d", i);
    void *function = dlsym(handle, name);
    indices[i] = function ? (s32)(uintptr_t)function : -1;
    if (function)
      jit_stats.loaded++;
    else
      jit_stats.failed++;
  }
  return true;
}

void jit_report_stats(void) {
  if (getenv("BJVM_JIT_STATS"))
    fprintf(stderr, "bjvm: jit: %u loaded, %u failed, %u modules\n", jit_stats.loaded, jit_stats.failed, jit_stats.modules);
}
#else
bool jit_load(const u8 *bytes, u32 len, int count, s32 *indices) {
  (void)bytes;
  (void)len;
  (void)count;
  (void)indices;
  return false;
}

void jit_report_stats(void) {}
#endif

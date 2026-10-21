// Minimal pthread surface for the WASI reactor build.
//
// profiler.c references pthread_create/pthread_join, but recent wasi-sdk
// sysroots no longer ship libwasi-emulated-pthread (only the header
// declarations behind _WASI_EMULATED_PTHREAD). The profiler is never enabled
// under WASI, so create fails at runtime and nothing else is needed.
#include <errno.h>
#include <pthread.h>

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
  (void)thread;
  (void)attr;
  (void)start_routine;
  (void)arg;
  return EAGAIN;
}

int pthread_join(pthread_t thread, void **retval) {
  (void)thread;
  (void)retval;
  return ESRCH;
}

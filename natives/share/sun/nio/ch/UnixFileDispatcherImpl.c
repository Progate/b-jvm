#include "bjvm_net.h"

// Vectorized IO, locking and direct-buffer hooks missing from
// sun/nio/fs/UnixNativeDispatcher.c. Locking has no WASI surface and
// pretends to succeed; setDirect0 always reports "no O_DIRECT".

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, readv0, "(Ljava/io/FileDescriptor;JI)J") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  s32 count = args[2].i;
  if (count <= 0 || count > 64)
    return (stack_value){.l = -1};
  __wasi_iovec_t iovs[64];
  const u8 *native_iovs = (const u8 *)(uintptr_t)(u64)args[1].l;
  for (s32 i = 0; i < count; ++i)
    iovs[i] = (__wasi_iovec_t){.buf = (u8 *)(uintptr_t) * (const u32 *)(native_iovs + i * 8),
                             .buf_len = *(const u32 *)(native_iovs + i * 8 + 4)};
  __wasi_size_t nread = 0;
  __wasi_errno_t e = __wasi_fd_read(fd, iovs, (__wasi_size_t)count, &nread);
  if (e) {
    bjvm_throw_io(thread, e, "readv");
    return value_null();
  }
  return (stack_value){.l = nread ? (s64)nread : -1};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, writev0, "(Ljava/io/FileDescriptor;JI)J") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  s32 count = args[2].i;
  if (count <= 0 || count > 64)
    return (stack_value){.l = -1};
  __wasi_ciovec_t iovs[64];
  const u8 *native_iovs = (const u8 *)(uintptr_t)(u64)args[1].l;
  for (s32 i = 0; i < count; ++i)
    iovs[i] = (__wasi_ciovec_t){.buf = (const u8 *)(uintptr_t) * (const u32 *)(native_iovs + i * 8),
                              .buf_len = *(const u32 *)(native_iovs + i * 8 + 4)};
  __wasi_size_t nwritten = 0;
  __wasi_errno_t e = __wasi_fd_write(fd, iovs, (__wasi_size_t)count, &nwritten);
  if (e) {
    bjvm_throw_io(thread, e, "writev");
    return value_null();
  }
  return (stack_value){.l = (s64)nwritten};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, lock0, "(Ljava/io/FileDescriptor;ZJJZ)I") {
  return (stack_value){.i = 0}; // no file locking; pretend it succeeded
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, release0, "(Ljava/io/FileDescriptor;JJ)V") {
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, closeIntFD, "(I)V") {
  s32 fd = args[0].i;
  if (bjvm_is_fake_fd(fd)) {
    bjvm_fakefd_free(fd);
    return value_null();
  }
  if (fd >= 0)
    (void)__wasi_fd_close(fd);
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, setDirect0, "(Ljava/io/FileDescriptor;)I") {
  return (stack_value){.i = -1};
}

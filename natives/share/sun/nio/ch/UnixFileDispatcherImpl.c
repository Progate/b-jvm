#include "bjvm_net.h"

// Vectorized IO, locking and direct-buffer hooks missing from
// sun/nio/fs/UnixNativeDispatcher.c. Locking has no WASI surface and
// pretends to succeed; setDirect0 always reports "no O_DIRECT".

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, readv0, "(Ljava/io/FileDescriptor;JI)J") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  s32 count = args[2].i;
  if (count <= 0 || count > 64)
    return (stack_value){.l = -1};
  struct iovec iovs[64];
  bjvm_decode_iovs(iovs, (u64)args[1].l, (u32)count);
  ssize_t nread = readv(fd, iovs, count);
  if (nread < 0) {
    bjvm_throw_io(thread, errno, "readv");
    return value_null();
  }
  return (stack_value){.l = nread ? (s64)nread : -1};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, writev0, "(Ljava/io/FileDescriptor;JI)J") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  s32 count = args[2].i;
  if (count <= 0 || count > 64)
    return (stack_value){.l = -1};
  struct iovec iovs[64];
  bjvm_decode_iovs(iovs, (u64)args[1].l, (u32)count);
  ssize_t nwritten = writev(fd, iovs, count);
  if (nwritten < 0) {
    bjvm_throw_io(thread, errno, "writev");
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
    (void)close(fd);
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, setDirect0, "(Ljava/io/FileDescriptor;)I") {
  return (stack_value){.i = -1};
}

#include "bjvm_net.h"

// Vectorized IO, locking and direct-buffer hooks missing from
// sun/nio/fs/UnixNativeDispatcher.c. Locking is fcntl(2) record locking, as in OpenJDK's
// UnixFileDispatcherImpl.c; setDirect0 always reports "no O_DIRECT".

#include <fcntl.h>

// sun.nio.ch.FileDispatcher's lock results
#define FILE_DISPATCHER_NO_LOCK (-1)
#define FILE_DISPATCHER_LOCKED 0
#define FILE_DISPATCHER_INTERRUPTED 2

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

// A lock to the end of the file is requested with size Long.MAX_VALUE, which fcntl spells as length 0
static struct flock bjvm_lock_range(short type, s64 pos, s64 size) {
  return (struct flock){
      .l_type = type, .l_whence = SEEK_SET, .l_start = (off_t)pos, .l_len = size == INT64_MAX ? 0 : (off_t)size};
}

// (fd: FileDescriptor, block: Boolean, pos: Long, size: Long, shared: Boolean) -> Int
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, lock0, "(Ljava/io/FileDescriptor;ZJJZ)I") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  int cmd = args[1].i ? F_SETLKW : F_SETLK;
  struct flock fl = bjvm_lock_range(args[4].i ? F_RDLCK : F_WRLCK, args[2].l, args[3].l);
  if (fcntl(fd, cmd, &fl) < 0) {
    if (cmd == F_SETLK && (errno == EAGAIN || errno == EACCES))
      return (stack_value){.i = FILE_DISPATCHER_NO_LOCK};
    if (errno == EINTR)
      return (stack_value){.i = FILE_DISPATCHER_INTERRUPTED};
    bjvm_throw_io(thread, errno, "Lock failed");
    return value_null();
  }
  return (stack_value){.i = FILE_DISPATCHER_LOCKED};
}

// (fd: FileDescriptor, pos: Long, size: Long) -> Unit
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, release0, "(Ljava/io/FileDescriptor;JJ)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  struct flock fl = bjvm_lock_range(F_UNLCK, args[1].l, args[2].l);
  if (fcntl(fd, F_SETLK, &fl) < 0)
    bjvm_throw_io(thread, errno, "Release failed");
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

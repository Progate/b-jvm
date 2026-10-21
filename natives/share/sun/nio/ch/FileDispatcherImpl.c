#include "bjvm_net.h"

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, transferTo0,
               "(Ljava/io/FileDescriptor;JJLjava/io/FileDescriptor;Z)J") {
  s32 from = bjvm_fd_val(args[0].handle->obj);
  s64 position = args[1].l, count = args[2].l;
  s32 to = bjvm_fd_val(args[3].handle->obj);
  if (count <= 0)
    return (stack_value){.l = 0};
  // Read-then-write fallback; chunk the copy so a sendfile cannot
  // allocate the whole file.
  u8 buf[16384];
  s64 done = 0;
  while (done < count) {
    u32 want = (u32)((count - done) > (s64)sizeof buf ? sizeof buf : (count - done));
    ssize_t nread = pread(from, buf, want, position + done);
    if (nread <= 0)
      break;
    ssize_t nwritten = write(to, buf, (size_t)nread);
    if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (nwritten < 0) {
      bjvm_throw_io(thread, errno, "transferTo");
      return value_null();
    }
    done += nwritten;
    if (nwritten < nread)
      break;
  }
  return (stack_value){.l = done};
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, transferFrom0,
               "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;JJZ)J") {
  s32 to = bjvm_fd_val(args[0].handle->obj);
  s32 from = bjvm_fd_val(args[1].handle->obj);
  s64 position = args[2].l, count = args[3].l;
  if (count <= 0)
    return (stack_value){.l = 0};
  u8 buf[16384];
  s64 done = 0;
  while (done < count) {
    u32 want = (u32)((count - done) > (s64)sizeof buf ? sizeof buf : (count - done));
    ssize_t nread = read(from, buf, want);
    if (nread <= 0)
      break;
    ssize_t nwritten = pwrite(to, buf, (size_t)nread, position + done);
    if (nwritten < 0) {
      bjvm_throw_io(thread, errno, "transferFrom");
      return value_null();
    }
    done += nwritten;
    if (nwritten < nread)
      break;
  }
  return (stack_value){.l = done};
}

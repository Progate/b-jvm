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
    __wasi_iovec_t iov = {.buf = buf, .buf_len = want};
    __wasi_size_t nread = 0;
    __wasi_errno_t e = __wasi_fd_pread(from, &iov, 1, (__wasi_filesize_t)(position + done), &nread);
    if (e || nread == 0)
      break;
    __wasi_ciovec_t wiov = {.buf = buf, .buf_len = nread};
    __wasi_size_t nwritten = 0;
    __wasi_errno_t we = __wasi_fd_write(to, &wiov, 1, &nwritten);
    if (we == __WASI_ERRNO_AGAIN)
      break;
    if (we) {
      bjvm_throw_io(thread, we, "transferTo");
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
    __wasi_iovec_t iov = {.buf = buf, .buf_len = want};
    __wasi_size_t nread = 0;
    __wasi_errno_t e = __wasi_fd_read(from, &iov, 1, &nread);
    if (e || nread == 0)
      break;
    __wasi_ciovec_t wiov = {.buf = buf, .buf_len = nread};
    __wasi_size_t nwritten = 0;
    __wasi_errno_t we = __wasi_fd_pwrite(to, &wiov, 1, (__wasi_filesize_t)(position + done), &nwritten);
    if (we) {
      bjvm_throw_io(thread, we, "transferFrom");
      return value_null();
    }
    done += nwritten;
    if (nwritten < nread)
      break;
  }
  return (stack_value){.l = done};
}

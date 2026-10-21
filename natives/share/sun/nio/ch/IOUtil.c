#include "bjvm_net.h"

DECLARE_NATIVE("sun/nio/ch", IOUtil, initIDs, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/ch", IOUtil, iovMax, "()I") { return (stack_value){.i = 1024}; }

DECLARE_NATIVE("sun/nio/ch", IOUtil, writevMax, "()J") { return (stack_value){.l = 1024}; }

DECLARE_NATIVE("sun/nio/ch", IOUtil, fdLimit, "()I") { return (stack_value){.i = 1024}; }

DECLARE_NATIVE("sun/nio/ch", IOUtil, fdVal, "(Ljava/io/FileDescriptor;)I") {
  return (stack_value){.i = bjvm_fd_val(args[0].handle->obj)};
}

DECLARE_NATIVE("sun/nio/ch", IOUtil, setfdVal, "(Ljava/io/FileDescriptor;I)V") {
  obj_header *fd = args[0].handle->obj;
  if (fd)
    StoreFieldInt(fd, "fd", args[1].i);
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", IOUtil, configureBlocking, "(Ljava/io/FileDescriptor;Z)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  bjvm_fd_set_nonblock(fd, args[1].i == 0);
  return value_null();
}

// Drain until empty. For fake eventfds this clears the pending signal;
// for real fds it reads until EAGAIN.
DECLARE_NATIVE("sun/nio/ch", IOUtil, drain, "(I)Z") {
  s32 fd = args[0].i;
  bjvm_fakefd *f = bjvm_fakefd_get(fd);
  if (f) {
    f->counter = 0;
    return (stack_value){.i = 1};
  }
  u8 buf[512];
  for (;;) {
    ssize_t nread = read(fd, buf, sizeof buf);
    if (nread <= 0)
      break;
  }
  return (stack_value){.i = 1};
}

DECLARE_NATIVE("sun/nio/ch", IOUtil, drain1, "(I)I") {
  s32 fd = args[0].i;
  bjvm_fakefd *f = bjvm_fakefd_get(fd);
  if (f) {
    int had = f->counter > 0;
    if (f->counter)
      f->counter--;
    return (stack_value){.i = had};
  }
  u8 b;
  ssize_t nread = read(fd, &b, 1);
  return (stack_value){.i = nread > 0 ? 1 : 0};
}

DECLARE_NATIVE("sun/nio/ch", IOUtil, write1, "(IB)I") {
  s32 fd = args[0].i;
  bjvm_fakefd *f = bjvm_fakefd_get(fd);
  if (f) {
    f->counter++;
    return (stack_value){.i = 1};
  }
  u8 b = (u8)args[1].i;
  ssize_t nwritten = write(fd, &b, 1);
  return (stack_value){.i = nwritten < 0 ? BJVM_IOS_THROWN : (s32)nwritten};
}

DECLARE_NATIVE("sun/nio/ch", IOUtil, randomBytes, "([B)Z") {
  obj_header *arr = args[0].handle->obj;
  u8 *data = ArrayData(arr);
  u32 len = ArrayLength(arr);
  u32 off = 0;
  while (off < len) {
    ssize_t n = getrandom(data + off, len - off, 0);
    if (n < 0)
      return (stack_value){.i = 0};
    off += (u32)n;
  }
  return (stack_value){.i = 1};
}

// No pipes in this runtime; callers fall back to sockets or eventfds.
DECLARE_NATIVE("sun/nio/ch", IOUtil, makePipe, "(Z)J") { return (stack_value){.l = -1}; }

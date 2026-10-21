#include "bjvm_net.h"

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, init, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, close0, "(Ljava/io/FileDescriptor;)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  if (bjvm_is_fake_fd(fd)) {
    bjvm_fakefd_free(fd);
    return value_null();
  }
  if (fd >= 0)
    (void)close(fd);
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", UnixDispatcher, preClose0, "(Ljava/io/FileDescriptor;)V") { return value_null(); }

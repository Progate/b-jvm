#include "bjvm_net.h"

/*
 * Datagram channel I/O for connected sockets. The fd behaves like a
 * stream fd once connected (fd_read/fd_write route to the remote), so
 * this shares the generic helpers with SocketDispatcher. Blocking fds
 * poll+retry via scheduler yields.
 */

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramDispatcher, read0, "(Ljava/io/FileDescriptor;JI)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    stack_value r = bjvm_do_read0(thread, fd, (u64)args[1].l, args[2].i);
    if (r.i == BJVM_IOS_UNAVAILABLE && !nonblock) {
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    ASYNC_RETURN(r);
  }
  ASYNC_END(value_null());
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramDispatcher, readv0, "(Ljava/io/FileDescriptor;JI)J",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    stack_value r = bjvm_do_readv0(thread, fd, (u64)args[1].l, args[2].i);
    if (r.l == BJVM_IOS_UNAVAILABLE && !nonblock) {
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    ASYNC_RETURN(r);
  }
  ASYNC_END(value_null());
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramDispatcher, write0, "(Ljava/io/FileDescriptor;JI)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    stack_value r = bjvm_do_write0(thread, fd, (u64)args[1].l, args[2].i);
    if (r.i == BJVM_IOS_UNAVAILABLE && !nonblock) {
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    ASYNC_RETURN(r);
  }
  ASYNC_END(value_null());
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramDispatcher, writev0, "(Ljava/io/FileDescriptor;JI)J",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    stack_value r = bjvm_do_writev0(thread, fd, (u64)args[1].l, args[2].i);
    if (r.l == BJVM_IOS_UNAVAILABLE && !nonblock) {
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    ASYNC_RETURN(r);
  }
  ASYNC_END(value_null());
}

DECLARE_NATIVE("sun/nio/ch", DatagramDispatcher, dup0, "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;)V") {
  // Duplicating a datagram fd has no socket-table equivalent; only used
  // on JDK code paths we never take (SelectSet emulation).
  return value_null();
}

#include "bjvm_net.h"

/*
 * Socket channel I/O. read0/readv0 return JDK IOStatus codes: positive
 * byte counts, -1 EOF, -2 UNAVAILABLE (nonblocking, no data). Blocking
 * fds poll+retry via scheduler yields so the VM stays responsive.
 *
 * The `long address` JDK passes points into Unsafe-allocated native
 * memory; for readv/writev it is a struct iovec[] where wasm32 entries
 * are {base:u32, len:u32} = 8 bytes each.
 */

// Generic blocked-or-retry loop for the four ops. `attempt` returns the
// finished stack_value, or {.i = -100} sentinel meaning "not ready yet"
// when the fd is blocking and got EAGAIN.
#define BJVM_TRY_AGAIN_SENTINEL (-100)

DECLARE_ASYNC_NATIVE("sun/nio/ch", SocketDispatcher, read0, "(Ljava/io/FileDescriptor;JI)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    // Locals are recomputed each iteration: after ASYNC_YIELD resumes,
    // control re-enters at the yield point and pre-loop locals are gone.
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

DECLARE_ASYNC_NATIVE("sun/nio/ch", SocketDispatcher, readv0, "(Ljava/io/FileDescriptor;JI)J",
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

DECLARE_ASYNC_NATIVE("sun/nio/ch", SocketDispatcher, write0, "(Ljava/io/FileDescriptor;JI)I",
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

DECLARE_ASYNC_NATIVE("sun/nio/ch", SocketDispatcher, writev0, "(Ljava/io/FileDescriptor;JI)J",
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

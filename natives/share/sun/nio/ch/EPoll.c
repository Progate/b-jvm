#include "bjvm_net.h"

/*
 * epoll emulation over poll_oneoff. epoll instances are fake fds holding
 * a table of (fd, interested events). wait() checks each registered fd:
 * fake eventfds via their pending counter, real fds via a nonblocking
 * poll_oneoff; when nothing is ready it yields to the scheduler until
 * the timeout expires.
 *
 * Event record layout written to pollAddress (matches the ints JDK's
 * EPoll.getDescriptor/getEvents read back): {events:u32 @0, fd:u32 @4}.
 */

#define BJVM_EPOLLIN 0x001
#define BJVM_EPOLLOUT 0x004
#define BJVM_EPOLLERR 0x008
#define BJVM_EPOLLHUP 0x010

DECLARE_NATIVE("sun/nio/ch", EPoll, eventSize, "()I") { return (stack_value){.i = 8}; }
DECLARE_NATIVE("sun/nio/ch", EPoll, eventsOffset, "()I") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/nio/ch", EPoll, dataOffset, "()I") { return (stack_value){.i = 4}; }

DECLARE_NATIVE("sun/nio/ch", EPoll, create, "()I") {
  return (stack_value){.i = bjvm_fakefd_alloc(BJVM_FAKE_EPOLL)};
}

DECLARE_NATIVE("sun/nio/ch", EPoll, ctl, "(IIII)I") {
  s32 epfd = args[0].i, opcode = args[1].i, fd = args[2].i, events = args[3].i;
  bjvm_fakefd *ep = bjvm_fakefd_get(epfd);
  if (!ep || ep->kind != BJVM_FAKE_EPOLL)
    return (stack_value){.i = -1};

  int idx = -1;
  for (u32 i = 0; i < arrlen(ep->entries); ++i) {
    if (ep->entries[i].fd == fd) {
      idx = (int)i;
      break;
    }
  }

  // EPOLL_CTL_ADD = 1, DEL = 2, MOD = 3
  if (opcode == 1) {
    if (idx >= 0)
      return (stack_value){.i = -1}; // EEXIST
    bjvm_epoll_entry e = {.fd = fd, .events = events};
    arrput(ep->entries, e);
  } else if (opcode == 2) {
    if (idx < 0)
      return (stack_value){.i = -1}; // ENOENT
    arrdel(ep->entries, (u32)idx);
  } else if (opcode == 3) {
    if (idx < 0)
      return (stack_value){.i = -1};
    ep->entries[idx].events = events;
  } else {
    return (stack_value){.i = -1};
  }
  return (stack_value){.i = 0};
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", EPoll, wait, "(IJII)I",
                     locals(rr_wakeup_info wakeup_info; u64 deadline_us), invoked_methods()) {
  for (;;) {
    // Locals are recomputed each iteration: after ASYNC_YIELD resumes,
    // control re-enters at the yield point and pre-loop locals are gone.
    s32 epfd = args[0].i;
    u64 poll_address = (u64)args[1].l;
    s32 num_fds = args[2].i;
    s32 timeout_ms = args[3].i;

    bjvm_fakefd *ep = bjvm_fakefd_get(epfd);
    if (!ep || ep->kind != BJVM_FAKE_EPOLL) {
      raise_vm_exception(thread, STR("java/io/IOException"), STR("bad epoll fd"));
      ASYNC_RETURN(value_null());
    }

    if (!self->deadline_us)
      self->deadline_us = timeout_ms < 0 ? UINT64_MAX : get_unix_us() + (u64)timeout_ms * 1000;

    s32 emitted = 0;
    for (u32 i = 0; i < arrlen(ep->entries) && emitted < num_fds; ++i) {
      bjvm_epoll_entry *entry = &ep->entries[i];
      int ready = 0;
      bjvm_fakefd *fake = bjvm_is_fake_fd(entry->fd) ? bjvm_fakefd_get(entry->fd) : nullptr;
      if (fake && fake->kind == BJVM_FAKE_EVENTFD) {
        if (fake->counter > 0)
          ready = 1;
      } else if (!fake) {
        int r = bjvm_fd_poll_once(entry->fd, !!(entry->events & BJVM_EPOLLIN),
                                !!(entry->events & (BJVM_EPOLLOUT | BJVM_EPOLLERR)));
        if (r < 0)
          ready = 4; // broken fd -> report as error
        else
          ready = r;
      }
      if (!ready)
        continue;
      u32 events = 0;
      if (ready & 1)
        events |= BJVM_EPOLLIN;
      if (ready & 2)
        events |= BJVM_EPOLLOUT;
      if (ready & 4)
        events |= BJVM_EPOLLERR | BJVM_EPOLLHUP;
      u8 *slot = (u8 *)(uintptr_t)poll_address + (u32)emitted * 8;
      *(u32 *)(slot) = events;
      *(u32 *)(slot + 4) = (u32)entry->fd;
      emitted++;
    }
    if (emitted > 0)
      ASYNC_RETURN((stack_value){.i = emitted});
    if (timeout_ms == 0 || get_unix_us() >= self->deadline_us)
      ASYNC_RETURN((stack_value){.i = 0});
    self->wakeup_info.kind = RR_WAKEUP_SLEEP;
    self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
    ASYNC_YIELD((void *)&self->wakeup_info);
  }
  ASYNC_END(value_null());
}

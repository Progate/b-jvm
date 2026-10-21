#include "bjvm_net.h"

// Selector wakeup. EPollSelectorImpl uses an EventFD it registers with
// the epoll instance; our epoll emulation checks the pending counter
// directly, so set0 only has to bump it. reset() goes through
// IOUtil.drain which clears it.
DECLARE_NATIVE("sun/nio/ch", EventFD, eventfd0, "()I") {
  return (stack_value){.i = bjvm_fakefd_alloc(BJVM_FAKE_EVENTFD)};
}

DECLARE_NATIVE("sun/nio/ch", EventFD, set0, "(I)I") {
  bjvm_fakefd *f = bjvm_fakefd_get(args[0].i);
  if (f && f->kind == BJVM_FAKE_EVENTFD)
    f->counter++;
  return (stack_value){.i = 0};
}

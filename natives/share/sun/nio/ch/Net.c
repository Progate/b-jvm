#include "bjvm_net.h"

// IPv4 only: BrowserOS's loopback table is IPv4, and reporting no IPv6
// support makes the JDK take the plain IPv4 path in every caller
// (socket0/bind0/connect0 then never see a v6 address).

DECLARE_NATIVE("sun/nio/ch", Net, isIPv6Available0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, isReusePortAvailable0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, isExclusiveBindAvailable, "()I") { return (stack_value){.i = -1}; }

DECLARE_NATIVE("sun/nio/ch", Net, shouldSetBothIPv4AndIPv6Options0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, canIPv6SocketJoinIPv4Group0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, canJoin6WithIPv4Group0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, canUseIPv6OptionsWithIPv4LocalAddress0, "()Z") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("sun/nio/ch", Net, initIDs, "()V") { return value_null(); }

// poll event bits, matching poll.h which Net.poll callers compare against
DECLARE_NATIVE("sun/nio/ch", Net, pollinValue, "()S") { return (stack_value){.i = 1}; }
DECLARE_NATIVE("sun/nio/ch", Net, polloutValue, "()S") { return (stack_value){.i = 4}; }
DECLARE_NATIVE("sun/nio/ch", Net, pollerrValue, "()S") { return (stack_value){.i = 8}; }
DECLARE_NATIVE("sun/nio/ch", Net, pollhupValue, "()S") { return (stack_value){.i = 16}; }
DECLARE_NATIVE("sun/nio/ch", Net, pollnvalValue, "()S") { return (stack_value){.i = 32}; }
DECLARE_NATIVE("sun/nio/ch", Net, pollconnValue, "()S") { return (stack_value){.i = 4}; }

DECLARE_NATIVE("sun/nio/ch", Net, socket0, "(ZZZZ)I") {
  // args: isIPv6, stream, reuse, fastLoopback -- v6 unsupported
  if (args[0].i) {
    raise_vm_exception(thread, STR("java/net/SocketException"), STR("IPv6 not supported"));
    return value_null();
  }
  // O_NONBLOCK forever: "blocking" Java I/O is the yield loop, never a blocking syscall
  // (→ bjvm_fd_set_nonblock).
  int fd = socket(AF_INET, (args[1].i ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    bjvm_throw_sock(thread, errno, "socket");
    return value_null();
  }
  return (stack_value){.i = fd};
}

DECLARE_NATIVE("sun/nio/ch", Net, bind0, "(Ljava/io/FileDescriptor;ZZLjava/net/InetAddress;I)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  u32 addr = bjvm_inet_addr_int(args[3].handle->obj);
  int e = bjvm_bind4(fd, addr, (u32)args[4].i);
  if (e)
    bjvm_throw_io(thread, e, "bind");
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, listen, "(Ljava/io/FileDescriptor;I)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  if (listen(fd, args[1].i) < 0)
    bjvm_throw_io(thread, errno, "listen");
  return value_null();
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", Net, connect0, "(ZLjava/io/FileDescriptor;Ljava/net/InetAddress;I)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  s32 fd = bjvm_fd_val(args[1].handle->obj);
  u32 addr = bjvm_inet_addr_int(args[2].handle->obj);
  int e = bjvm_connect4(fd, addr, (u32)args[3].i);
  if (e == EINPROGRESS) {
    // A nonblocking connect reports EINPROGRESS; on the kernel's loopback table the
    // outcome is decided immediately, so SO_ERROR already knows it.
    int soerr = 0;
    socklen_t len = sizeof soerr;
    e = (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0) ? soerr : errno;
  }
  if (e) {
    bjvm_throw_sock(thread, e, "connect");
    ASYNC_RETURN(value_null());
  }
  ASYNC_END((stack_value){.i = 1});
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", Net, accept,
                     "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;[Ljava/net/InetSocketAddress;)I",
                     locals(rr_wakeup_info wakeup_info; s64 deadline_us), invoked_methods()) {
  for (;;) {
    // Locals are recomputed each iteration: after ASYNC_YIELD resumes,
    // control re-enters at the yield point and pre-loop locals are gone.
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    // SOCK_NONBLOCK on the new fd too; the listening fd itself is already O_NONBLOCK
    // (→ socket0), so a listening queue with nothing pending answers EAGAIN.
    int newfd = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int e = newfd < 0 ? errno : 0;
    if (e == 0) {
      obj_header *nfd_obj = args[1].handle->obj;
      if (nfd_obj)
        StoreFieldInt(nfd_obj, "fd", (s32)newfd);
      // Report the peer address when the table has one; loopback-0 is a
      // fine stand-in when it doesn't (the ABI cannot carry it anyway).
      obj_header *isaa = args[2].handle->obj;
      if (isaa && ArrayLength(isaa) > 0) {
        bjvm_sockaddr peer = {.is_v4 = true, .port = 0};
        if (bjvm_getaddr(true, (s32)newfd, &peer) == 0 && peer.is_v4) {
          obj_header *isa = bjvm_make_inet_socket_address(thread, __builtin_bswap32(*(u32 *)peer.addr), peer.port);
          if (isa)
            ReferenceArrayStore(isaa, 0, isa);
        } else {
          obj_header *isa = bjvm_make_inet_socket_address(thread, 0x7f000001 /* 127.0.0.1 */, 0);
          if (isa)
            ReferenceArrayStore(isaa, 0, isa);
        }
      }
      ASYNC_RETURN((stack_value){.i = 1});
    }
    if (e != EAGAIN && e != EWOULDBLOCK) {
      bjvm_throw_io(thread, e, "accept");
      ASYNC_RETURN(value_null());
    }
    if (bjvm_fd_is_nonblock(fd)) {
      // A nonblocking accept that found nothing is "unavailable" to the
      // caller; Tomcat's poller will come back when there is an event.
      ASYNC_RETURN((stack_value){.i = BJVM_IOS_UNAVAILABLE});
    }
    // Blocking accept: yield to the round-robin scheduler and retry.
    self->wakeup_info.kind = RR_WAKEUP_SLEEP;
    self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
    ASYNC_YIELD((void *)&self->wakeup_info);
  }
  ASYNC_END(value_null());
}

DECLARE_NATIVE("sun/nio/ch", Net, shutdown, "(Ljava/io/FileDescriptor;I)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  int how = args[1].i; // JDK encodes: 0 read, 1 write (see Net.java)
  int posix_how = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : how == 2 ? SHUT_RDWR : -1;
  if (posix_how >= 0)
    (void)shutdown(fd, posix_how);
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, localPort, "(Ljava/io/FileDescriptor;)I") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  bjvm_sockaddr a;
  if (bjvm_getaddr(false, fd, &a))
    return (stack_value){.i = 0};
  return (stack_value){.i = (s32)a.port};
}

DECLARE_NATIVE("sun/nio/ch", Net, localInetAddress, "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  bjvm_sockaddr a = {.is_v4 = true};
  if (bjvm_getaddr(false, fd, &a) || !a.is_v4)
    return (stack_value){.obj = bjvm_make_inet4(thread, 0)};
  return (stack_value){.obj = bjvm_make_inet4(thread, __builtin_bswap32(*(u32 *)a.addr))};
}

DECLARE_NATIVE("sun/nio/ch", Net, remotePort, "(Ljava/io/FileDescriptor;)I") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  bjvm_sockaddr a;
  if (bjvm_getaddr(true, fd, &a))
    return (stack_value){.i = 0};
  return (stack_value){.i = (s32)a.port};
}

DECLARE_NATIVE("sun/nio/ch", Net, remoteInetAddress, "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  bjvm_sockaddr a = {.is_v4 = true};
  if (bjvm_getaddr(true, fd, &a) || !a.is_v4)
    return (stack_value){.obj = bjvm_make_inet4(thread, 0x7f000001)};
  return (stack_value){.obj = bjvm_make_inet4(thread, __builtin_bswap32(*(u32 *)a.addr))};
}

DECLARE_NATIVE("sun/nio/ch", Net, getIntOption0, "(Ljava/io/FileDescriptor;ZII)I") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  u32 val = 0;
  socklen_t len = sizeof val;
  // level/opt are the JDK's SocketOptionRegistry values, which are the platform's real constants
  if (getsockopt(fd, args[2].i, args[3].i, &val, &len) < 0)
    return (stack_value){.i = 0}; // unknown option -> default 0
  return (stack_value){.i = (s32)val};
}

DECLARE_NATIVE("sun/nio/ch", Net, setIntOption0, "(Ljava/io/FileDescriptor;ZIIIZ)V") {
  s32 fd = bjvm_fd_val(args[0].handle->obj);
  u32 val = (u32)args[4].i;
  (void)setsockopt(fd, args[2].i, args[3].i, &val, sizeof val);
  return value_null();
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", Net, poll, "(Ljava/io/FileDescriptor;IJ)I",
                     locals(rr_wakeup_info wakeup_info; u64 deadline_us), invoked_methods()) {
  for (;;) {
    // Locals are recomputed each iteration: after ASYNC_YIELD resumes,
    // control re-enters at the yield point and pre-loop locals are gone.
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    int events = args[1].i;      // poll.h bits: 1=IN, 4=OUT, 8=ERR, 16=HUP
    s64 timeout_ms = args[2].l;  // <0 wait forever, 0 return now
    if (!self->deadline_us)
      self->deadline_us = timeout_ms >= 0 ? get_unix_us() + (u64)timeout_ms * 1000 : UINT64_MAX;

    int ready = bjvm_fd_poll_once(fd, !!(events & 1), !!(events & 4));
    if (ready < 0) {
      bjvm_throw_io(thread, -ready, "poll");
      ASYNC_RETURN(value_null());
    }
    if (ready)
      ASYNC_RETURN((stack_value){.i = 1});
    if (timeout_ms == 0 || get_unix_us() >= self->deadline_us)
      ASYNC_RETURN((stack_value){.i = 0});
    self->wakeup_info.kind = RR_WAKEUP_SLEEP;
    self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
    ASYNC_YIELD((void *)&self->wakeup_info);
  }
  ASYNC_END(value_null());
}

DECLARE_NATIVE("sun/nio/ch", Net, pollConnect, "(Ljava/io/FileDescriptor;J)Z") {
  // The loopback table resolves connects immediately, so the channel is
  // either connected already or connect0 threw.
  return (stack_value){.i = 1};
}

DECLARE_NATIVE("sun/nio/ch", Net, available, "(Ljava/io/FileDescriptor;)I") {
  // No FIONREAD equivalent; 0 = "don't know, read anyway".
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", Net, sendOOB, "(Ljava/io/FileDescriptor;B)I") {
  raise_vm_exception(thread, STR("java/io/IOException"), STR("OOB data not supported"));
  return value_null();
}

DECLARE_NATIVE("sun/nio/ch", Net, discardOOB, "(Ljava/io/FileDescriptor;)Z") {
  return (stack_value){.i = 0};
}

#define BJVM_NOGROUP(msg)                                                                                                \
  raise_vm_exception(thread, STR("java/net/SocketException"), STR(msg))

DECLARE_NATIVE("sun/nio/ch", Net, joinOrDrop4, "(ZLjava/io/FileDescriptor;III)I") {
  BJVM_NOGROUP("multicast groups not supported");
  return value_null();
}
DECLARE_NATIVE("sun/nio/ch", Net, blockOrUnblock4, "(ZLjava/io/FileDescriptor;III)I") {
  BJVM_NOGROUP("multicast groups not supported");
  return value_null();
}
DECLARE_NATIVE("sun/nio/ch", Net, joinOrDrop6, "(ZLjava/io/FileDescriptor;[BI[B)I") {
  BJVM_NOGROUP("multicast groups not supported");
  return value_null();
}
DECLARE_NATIVE("sun/nio/ch", Net, blockOrUnblock6, "(ZLjava/io/FileDescriptor;[BI[B)I") {
  BJVM_NOGROUP("multicast groups not supported");
  return value_null();
}
DECLARE_NATIVE("sun/nio/ch", Net, setInterface4, "(Ljava/io/FileDescriptor;I)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/ch", Net, getInterface4, "(Ljava/io/FileDescriptor;)I") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/nio/ch", Net, setInterface6, "(Ljava/io/FileDescriptor;I)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/ch", Net, getInterface6, "(Ljava/io/FileDescriptor;)I") { return (stack_value){.i = 0}; }

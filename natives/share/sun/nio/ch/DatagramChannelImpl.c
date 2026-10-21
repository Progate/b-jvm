#include "bjvm_net.h"

/*
 * DatagramChannel receive/send/disconnect. The JDK hands receive0 a
 * NativeSocketAddress buffer laid out like a real sockaddr_in(6), but
 * the WASI socket ABI uses its own {family u16, addr bytes @+2} form
 * with the port out-of-band -- so each call transcodes between the two.
 *
 * Families: ABI uses 1=ipv4 / 2=ipv6, the JDK sockaddr uses 2=AF_INET /
 * 10=AF_INET6. Datagrams are IPv4-only here (IPv6 fails at socket0).
 */

static bool bjvm_jdk_sockaddr_decode(const u8 *sa, u32 sa_len, bjvm_sockaddr *out) {
  u16 family = *(const u16 *)sa;
  if (family == 2 /* AF_INET */ && sa_len >= 16) {
    out->is_v4 = true;
    memcpy(out->addr, sa + 4, 4);
    out->port = __builtin_bswap16(*(const u16 *)(sa + 2));
    return true;
  }
  if (family == 10 /* AF_INET6 */ && sa_len >= 28) {
    out->is_v4 = false;
    memcpy(out->addr, sa + 8, 16);
    out->port = __builtin_bswap16(*(const u16 *)(sa + 2));
    return true;
  }
  return false;
}

static void bjvm_jdk_sockaddr_encode(u8 *sa, u32 sa_len, const u8 addr4[4], u32 port) {
  memset(sa, 0, sa_len);
  *(u16 *)sa = 2 /* AF_INET */;
  *(u16 *)(sa + 2) = (u16)__builtin_bswap16((u16)port);
  memcpy(sa + 4, addr4, 4);
}

DECLARE_NATIVE("sun/nio/ch", DatagramChannelImpl, disconnect0, "(Ljava/io/FileDescriptor;Z)V") {
  // The socket table has no disconnect operation; the channel stays
  // bound and simply stops filtering to one remote. Harmless as a no-op.
  return value_null();
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramChannelImpl, receive0, "(Ljava/io/FileDescriptor;JIJZ)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    __wasi_iovec_t iov = {.buf = (u8 *)(uintptr_t)(u64)args[1].l, .buf_len = (u32)args[2].i};
    u8 abi_addr[128];
    bjvm_addrbuf ab = {abi_addr, 128};
    u32 port = 0;
    __wasi_size_t nread = 0;
    u16 oflags = 0;
    __wasi_errno_t e = bjvm_sock_recv_from(fd, &iov, 1, &ab, 0, &port, &nread, &oflags);
    if (e == __WASI_ERRNO_AGAIN) {
      if (nonblock)
        ASYNC_RETURN((stack_value){.i = BJVM_IOS_UNAVAILABLE});
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    if (e) {
      bjvm_throw_sock(thread, e, "receive");
      ASYNC_RETURN(value_null());
    }
    u16 family = *(u16 *)abi_addr;
    if (family == 1 /* ABI ipv4 */)
      bjvm_jdk_sockaddr_encode((u8 *)(uintptr_t)(u64)args[3].l, 16, abi_addr + 2, port);
    else
      memset((void *)(uintptr_t)(u64)args[3].l, 0, 28);
    ASYNC_RETURN((stack_value){.i = (s32)nread});
  }
  ASYNC_END(value_null());
}

DECLARE_ASYNC_NATIVE("sun/nio/ch", DatagramChannelImpl, send0, "(Ljava/io/FileDescriptor;JIJI)I",
                     locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  for (;;) {
    s32 fd = bjvm_fd_val(args[0].handle->obj);
    bool nonblock = bjvm_fd_is_nonblock(fd);
    __wasi_ciovec_t iov = {.buf = (const u8 *)(uintptr_t)(u64)args[1].l, .buf_len = (u32)args[2].i};
    bjvm_sockaddr target = {0};
    if (!bjvm_jdk_sockaddr_decode((const u8 *)(uintptr_t)(u64)args[3].l, (u32)args[4].i, &target)) {
      raise_vm_exception(thread, STR("java/net/SocketException"), STR("unsupported address family"));
      ASYNC_RETURN(value_null());
    }
    if (!target.is_v4) {
      raise_vm_exception(thread, STR("java/net/SocketException"), STR("IPv6 not supported"));
      ASYNC_RETURN(value_null());
    }
    bjvm_addrbuf ab = {target.addr, 4};
    __wasi_size_t nwritten = 0;
    __wasi_errno_t e = bjvm_sock_send_to(fd, &iov, 1, &ab, target.port, 0, &nwritten);
    if (e == __WASI_ERRNO_AGAIN) {
      if (nonblock)
        ASYNC_RETURN((stack_value){.i = BJVM_IOS_UNAVAILABLE});
      self->wakeup_info.kind = RR_WAKEUP_SLEEP;
      self->wakeup_info.wakeup_us = get_unix_us() + BJVM_IO_POLL_US;
      ASYNC_YIELD((void *)&self->wakeup_info);
      continue;
    }
    if (e) {
      bjvm_throw_sock(thread, e, "send");
      ASYNC_RETURN(value_null());
    }
    ASYNC_RETURN((stack_value){.i = (s32)nwritten});
  }
  ASYNC_END(value_null());
}

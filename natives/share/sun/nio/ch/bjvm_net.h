#ifndef BJVM_NET_H
#define BJVM_NET_H

/*
 * Socket natives for sun.nio.ch, backed by the WASI preview1 socket
 * extensions that BrowserOS registers for every guest (sock_open /
 * sock_bind / sock_listen / sock_connect / sock_getlocaladdr /
 * sock_getpeeraddr / sock_getsockopt / sock_setsockopt) plus the standard
 * fd_read / fd_write / poll_oneoff / sock_accept. Blocking ops are
 * emulated by nonblocking polling plus round-robin scheduler yields so a
 * blocked Java thread does not stall the whole VM.
 *
 * Intentionally missing: IPv6 (isIPv6Available0 reports false so the JDK
 * takes the IPv4 path everywhere), UDP, and host name resolution
 * (InetAddressImpl answers "localhost" only).
 */

#include <natives-dsl.h>
#include <roundrobin_scheduler.h>
#include <wasi/api.h>

#define BJVM_WASI_IMPORT(name) __attribute__((import_module("wasi_snapshot_preview1"), import_name(name)))

// BrowserOS socket extensions (browser-wasi p1/sockets.ts). Addresses are
// passed as a pointer to a ciovec-like {buf, bufLen} containing either 4
// raw IPv4 bytes, 16 raw IPv6 bytes, or a 128-byte sockaddr_any
// {family:u16, addr:...}. Families (BSD values): 1=inet, 2=inet6. socktype 2=stream.
BJVM_WASI_IMPORT("sock_open")
__wasi_errno_t bjvm_sock_open(u32 family, u32 socktype, __wasi_fd_t *result_fd);
BJVM_WASI_IMPORT("sock_bind")
__wasi_errno_t bjvm_sock_bind(__wasi_fd_t fd, const void *addrbuf, u32 port);
BJVM_WASI_IMPORT("sock_listen")
__wasi_errno_t bjvm_sock_listen(__wasi_fd_t fd, u32 backlog);
BJVM_WASI_IMPORT("sock_connect")
__wasi_errno_t bjvm_sock_connect(__wasi_fd_t fd, const void *addrbuf, u32 port);
BJVM_WASI_IMPORT("sock_getlocaladdr")
__wasi_errno_t bjvm_sock_getlocaladdr(__wasi_fd_t fd, void *addrbuf, u32 *port);
BJVM_WASI_IMPORT("sock_getpeeraddr")
__wasi_errno_t bjvm_sock_getpeeraddr(__wasi_fd_t fd, void *addrbuf, u32 *port);
BJVM_WASI_IMPORT("sock_getsockopt")
__wasi_errno_t bjvm_sock_getsockopt(__wasi_fd_t fd, u32 level, u32 name, void *value, u32 value_len);
BJVM_WASI_IMPORT("sock_setsockopt")
__wasi_errno_t bjvm_sock_setsockopt(__wasi_fd_t fd, u32 level, u32 name, const void *value, u32 value_len);
BJVM_WASI_IMPORT("sock_recv_from")
__wasi_errno_t bjvm_sock_recv_from(__wasi_fd_t fd, const __wasi_iovec_t *iovs, u32 iovs_len,
                                   const void *addrbuf, u32 flags, u32 *port, __wasi_size_t *nread, u16 *oflags);
BJVM_WASI_IMPORT("sock_send_to")
__wasi_errno_t bjvm_sock_send_to(__wasi_fd_t fd, const __wasi_ciovec_t *iovs, u32 iovs_len,
                                 const void *addrbuf, u32 port, u32 flags, __wasi_size_t *nwritten);

// JDK IOStatus codes
#define BJVM_IOS_EOF (-1)
#define BJVM_IOS_UNAVAILABLE (-2)
#define BJVM_IOS_INTERRUPTED (-3)
#define BJVM_IOS_UNSUPPORTED (-4)
#define BJVM_IOS_THROWN (-5)

// Fake fd space. EventFDs and epoll instances are VM-internal objects the
// host fd table knows nothing about, so they get numbers no real fd can
// take. Any site that closes/drains/polls an fd must special-case them.
#define BJVM_FAKE_FD_BASE 0x20000

typedef enum { BJVM_FAKE_EVENTFD = 1, BJVM_FAKE_EPOLL } bjvm_fakefd_kind;

typedef struct {
  int fd;
  int events; // EPOLLIN/EPOLLOUT bits the selector registered for
} bjvm_epoll_entry;

typedef struct {
  int fd;
  bjvm_fakefd_kind kind;
  u64 counter; // eventfd: pending signal count
  bjvm_epoll_entry *entries; // epoll: registered fds (stretchy buffer)
} bjvm_fakefd;

// Shared across all translation units via bjvm_net.c — a static here would
// give every .c file its own private table.
extern bjvm_fakefd *bjvm_fakefds;
extern int bjvm_next_fake_fd;

static inline bool bjvm_is_fake_fd(int fd) { return fd >= BJVM_FAKE_FD_BASE; }

static bjvm_fakefd *bjvm_fakefd_get(int fd) {
  for (u32 i = 0; i < arrlen(bjvm_fakefds); ++i) {
    if (bjvm_fakefds[i].fd == fd)
      return &bjvm_fakefds[i];
  }
  return nullptr;
}

static int bjvm_fakefd_alloc(bjvm_fakefd_kind kind) {
  bjvm_fakefd f = {.fd = bjvm_next_fake_fd++, .kind = kind, .counter = 0, .entries = nullptr};
  arrput(bjvm_fakefds, f);
  return f.fd;
}

static void bjvm_fakefd_free(int fd) {
  for (u32 i = 0; i < arrlen(bjvm_fakefds); ++i) {
    if (bjvm_fakefds[i].fd == fd) {
      arrfree(bjvm_fakefds[i].entries);
      arrdel(bjvm_fakefds, i);
      return;
    }
  }
}

// fds the JDK configured nonblocking via IOUtil.configureBlocking
extern int *bjvm_nonblock_fds;

static inline bool bjvm_fd_is_nonblock(int fd) {
  for (u32 i = 0; i < arrlen(bjvm_nonblock_fds); ++i) {
    if (bjvm_nonblock_fds[i] == fd)
      return true;
  }
  return false;
}

static void bjvm_fd_set_nonblock(int fd, bool nonblock) {
  if (!bjvm_is_fake_fd(fd)) {
    __wasi_fdstat_t stat;
    if (__wasi_fd_fdstat_get(fd, &stat) == 0) {
      __wasi_fdflags_t flags = nonblock ? (stat.fs_flags | __WASI_FDFLAGS_NONBLOCK)
                                        : (stat.fs_flags & ~(__wasi_fdflags_t)__WASI_FDFLAGS_NONBLOCK);
      (void)__wasi_fd_fdstat_set_flags(fd, flags);
    }
  }
  for (u32 i = 0; i < arrlen(bjvm_nonblock_fds); ++i) {
    if (bjvm_nonblock_fds[i] == fd) {
      if (!nonblock)
        arrdel(bjvm_nonblock_fds, i);
      return;
    }
  }
  if (nonblock)
    arrput(bjvm_nonblock_fds, fd);
}

static inline int bjvm_fd_val(obj_header *fd_obj) {
  return fd_obj ? LoadFieldInt(fd_obj, "fd") : -1;
}

static void bjvm_throw_sock(vm_thread *thread, __wasi_errno_t errno_, const char *what) {
  char msg[128];
  snprintf(msg, sizeof msg, "%s: errno %d", what, (int)errno_);
  raise_vm_exception(thread, STR("java/net/SocketException"), hslc((heap_string){.chars = msg, .len = (u32)strlen(msg)}));
}

static void bjvm_throw_io(vm_thread *thread, __wasi_errno_t errno_, const char *what) {
  char msg[128];
  snprintf(msg, sizeof msg, "%s: errno %d", what, (int)errno_);
  raise_vm_exception(thread, STR("java/io/IOException"), hslc((heap_string){.chars = msg, .len = (u32)strlen(msg)}));
}

// Generic fd_read / fd_write I/O, shared by the stream and datagram
// dispatchers (both operate on plain fds). `address` is Unsafe-allocated
// native memory; readv/writev take a struct iovec[] there where wasm32
// entries are {base:u32, len:u32} = 8 bytes each.
static stack_value bjvm_do_read0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  __wasi_iovec_t iov = {.buf = (u8 *)(uintptr_t)address, .buf_len = (u32)count};
  __wasi_size_t nread = 0;
  __wasi_errno_t e = __wasi_fd_read(fd, &iov, 1, &nread);
  if (e == __WASI_ERRNO_AGAIN)
    return (stack_value){.i = BJVM_IOS_UNAVAILABLE};
  if (e == __WASI_ERRNO_BADF || e == __WASI_ERRNO_NOTSOCK || e == __WASI_ERRNO_INVAL) {
    bjvm_throw_io(thread, e, "read");
    return value_null();
  }
  if (e) {
    // EOF and pipe-close surface as zero-length reads or specific
    // errnos depending on backend; treat leftovers as EOF.
    return (stack_value){.i = BJVM_IOS_EOF};
  }
  return (stack_value){.i = nread ? (s32)nread : BJVM_IOS_EOF};
}

static stack_value bjvm_do_readv0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  if (count <= 0 || count > 1024)
    return (stack_value){.l = BJVM_IOS_UNSUPPORTED};
  __wasi_iovec_t iovs[64];
  u32 n = count > 64 ? 64 : (u32)count;
  const u8 *native_iovs = (const u8 *)(uintptr_t)address;
  for (u32 i = 0; i < n; ++i) {
    u32 base = *(const u32 *)(native_iovs + i * 8);
    u32 len = *(const u32 *)(native_iovs + i * 8 + 4);
    iovs[i] = (__wasi_iovec_t){.buf = (u8 *)(uintptr_t)base, .buf_len = len};
  }
  __wasi_size_t nread = 0;
  __wasi_errno_t e = __wasi_fd_read(fd, iovs, n, &nread);
  if (e == __WASI_ERRNO_AGAIN)
    return (stack_value){.l = BJVM_IOS_UNAVAILABLE};
  if (e)
    return (stack_value){.l = BJVM_IOS_EOF};
  return (stack_value){.l = nread ? (s64)nread : BJVM_IOS_EOF};
}

static stack_value bjvm_do_write0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  __wasi_ciovec_t iov = {.buf = (const u8 *)(uintptr_t)address, .buf_len = (u32)count};
  __wasi_size_t nwritten = 0;
  __wasi_errno_t e = __wasi_fd_write(fd, &iov, 1, &nwritten);
  if (e == __WASI_ERRNO_AGAIN)
    return (stack_value){.i = BJVM_IOS_UNAVAILABLE};
  if (e) {
    bjvm_throw_io(thread, e, "write");
    return value_null();
  }
  return (stack_value){.i = (s32)nwritten};
}

static stack_value bjvm_do_writev0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  if (count <= 0 || count > 1024)
    return (stack_value){.l = BJVM_IOS_UNSUPPORTED};
  __wasi_ciovec_t iovs[64];
  u32 n = count > 64 ? 64 : (u32)count;
  const u8 *native_iovs = (const u8 *)(uintptr_t)address;
  for (u32 i = 0; i < n; ++i) {
    u32 base = *(const u32 *)(native_iovs + i * 8);
    u32 len = *(const u32 *)(native_iovs + i * 8 + 4);
    iovs[i] = (__wasi_ciovec_t){.buf = (const u8 *)(uintptr_t)base, .buf_len = len};
  }
  __wasi_size_t nwritten = 0;
  __wasi_errno_t e = __wasi_fd_write(fd, iovs, n, &nwritten);
  if (e == __WASI_ERRNO_AGAIN)
    return (stack_value){.l = BJVM_IOS_UNAVAILABLE};
  if (e) {
    bjvm_throw_io(thread, e, "writev");
    return (stack_value){.l = BJVM_IOS_THROWN};
  }
  return (stack_value){.l = (s64)nwritten};
}


// {buf, bufLen} pair the extension ABI takes its address-of-pointer to.
typedef struct {
  const void *buf;
  u32 buf_len;
} bjvm_addrbuf;

// `addr_int` is the holder().address packed int, which is big-endian as a
// value; the ABI wants the octets in network order.
static inline __wasi_errno_t bjvm_bind4(int fd, u32 addr_int, u32 port) {
  u32 be = __builtin_bswap32(addr_int);
  bjvm_addrbuf ab = {&be, 4};
  return bjvm_sock_bind(fd, &ab, port);
}

static inline __wasi_errno_t bjvm_connect4(int fd, u32 addr_int, u32 port) {
  u32 be = __builtin_bswap32(addr_int);
  bjvm_addrbuf ab = {&be, 4};
  return bjvm_sock_connect(fd, &ab, port);
}

typedef struct {
  bool is_v4;
  u8 addr[16]; // first 4 bytes used for v4
  u32 port;
} bjvm_sockaddr;

static inline __wasi_errno_t bjvm_getaddr(bool peer, int fd, bjvm_sockaddr *out) {
  u8 sockaddr_any[128];
  bjvm_addrbuf ab = {sockaddr_any, 128};
  u32 port = 0;
  __wasi_errno_t e = peer ? bjvm_sock_getpeeraddr(fd, &ab, &port) : bjvm_sock_getlocaladdr(fd, &ab, &port);
  if (e)
    return e;
  u16 family = *(u16 *)sockaddr_any;
  out->is_v4 = family != 2;
  memcpy(out->addr, sockaddr_any + 2, out->is_v4 ? 4 : 16);
  out->port = port;
  return 0;
}

// Nonblocking readiness check on a real fd via poll_oneoff plus a
// zero-timeout clock subscription. Returns ready bits (1=read, 2=write)
// or -(errno).
static inline int bjvm_fd_poll_once(int fd, bool want_read, bool want_write) {
  __wasi_subscription_t subs[3];
  __wasi_event_t out[3];
  memset(subs, 0, sizeof subs);
  u32 n = 0;
  if (want_read) {
    subs[n].userdata = n;
    subs[n].u.tag = __WASI_EVENTTYPE_FD_READ;
    subs[n].u.u.fd_read.file_descriptor = fd;
    n++;
  }
  if (want_write) {
    subs[n].userdata = n;
    subs[n].u.tag = __WASI_EVENTTYPE_FD_WRITE;
    subs[n].u.u.fd_write.file_descriptor = fd;
    n++;
  }
  subs[n].userdata = n;
  subs[n].u.tag = __WASI_EVENTTYPE_CLOCK;
  subs[n].u.u.clock.id = __WASI_CLOCKID_MONOTONIC;
  subs[n].u.u.clock.timeout = 0;
  subs[n].u.u.clock.precision = 0;
  subs[n].u.u.clock.flags = 0;
  n++;

  __wasi_size_t nevents = 0;
  __wasi_errno_t e = __wasi_poll_oneoff(subs, out, n, &nevents);
  if (e)
    return -(int)e;
  int ready = 0;
  for (u32 i = 0; i < nevents; ++i) {
    if (out[i].type == __WASI_EVENTTYPE_FD_READ)
      ready |= out[i].error ? 4 : 1;
    else if (out[i].type == __WASI_EVENTTYPE_FD_WRITE)
      ready |= 2;
  }
  return ready;
}

// Run class init + a ()V ctor synchronously for `cd`, returning the
// object (or nullptr with thread->current_exception set on failure).
static obj_header *bjvm_new_simple_object(vm_thread *thread, classdesc *cd, slice init_name, slice init_desc) {
  initialize_class_t init_ctx = {.args = {thread, cd}};
  thread->stack.synchronous_depth++;
  future_t f = initialize_class(&init_ctx);
  thread->stack.synchronous_depth--;
  if (f.status != FUTURE_READY || init_ctx._result)
    return nullptr;
  cp_method *ctor = method_lookup(cd, init_name, init_desc, true, false);
  obj_header *o = new_object(thread, cd);
  if (!ctor || !o)
    return nullptr;
  thread->stack.synchronous_depth++;
  call_interpreter_synchronous(thread, ctor, (stack_value[]){{.obj = o}});
  thread->stack.synchronous_depth--;
  return o;
}

// Inet4Address with holder().address = packed network-order int.
static obj_header *bjvm_make_inet4(vm_thread *thread, u32 addr_be_int) {
  classdesc *ia = bootstrap_lookup_class(thread, STR("java/net/Inet4Address"));
  if (!ia)
    return nullptr;
  obj_header *o = bjvm_new_simple_object(thread, ia, STR("<init>"), STR("()V"));
  if (!o)
    return nullptr;
  obj_header *holder = LoadFieldObject(o, "java/net/InetAddress$InetAddressHolder", "holder");
  if (holder)
    StoreFieldInt(holder, "address", (s32)addr_be_int);
  return o;
}

static obj_header *bjvm_make_inet_socket_address(vm_thread *thread, u32 addr_be_int, u32 port) {
  obj_header *ia = bjvm_make_inet4(thread, addr_be_int);
  if (!ia)
    return nullptr;
  classdesc *isa = bootstrap_lookup_class(thread, STR("java/net/InetSocketAddress"));
  if (!isa)
    return nullptr;
  initialize_class_t init_ctx = {.args = {thread, isa}};
  thread->stack.synchronous_depth++;
  future_t f = initialize_class(&init_ctx);
  thread->stack.synchronous_depth--;
  if (f.status != FUTURE_READY || init_ctx._result)
    return nullptr;
  cp_method *ctor = method_lookup(isa, STR("<init>"), STR("(Ljava/net/InetAddress;I)V"), true, false);
  obj_header *o = new_object(thread, isa);
  if (!ctor || !o)
    return nullptr;
  thread->stack.synchronous_depth++;
  call_interpreter_synchronous(thread, ctor, (stack_value[]){{.obj = o}, {.obj = ia}, {.i = (s32)port}});
  thread->stack.synchronous_depth--;
  return o;
}

// The packed network-order int in holder().address of an InetAddress.
static inline u32 bjvm_inet_addr_int(obj_header *inet) {
  if (!inet)
    return 0;
  obj_header *holder = LoadFieldObject(inet, "java/net/InetAddress$InetAddressHolder", "holder");
  return holder ? (u32)LoadFieldInt(holder, "address") : 0;
}

// How long a blocked-on-I/O Java thread sleeps between readiness checks.
#define BJVM_IO_POLL_US 4000

#endif

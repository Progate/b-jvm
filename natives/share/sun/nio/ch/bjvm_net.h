#ifndef BJVM_NET_H
#define BJVM_NET_H

/*
 * Socket natives for sun.nio.ch, backed by ordinary POSIX socket and fd
 * calls (socket/bind/listen/connect/accept4/getsockname/getpeername/
 * getsockopt/setsockopt/readv/writev/recvmsg/sendmsg/shutdown/close/poll),
 * which under the wasm32-unknown-linux-musl build the BrowserOS kernel
 * serves through browser_os_linux.syscall. The level/opt ints the JDK
 * passes to getsockopt/setsockopt are the real Linux constants
 * (SocketOptionRegistry is generated from the JDK build's headers), so
 * they go through verbatim.
 *
 * Sockets are created O_NONBLOCK and stay that way: a blocking-mode Java
 * read is emulated by nonblocking polling plus round-robin scheduler
 * yields, so a blocked Java thread does not stall the single-threaded VM.
 * bjvm_nonblock_fds records the *JDK-level* blocking mode, which decides
 * between UNAVAILABLE and keep-polling.
 *
 * Intentionally missing: IPv6 (isIPv6Available0 reports false so the JDK
 * takes the IPv4 path everywhere), UDP multicast groups, and host name
 * resolution (InetAddressImpl answers "localhost" only).
 */

#include <natives-dsl.h>
#include <roundrobin_scheduler.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <unistd.h>

// JDK IOStatus codes
#define BJVM_IOS_EOF (-1)
#define BJVM_IOS_UNAVAILABLE (-2)
#define BJVM_IOS_INTERRUPTED (-3)
#define BJVM_IOS_UNSUPPORTED (-4)
#define BJVM_IOS_THROWN (-5)

// Fake fd space. EventFDs and epoll instances are VM-internal objects the
// kernel fd table knows nothing about, so they get numbers no real fd can
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

// Only updates the bookkeeping: real fds stay O_NONBLOCK regardless (a
// "blocking" Java socket is the yield loop over EAGAIN, not a blocking
// syscall — there is no other thread to run while we sleep). Clearing
// O_NONBLOCK on a blocking-mode channel would hang the whole VM.
static void bjvm_fd_set_nonblock(int fd, bool nonblock) {
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

static void bjvm_throw_sock(vm_thread *thread, int errno_, const char *what) {
  char msg[128];
  snprintf(msg, sizeof msg, "%s: errno %d", what, (int)errno_);
  raise_vm_exception(thread, STR("java/net/SocketException"), hslc((heap_string){.chars = msg, .len = (u32)strlen(msg)}));
}

static void bjvm_throw_io(vm_thread *thread, int errno_, const char *what) {
  char msg[128];
  snprintf(msg, sizeof msg, "%s: errno %d", what, (int)errno_);
  raise_vm_exception(thread, STR("java/io/IOException"), hslc((heap_string){.chars = msg, .len = (u32)strlen(msg)}));
}

// Generic readv / writev I/O, shared by the stream and datagram
// dispatchers (both operate on plain fds). `address` is Unsafe-allocated
// native memory; readv/writev take a struct iovec[] there where wasm32
// entries are {base:u32, len:u32} = 8 bytes each, matching struct iovec.
static stack_value bjvm_do_read0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  struct iovec iov = {.iov_base = (void *)(uintptr_t)address, .iov_len = (size_t)count};
  ssize_t n = readv(fd, &iov, 1);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return (stack_value){.i = BJVM_IOS_UNAVAILABLE};
  if (n < 0 && (errno == EBADF || errno == ENOTSOCK || errno == EINVAL)) {
    bjvm_throw_io(thread, errno, "read");
    return value_null();
  }
  if (n < 0) {
    // EOF and pipe-close surface as zero-length reads or specific
    // errnos depending on backend; treat leftovers as EOF.
    return (stack_value){.i = BJVM_IOS_EOF};
  }
  return (stack_value){.i = n ? (s32)n : BJVM_IOS_EOF};
}

// Decode the wasm32 struct iovec[] at `address` (entries come from the
// JDK's IOVec wrapper and are {base:u32, len:u32} = 8 bytes each).
static inline void bjvm_decode_iovs(struct iovec *iovs, u64 address, u32 n) {
  const u8 *native_iovs = (const u8 *)(uintptr_t)address;
  for (u32 i = 0; i < n; ++i) {
    u32 base = *(const u32 *)(native_iovs + i * 8);
    u32 len = *(const u32 *)(native_iovs + i * 8 + 4);
    iovs[i] = (struct iovec){.iov_base = (void *)(uintptr_t)base, .iov_len = len};
  }
}

static stack_value bjvm_do_readv0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  if (count <= 0 || count > 1024)
    return (stack_value){.l = BJVM_IOS_UNSUPPORTED};
  struct iovec iovs[64];
  u32 n = count > 64 ? 64 : (u32)count;
  bjvm_decode_iovs(iovs, address, n);
  ssize_t nread = readv(fd, iovs, (int)n);
  if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return (stack_value){.l = BJVM_IOS_UNAVAILABLE};
  if (nread < 0)
    return (stack_value){.l = BJVM_IOS_EOF};
  return (stack_value){.l = nread ? (s64)nread : BJVM_IOS_EOF};
}

static stack_value bjvm_do_write0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  struct iovec iov = {.iov_base = (void *)(uintptr_t)address, .iov_len = (size_t)count};
  ssize_t n = writev(fd, &iov, 1);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return (stack_value){.i = BJVM_IOS_UNAVAILABLE};
  if (n < 0) {
    bjvm_throw_io(thread, errno, "write");
    return value_null();
  }
  return (stack_value){.i = (s32)n};
}

static stack_value bjvm_do_writev0(vm_thread *thread, s32 fd, u64 address, s32 count) {
  if (count <= 0 || count > 1024)
    return (stack_value){.l = BJVM_IOS_UNSUPPORTED};
  struct iovec iovs[64];
  u32 n = count > 64 ? 64 : (u32)count;
  bjvm_decode_iovs(iovs, address, n);
  ssize_t nwritten = writev(fd, iovs, (int)n);
  if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return (stack_value){.l = BJVM_IOS_UNAVAILABLE};
  if (nwritten < 0) {
    bjvm_throw_io(thread, errno, "writev");
    return (stack_value){.l = BJVM_IOS_THROWN};
  }
  return (stack_value){.l = (s64)nwritten};
}

// {buf, bufLen} pair holding 4 raw IPv4 bytes (or 16 IPv6), the same
// shape the old socket ABI took.
typedef struct {
  const void *buf;
  u32 buf_len;
} bjvm_addrbuf;

// `addr_int` is the holder().address packed int, which is big-endian as a
// value; sockaddr_in wants the octets in network order.
static inline int bjvm_bind4(int fd, u32 addr_int, u32 port) {
  struct sockaddr_in sa = {
      .sin_family = AF_INET,
      .sin_port = htons((u16)port),
  };
  u32 be = __builtin_bswap32(addr_int);
  memcpy(&sa.sin_addr, &be, 4);
  return bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 ? 0 : errno;
}

static inline int bjvm_connect4(int fd, u32 addr_int, u32 port) {
  struct sockaddr_in sa = {
      .sin_family = AF_INET,
      .sin_port = htons((u16)port),
  };
  u32 be = __builtin_bswap32(addr_int);
  memcpy(&sa.sin_addr, &be, 4);
  return connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0 ? 0 : errno;
}

typedef struct {
  bool is_v4;
  u8 addr[16]; // first 4 bytes used for v4
  u32 port;
} bjvm_sockaddr;

static inline int bjvm_getaddr(bool peer, int fd, bjvm_sockaddr *out) {
  struct sockaddr_storage sa;
  socklen_t len = sizeof sa;
  int r = peer ? getpeername(fd, (struct sockaddr *)&sa, &len) : getsockname(fd, (struct sockaddr *)&sa, &len);
  if (r < 0)
    return errno;
  out->is_v4 = sa.ss_family != AF_INET6;
  if (out->is_v4) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)&sa;
    memcpy(out->addr, &in->sin_addr, 4);
    out->port = ntohs(in->sin_port);
  } else {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&sa;
    memcpy(out->addr, &in6->sin6_addr, 16);
    out->port = ntohs(in6->sin6_port);
  }
  return 0;
}

// Nonblocking readiness check on a real fd. Returns ready bits
// (1=read, 2=write, 4=error) or -(errno).
static inline int bjvm_fd_poll_once(int fd, bool want_read, bool want_write) {
  struct pollfd pfd = {.fd = fd, .events = 0, .revents = 0};
  if (want_read)
    pfd.events |= POLLIN;
  if (want_write)
    pfd.events |= POLLOUT;
  int r = poll(&pfd, 1, 0);
  if (r < 0)
    return -errno;
  if (r == 0)
    return 0;
  int ready = 0;
  if (pfd.revents & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI))
    ready |= 1;
  if (pfd.revents & POLLOUT)
    ready |= 2;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    ready |= 4;
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

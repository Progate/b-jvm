
#include <natives-dsl.h>
#ifdef EMSCRIPTEN
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#else
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __wasi__
// UnixException.translateToIOException は errno を同梱 JDK の UnixConstants（Linux で生成）と
// 比べて NoSuchFileException などを選ぶ。wasi-libc の errno は番号が違う（ENOENT が 44）ので、
// そのままだとどれにも当たらず素の FileSystemException になり、NoSuchFileException を
// 受け止めるつもりのコード（IntelliJ の Registry が読む early-access-registry.txt）が落ちる。
// 表にあるのは UnixConstants にあるものだけで、それ以外は strerror のためにそのまま通す
static const struct {
  int wasi;
  int linux;
} errno_to_linux[] = {
    {ENOENT, 2},     {ENXIO, 6},   {EACCES, 13},  {EEXIST, 17},  {ENOTDIR, 20}, {EINVAL, 22},
    {EXDEV, 18},     {EISDIR, 21}, {ENOTEMPTY, 39}, {ENOSPC, 28}, {EAGAIN, 11},  {ENOSYS, 38},
    {ELOOP, 40},     {EROFS, 30},  {ERANGE, 34},  {EMFILE, 24},
};

static int to_linux_errno(int code) {
  for (size_t i = 0; i < sizeof(errno_to_linux) / sizeof(errno_to_linux[0]); ++i)
    if (errno_to_linux[i].wasi == code)
      return errno_to_linux[i].linux;
  return code;
}

static int from_linux_errno(int code) {
  for (size_t i = 0; i < sizeof(errno_to_linux) / sizeof(errno_to_linux[0]); ++i)
    if (errno_to_linux[i].linux == code)
      return errno_to_linux[i].wasi;
  return code;
}
#else
static int to_linux_errno(int code) { return code; }
static int from_linux_errno(int code) { return code; }
#endif

static obj_header *create_unix_exception_from_linux(vm_thread *thread, int errno_code);

static obj_header *create_unix_exception(vm_thread *thread, int errno_code) {
  return create_unix_exception_from_linux(thread, to_linux_errno(errno_code));
}

static obj_header *create_unix_exception_from_linux(vm_thread *thread, int errno_code) {
  classdesc *classdesc = cached_classes(thread->vm)->unix_exception;
  obj_header *obj = new_object(thread, classdesc);

  cp_method *method = method_lookup(classdesc, STR("<init>"), STR("(I)V"), true, false);
  call_interpreter_synchronous(thread, method,
                               (stack_value[]){{.obj = obj}, {.i = errno_code}}); // constructor is void method

  return obj;
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, init, "()I") { return value_null(); }

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getcwd, "()[B") {
  INIT_STACK_STRING(cwd, 1024);
  char *p = getcwd(cwd.chars, 1024);
  if (p == nullptr) {
    return value_null();
  }
  cwd.len = strlen(cwd.chars);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, cwd.len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), cwd.chars, cwd.len);
  return (stack_value){.obj = array};
}

// UnixFileAttributes の欄へ写す。stat0 / lstat0 / fstat0 / fstatat0 が共有する
static void fill_attrs(obj_header *attrs, const struct stat *st) {

#define MapAttrLong(name, value) StoreFieldLong(attrs, (#name), value)
#define MapAttrInt(name, value) StoreFieldInt(attrs, (#name), value)
  MapAttrInt(st_mode, st->st_mode);
  MapAttrLong(st_ino, st->st_ino);
  MapAttrLong(st_dev, st->st_dev);
  MapAttrLong(st_rdev, st->st_rdev);
  MapAttrInt(st_nlink, st->st_nlink);
  MapAttrInt(st_uid, st->st_uid);
  MapAttrInt(st_gid, st->st_gid);
  MapAttrLong(st_size, st->st_size);

#ifdef __APPLE__
  MapAttrLong(st_atime_sec, st->st_atime);
  MapAttrLong(st_atime_nsec, st->st_atimensec);

  MapAttrLong(st_mtime_sec, st->st_mtime);
  MapAttrLong(st_mtime_nsec, st->st_mtimensec);

  MapAttrLong(st_ctime_sec, st->st_ctime);
  MapAttrLong(st_ctime_nsec, st->st_ctimensec);
#else
  MapAttrLong(st_atime_sec, st->st_atim.tv_sec);
  MapAttrLong(st_atime_nsec, st->st_atim.tv_nsec);

  MapAttrLong(st_mtime_sec, st->st_mtim.tv_sec);
  MapAttrLong(st_mtime_nsec, st->st_mtim.tv_nsec);

  MapAttrLong(st_ctime_sec, st->st_ctim.tv_sec);
  MapAttrLong(st_ctime_nsec, st->st_ctim.tv_nsec);
#endif

#ifdef __APPLE__
  MapAttrLong(st_birthtime_sec, st->st_birthtime);
  MapAttrLong(st_birthtime_nsec, st->st_birthtimensec);
#endif

#undef MapAttrLong
#undef MapAttrInt

}

stack_value stat_impl(value *args, bool follow) {
  struct stat st;

  if (!args[1].handle)
    return value_null();

  uintptr_t buf = args[0].l;
  int result = follow ? stat((char *)buf, &st) : lstat((char *)buf, &st);
  if (result)
    return (stack_value){.i = to_linux_errno(errno)};

  fill_attrs(args[1].handle->obj, &st);
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, stat0, "(JLsun/nio/fs/UnixFileAttributes;)I") {
  return stat_impl(args, true);
}

// stat0 と違い lstat0 は失敗を UnixException で返す（JDK の宣言が throws UnixException）。
// 黙って戻ると、無いパスが「属性がすべて 0 のファイル」に見える
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, lstat0, "(JLsun/nio/fs/UnixFileAttributes;)V") {
  // lstat はリンクをたどらない（以前は stat を呼んでいて、リンクが指す先の属性を返していた）
  stack_value result = stat_impl(args, false);
  if (result.i != 0)
    thread->current_exception = create_unix_exception_from_linux(thread, result.i);
  return value_null();
}

// DIR *opendir(const char* dirname)
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, opendir0, "(J)J") {
  DIR *dir = opendir((char const *)args[0].l);
  if (!dir) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.l = (s64)dir};
}

// struct dirent* readdir(DIR *dirp), return  dirent->d_name
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, readdir0, "(J)[B") {
  struct dirent *entry = readdir((DIR *)args[0].l);
  if (!entry) {
    return value_null();
  }
  size_t len = strlen(entry->d_name);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), entry->d_name, len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, closedir, "(J)V") {
  closedir((DIR *)args[0].l);
  return value_null();
}

#ifdef __wasi__
// sun/nio/fs/UnixConstants in the bundled JDK was generated on Linux, so the oflags that reach open0
// are Linux's. wasi-libc encodes them completely differently -- O_RDONLY is 0x04000000 there, not 0 --
// so passing them through unchanged makes every open() fail with EINVAL. Notably that includes
// lib/modules, which is how the boot layer fails to initialize.
enum {
  LINUX_O_ACCMODE = 03,
  LINUX_O_WRONLY = 01,
  LINUX_O_RDWR = 02,
  LINUX_O_CREAT = 0100,
  LINUX_O_EXCL = 0200,
  LINUX_O_TRUNC = 01000,
  LINUX_O_APPEND = 02000,
  LINUX_O_NONBLOCK = 04000,
  LINUX_O_DSYNC = 010000,
  LINUX_O_DIRECTORY = 0200000,
  LINUX_O_NOFOLLOW = 0400000,
  LINUX_O_SYNC = 04010000,
};

static int translate_oflags(int linux_flags) {
  int accmode = linux_flags & LINUX_O_ACCMODE;
  int flags = accmode == LINUX_O_WRONLY ? O_WRONLY : accmode == LINUX_O_RDWR ? O_RDWR : O_RDONLY;

  // O_SYNC contains the O_DSYNC bit on Linux, so it has to be tested as a whole first.
  if ((linux_flags & LINUX_O_SYNC) == LINUX_O_SYNC)
    flags |= O_SYNC;
  else if (linux_flags & LINUX_O_DSYNC)
    flags |= O_DSYNC;

  if (linux_flags & LINUX_O_CREAT)
    flags |= O_CREAT;
  if (linux_flags & LINUX_O_EXCL)
    flags |= O_EXCL;
  if (linux_flags & LINUX_O_TRUNC)
    flags |= O_TRUNC;
  if (linux_flags & LINUX_O_APPEND)
    flags |= O_APPEND;
  if (linux_flags & LINUX_O_NONBLOCK)
    flags |= O_NONBLOCK;
  if (linux_flags & LINUX_O_DIRECTORY)
    flags |= O_DIRECTORY;
  if (linux_flags & LINUX_O_NOFOLLOW)
    flags |= O_NOFOLLOW;

  return flags;
}
#else
static int translate_oflags(int flags) { return flags; }
#endif

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, open0, "(JII)I") {
  int result = open((char const *)args[0].l, translate_oflags(args[1].i), args[2].i);
  if (result >= 0)
    return (stack_value){.i = result};

  thread->current_exception = create_unix_exception(thread, errno);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, mkdir0, "(JI)V") {
  int result = mkdir((char const *)args[0].l, args[1].i);
  if (result != 0)
    thread->current_exception = create_unix_exception(thread, errno);
  return value_null();
}

// Path.toRealPath。kotlinc はソースや jar のパスをこれで正規化する
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, realpath0, "(J)[B") {
  char resolved[PATH_MAX];
  if (!realpath((char const *)args[0].l, resolved)) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  size_t len = strlen(resolved);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, (int)len);
  if (!array)
    return value_null();
  memcpy(ArrayData(array), resolved, len);
  return (stack_value){.obj = array};
}

// JDK 23 での名前は rmdir0（rmdir のままだと Files.delete でディレクトリを消せない）
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, rmdir0, "(J)V") {
  int result = rmdir((char const *)args[0].l);
  if (result != 0)
    thread->current_exception = create_unix_exception(thread, errno);
  return value_null();
}

// JDK 23 の access0 は errno を戻り値で返す（例外は投げない）。投げると呼び出し側に受け止める口が
// 無く、Files.createDirectories が親をたどる途中で生の UnixException が漏れていた
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, access0, "(JI)I") {
  int result = access((char const *)args[0].l, args[1].i);
  return (stack_value){.i = result == 0 ? 0 : to_linux_errno(errno)};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, size0, "(Ljava/io/FileDescriptor;)J") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  struct stat st;
  int result = fstat(fd, &st);
  if (result) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.l = st.st_size};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, allocationGranularity0, "()J") {
#ifdef __wasi__
  // wasi-libc emulates mmap with malloc, so mappings are only malloc-aligned. FileChannelImpl asserts
  // that the address map0 returns is a multiple of this granularity, and it also uses it to round map
  // offsets down. Reporting 1 tells the truth about an allocator-backed mapping: there are no pages to
  // align to. Reporting 4096 would make the JDK assert on the very first mapping (lib/modules).
  return (stack_value){.l = 1};
#else
  return (stack_value){.l = 4096};
#endif
}

// (fd: FileDescriptor, prot:Int, pos: Long, len: Long, isSync: Boolean) -> Long
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, map0, "(Ljava/io/FileDescriptor;IJJZ)J") {
  DCHECK(args[0].handle->obj);
  int fd = LoadFieldInt(args[0].handle->obj, "fd");
  int prot = args[1].i;
  off_t pos = args[2].l;
  off_t len = args[3].l;
  int flags = args[4].i ? MAP_SHARED : MAP_PRIVATE;
  void *result = mmap(nullptr, len, prot | PROT_READ, flags, fd, pos);
  if (result == MAP_FAILED) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  arrput(thread->vm->mmap_allocations, ((mmap_allocation){result, len}));
  return (stack_value){.l = (s64)result};
}

// JDK 23 では int を返す（0 で成功）。(JJ)V のままだと UnsatisfiedLinkError になり、
// MappedByteBuffer を解放する Cleaner のスレッドごと落ちる
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, unmap0, "(JJ)I") {
  void *addr = (void *)args[0].l;
  size_t len = args[1].l;
  for (size_t i = 0; i < arrlenu(thread->vm->mmap_allocations); ++i) {
    if (thread->vm->mmap_allocations[i].ptr == addr) {
      arrdelswap(thread->vm->mmap_allocations, i);
      break;
    }
  }
  int result = munmap(addr, len);
  if (result) {
    thread->current_exception = create_unix_exception(thread, errno);
  }
  return (stack_value){.i = 0};
}

// Ordinary (non-mapped) file I/O for FileChannel. The JDK reaches these whenever it goes through
// java.nio -- Files.newInputStream, FileChannel.read, and everything layered on them. The buffer is a
// raw address into the VM's linear memory, handed to us by DirectByteBuffer, so we read and write it
// in place.

// Extract the int fd out of a java.io.FileDescriptor.
static int unwrap_fd(obj_header *file_descriptor) {
  DCHECK(file_descriptor);
  return LoadFieldInt(file_descriptor, "fd");
}

// The JDK signals end of file with IOStatus.EOF rather than 0.
#define IOSTATUS_EOF (-1)

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, read0, "(Ljava/io/FileDescriptor;JI)I") {
  ssize_t n = read(unwrap_fd(args[0].handle->obj), (void *)(uintptr_t)args[1].l, (size_t)args[2].i);
  if (n < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = n == 0 ? IOSTATUS_EOF : (s32)n};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, pread0, "(Ljava/io/FileDescriptor;JIJ)I") {
  ssize_t n = pread(unwrap_fd(args[0].handle->obj), (void *)(uintptr_t)args[1].l, (size_t)args[2].i,
                    (off_t)args[3].l);
  if (n < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = n == 0 ? IOSTATUS_EOF : (s32)n};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, write0, "(Ljava/io/FileDescriptor;JI)I") {
  ssize_t n = write(unwrap_fd(args[0].handle->obj), (void *)(uintptr_t)args[1].l, (size_t)args[2].i);
  if (n < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)n};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, pwrite0, "(Ljava/io/FileDescriptor;JIJ)I") {
  ssize_t n = pwrite(unwrap_fd(args[0].handle->obj), (void *)(uintptr_t)args[1].l, (size_t)args[2].i,
                     (off_t)args[3].l);
  if (n < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = (s32)n};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, seek0, "(Ljava/io/FileDescriptor;J)J") {
  int fd = unwrap_fd(args[0].handle->obj);
  // A negative offset means "tell me where we are" rather than "seek there".
  off_t result = args[1].l < 0 ? lseek(fd, 0, SEEK_CUR) : lseek(fd, (off_t)args[1].l, SEEK_SET);
  if (result < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.l = result};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, truncate0, "(Ljava/io/FileDescriptor;J)I") {
  if (ftruncate(unwrap_fd(args[0].handle->obj), (off_t)args[1].l) < 0) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, force0, "(Ljava/io/FileDescriptor;Z)I") {
  // Nothing is buffered outside the host's filesystem, so there is nothing to flush.
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, close0, "(Ljava/io/FileDescriptor;)V") {
  int fd = unwrap_fd(args[0].handle->obj);
  if (fd != -1)
    close(fd);
  return value_null();
}

// 実機（OpenJDK の UnixFileDispatcherImpl.c）と同じ規則にする。chr/fifo/sock は
// FIONREAD で滞留バイト数、通常ファイルは size - position、どちらも取れなければ 0。
// WASI に FIONREAD 相当は無いので、非シーク可能な fd は lseek が失敗して 0 に落ちる
DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, available0, "(Ljava/io/FileDescriptor;)I") {
  int fd = unwrap_fd(args[0].handle->obj);
  struct stat fbuf;
  s64 size = -1;

  if (fstat(fd, &fbuf) != -1) {
#ifndef __wasi__
    int mode = fbuf.st_mode;
    if (S_ISCHR(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
      int n = 0;
      if (ioctl(fd, FIONREAD, &n) >= 0)
        return (stack_value){.i = n};
    } else
#endif
    if (S_ISREG(fbuf.st_mode)) {
      size = fbuf.st_size;
    }
  }

  off_t position = lseek(fd, 0, SEEK_CUR);
  if (position == -1)
    return (stack_value){.i = 0};

  if (size < position) {
    // サイズ不明、あるいは開いたあと縮んだ fd では末端を測ってから元の位置に戻す
    off_t end = lseek(fd, 0, SEEK_END);
    if (end == -1 || lseek(fd, position, SEEK_SET) == -1)
      return (stack_value){.i = 0};
    size = end;
  }

  s64 available = size - position;
  return (stack_value){.i = available > INT32_MAX ? INT32_MAX : (s32)available};
}

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, isOther0, "(Ljava/io/FileDescriptor;)Z") {
  int fd = unwrap_fd(args[0].handle->obj);
  struct stat fbuf;
  if (fstat(fd, &fbuf) == -1) {
    thread->current_exception = create_unix_exception(thread, errno);
    return value_null();
  }
  int mode = fbuf.st_mode;
  return (stack_value){.i = (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)) ? 0 : 1};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, strerror, "(I)[B") {
  char *msg = strerror(from_linux_errno(args[0].i));
  size_t len = strlen(msg);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), msg, len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, init0, "()V") { return value_null(); }
// ---------------------------------------------------------------------------------------------
// JDK 23 の UnixNativeDispatcher が宣言していて、まだ無かったもの。Files.delete / move /
// newDirectoryStream / readSymbolicLink / setPosixFilePermissions、jdk.zipfs の書き出し
// （一時ファイルを作って rename で差し替える）などが通る。

#define THROW_ERRNO()                                                                                                  \
  do {                                                                                                                 \
    thread->current_exception = create_unix_exception(thread, errno);                                                  \
    return value_null();                                                                                               \
  } while (0)

// UnixConstants（Linux）の AT_* を wasi-libc の値へ
static int translate_at_flags(int linux_flags) {
  int flags = 0;
  if (linux_flags & 0x100)
    flags |= AT_SYMLINK_NOFOLLOW;
  if (linux_flags & 0x200)
    flags |= AT_REMOVEDIR;
  return flags;
}

static stack_value byte_array_of(vm_thread *thread, const char *chars, size_t len) {
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, (int)len);
  if (!array)
    return value_null();
  memcpy(ArrayData(array), chars, len);
  return (stack_value){.obj = array};
}

// WASI には fd を複製する仕組みが無い。init が openat 系の能力を申告しないので、JDK の
// SecureDirectoryStream（ここを呼ぶ）は選ばれない
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, dup, "(I)I") {
  errno = ENOTSUP;
  THROW_ERRNO();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, openat0, "(IJII)I") {
  int fd = openat(args[0].i, (char const *)args[1].l, translate_oflags(args[2].i), args[3].i);
  if (fd < 0)
    THROW_ERRNO();
  return (stack_value){.i = fd};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, close0, "(I)V") {
  // 本物と同じく EINTR 以外の失敗も握り潰さない
  if (close(args[0].i) != 0 && errno != EINTR)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, read0, "(IJI)I") {
  ssize_t n = read(args[0].i, (void *)args[1].l, args[2].i);
  if (n < 0)
    THROW_ERRNO();
  return (stack_value){.i = (s32)n};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, write0, "(IJI)I") {
  ssize_t n = write(args[0].i, (void const *)args[1].l, args[2].i);
  if (n < 0)
    THROW_ERRNO();
  return (stack_value){.i = (s32)n};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, link0, "(JJ)V") {
  if (link((char const *)args[0].l, (char const *)args[1].l) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, unlink0, "(J)V") {
  if (unlink((char const *)args[0].l) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, unlinkat0, "(IJI)V") {
  if (unlinkat(args[0].i, (char const *)args[1].l, translate_at_flags(args[2].i)) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, rename0, "(JJ)V") {
  if (rename((char const *)args[0].l, (char const *)args[1].l) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, renameat0, "(IJIJ)V") {
  if (renameat(args[0].i, (char const *)args[1].l, args[2].i, (char const *)args[3].l) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, readlink0, "(J)[B") {
  char target[PATH_MAX];
  ssize_t n = readlink((char const *)args[0].l, target, sizeof(target));
  if (n < 0)
    THROW_ERRNO();
  return byte_array_of(thread, target, (size_t)n);
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, symlink0, "(JJ)V") {
  if (symlink((char const *)args[0].l, (char const *)args[1].l) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fstat0, "(ILsun/nio/fs/UnixFileAttributes;)V") {
  struct stat st;
  if (fstat(args[0].i, &st) != 0)
    THROW_ERRNO();
  fill_attrs(args[1].handle->obj, &st);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fstatat0, "(IJILsun/nio/fs/UnixFileAttributes;)V") {
  struct stat st;
  if (fstatat(args[0].i, (char const *)args[1].l, &st, translate_at_flags(args[2].i)) != 0)
    THROW_ERRNO();
  fill_attrs(args[3].handle->obj, &st);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fdopendir, "(I)J") {
  DIR *dir = fdopendir(args[0].i);
  if (!dir)
    THROW_ERRNO();
  return (stack_value){.l = (s64)dir};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, rewind, "(J)V") {
  rewinddir((DIR *)args[0].l);
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getlinelen, "(J)I") {
  // /etc/mtab を読む Linux の FileStore 用。ここに mtab は無いので、いつも終わり
  return (stack_value){.i = -1};
}

// WASI には所有者も権限ビットも無い。chmod / chown は「変えた」ことにし、時刻だけは本当に変える
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, chmod0, "(JI)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fchmod0, "(II)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, chown0, "(JII)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, lchown0, "(JII)V") { return value_null(); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fchown0, "(III)V") { return value_null(); }

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, mknod0, "(JIJ)V") {
  errno = ENOTSUP;
  THROW_ERRNO();
}

// 時刻は utimes 系がマイクロ秒、futimens がナノ秒で来る
static int set_times(int dirfd, char const *path, int fd, s64 atime, s64 mtime, bool nanos, int at_flags) {
  struct timespec times[2];
  s64 div = nanos ? 1000000000 : 1000000, mul = nanos ? 1 : 1000;
  times[0] = (struct timespec){.tv_sec = atime / div, .tv_nsec = (atime % div) * mul};
  times[1] = (struct timespec){.tv_sec = mtime / div, .tv_nsec = (mtime % div) * mul};
  return path ? utimensat(dirfd, path, times, at_flags) : futimens(fd, times);
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, utimes0, "(JJJ)V") {
  if (set_times(AT_FDCWD, (char const *)args[0].l, -1, args[1].l, args[2].l, false, 0) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, lutimes0, "(JJJ)V") {
  if (set_times(AT_FDCWD, (char const *)args[0].l, -1, args[1].l, args[2].l, false, AT_SYMLINK_NOFOLLOW) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, futimes0, "(IJJ)V") {
  if (set_times(0, nullptr, args[0].i, args[1].l, args[2].l, false, 0) != 0)
    THROW_ERRNO();
  return value_null();
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, futimens0, "(IJJ)V") {
  if (set_times(0, nullptr, args[0].i, args[1].l, args[2].l, true, 0) != 0)
    THROW_ERRNO();
  return value_null();
}

// 利用者とグループは 1 つずつしか無い（uid / gid は stat が返す 0）
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getpwuid, "(I)[B") { return byte_array_of(thread, "user", 4); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getgrgid, "(I)[B") { return byte_array_of(thread, "user", 4); }
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getpwnam0, "(J)I") {
  return (stack_value){.i = strcmp((char const *)args[0].l, "user") == 0 ? 0 : -1};
}
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, getgrnam0, "(J)I") {
  return (stack_value){.i = strcmp((char const *)args[0].l, "user") == 0 ? 0 : -1};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, statvfs0, "(JLsun/nio/fs/UnixFileStoreAttributes;)V") {
  errno = ENOTSUP;
  THROW_ERRNO();
}

// 拡張属性は無い（UserDefinedFileAttributeView は ENOTSUP で「非対応」と分かる）
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fgetxattr0, "(IJJI)I") {
  errno = ENOTSUP;
  THROW_ERRNO();
}
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fsetxattr0, "(IJJI)V") {
  errno = ENOTSUP;
  THROW_ERRNO();
}
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, fremovexattr0, "(IJ)V") {
  errno = ENOTSUP;
  THROW_ERRNO();
}
DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, flistxattr, "(IJI)I") {
  errno = ENOTSUP;
  THROW_ERRNO();
}

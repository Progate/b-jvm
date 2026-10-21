
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

static obj_header *create_unix_exception(vm_thread *thread, int errno_code) {
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

stack_value stat_impl(value *args) {
  struct stat st;

  if (!args[1].handle)
    return value_null();

  uintptr_t buf = args[0].l;
  int result = stat((char *)buf, &st);
  if (result)
    return (stack_value){.i = errno};

  obj_header *attrs = args[1].handle->obj;

#define MapAttrLong(name, value) StoreFieldLong(attrs, (#name), value)
#define MapAttrInt(name, value) StoreFieldInt(attrs, (#name), value)
  MapAttrInt(st_mode, st.st_mode);
  MapAttrLong(st_ino, st.st_ino);
  MapAttrLong(st_dev, st.st_dev);
  MapAttrLong(st_rdev, st.st_rdev);
  MapAttrInt(st_nlink, st.st_nlink);
  MapAttrInt(st_uid, st.st_uid);
  MapAttrInt(st_gid, st.st_gid);
  MapAttrLong(st_size, st.st_size);

#ifdef __APPLE__
  MapAttrLong(st_atime_sec, st.st_atime);
  MapAttrLong(st_atime_nsec, st.st_atimensec);

  MapAttrLong(st_mtime_sec, st.st_mtime);
  MapAttrLong(st_mtime_nsec, st.st_mtimensec);

  MapAttrLong(st_ctime_sec, st.st_ctime);
  MapAttrLong(st_ctime_nsec, st.st_ctimensec);
#else
  MapAttrLong(st_atime_sec, st.st_atim.tv_sec);
  MapAttrLong(st_atime_nsec, st.st_atim.tv_nsec);

  MapAttrLong(st_mtime_sec, st.st_mtim.tv_sec);
  MapAttrLong(st_mtime_nsec, st.st_mtim.tv_nsec);

  MapAttrLong(st_ctime_sec, st.st_ctim.tv_sec);
  MapAttrLong(st_ctime_nsec, st.st_ctim.tv_nsec);
#endif

#ifdef __APPLE__
  MapAttrLong(st_birthtime_sec, st.st_birthtime);
  MapAttrLong(st_birthtime_nsec, st.st_birthtimensec);
#endif

#undef MapAttrLong
#undef MapAttrInt

  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, stat0, "(JLsun/nio/fs/UnixFileAttributes;)I") {
  return stat_impl(args);
}

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, lstat0, "(JLsun/nio/fs/UnixFileAttributes;)V") {
  stat_impl(args);
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

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, access0, "(JI)I") {
  int result = access((char const *)args[0].l, args[1].i);
  if (result == 0)
    return (stack_value){.i = 0};

  thread->current_exception = create_unix_exception(thread, errno);
  return value_null();
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

DECLARE_NATIVE("sun/nio/ch", UnixFileDispatcherImpl, unmap0, "(JJ)V") {
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
  return value_null();
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

DECLARE_NATIVE("sun/nio/fs", UnixNativeDispatcher, strerror, "(I)[B") {
  char *msg = strerror(args[0].i);
  size_t len = strlen(msg);
  obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, len);
  if (!array) {
    return value_null();
  }
  memcpy(ArrayData(array), msg, len);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("sun/nio/ch", FileDispatcherImpl, init0, "()V") { return value_null(); }
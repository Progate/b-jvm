#include <errno.h>
#include <natives-dsl.h>
#ifdef EMSCRIPTEN
#include <fcntl.h>
#else
#include <sys/fcntl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

// RandomAccessFile は FileDescriptor.fd（整数の fd）を持つ。FileInputStream と同じ形で、
// getChannel() が返す FileChannel（size / read / map）も同じ fd を使うので、ここで
// FILE* などの別の口を持つと、チャネル側からは「Bad file descriptor」になる
// （kotlinc は RandomAccessFile#getChannel().map() で jar を読む）

// java.io.RandomAccessFile の mode のビット
enum { RAF_O_RDONLY = 1, RAF_O_RDWR = 2, RAF_O_SYNC = 4, RAF_O_DSYNC = 8 };

DECLARE_NATIVE("java/io", RandomAccessFile, initIDs, "()V") { return value_null(); }

static s32 raf_fd(obj_header *raf) {
  obj_header *fd = LoadFieldObject(raf, "java/io/FileDescriptor", "fd");
  return fd ? LoadFieldInt(fd, "fd") : -1;
}

static stack_value raise_io(vm_thread *thread, const char *what) {
  INIT_STACK_STRING(msg, 256);
  msg = bprintf(msg, "%s: %s", what, strerror(errno));
  raise_vm_exception(thread, STR("java/io/IOException"), msg);
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, open0, "(Ljava/lang/String;I)V") {
  if (!args[0].handle->obj)
    return value_null();
  heap_string filename = AsHeapString(args[0].handle->obj, on_oom);

  int mode = args[1].i;
  int flags = (mode & RAF_O_RDWR) ? O_RDWR | O_CREAT : O_RDONLY;
  if (mode & RAF_O_SYNC)
    flags |= O_SYNC;
  else if (mode & RAF_O_DSYNC)
    flags |= O_DSYNC;

  s32 unix_fd = open(filename.chars, flags, 0666);
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  StoreFieldInt(fd, "fd", unix_fd);
  if (unix_fd < 0) {
    // 本物と同じく「パス (理由)」の形にする
    INIT_STACK_STRING(msg, 1024);
    msg = bprintf(msg, "%.*s (%s)", fmt_slice(filename), strerror(errno));
    raise_vm_exception(thread, STR("java/io/FileNotFoundException"), msg);
  }
  free_heap_str(filename);

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, read0, "()I") {
  u8 byte;
  ssize_t n = read(raf_fd(obj->obj), &byte, 1);
  if (n < 0)
    return raise_io(thread, "read");
  return (stack_value){.i = n == 0 ? -1 : byte};
}

DECLARE_NATIVE("java/io", RandomAccessFile, readBytes0, "([BII)I") {
  object buf = args[0].handle->obj;
  s32 off = args[1].i, len = args[2].i;
  if (len == 0)
    return (stack_value){.i = 0};
  ssize_t n = read(raf_fd(obj->obj), (char *)ArrayData(buf) + off, len);
  if (n < 0)
    return raise_io(thread, "read");
  return (stack_value){.i = n == 0 ? -1 : (s32)n};
}

DECLARE_NATIVE("java/io", RandomAccessFile, write0, "(I)V") {
  u8 byte = (u8)args[0].i;
  if (write(raf_fd(obj->obj), &byte, 1) < 0)
    return raise_io(thread, "write");
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, writeBytes0, "([BII)V") {
  object buf = args[0].handle->obj;
  s32 off = args[1].i, len = args[2].i;
  const char *p = (const char *)ArrayData(buf) + off;
  while (len > 0) {
    ssize_t n = write(raf_fd(obj->obj), p, len);
    if (n < 0)
      return raise_io(thread, "write");
    p += n;
    len -= (s32)n;
  }
  return value_null();
}

DECLARE_NATIVE("java/io", RandomAccessFile, getFilePointer, "()J") {
  off_t pos = lseek(raf_fd(obj->obj), 0, SEEK_CUR);
  if (pos < 0)
    return raise_io(thread, "getFilePointer");
  return (stack_value){.l = pos};
}

DECLARE_NATIVE("java/io", RandomAccessFile, seek0, "(J)V") {
  if (lseek(raf_fd(obj->obj), (off_t)args[0].l, SEEK_SET) < 0)
    return raise_io(thread, "seek");
  return value_null();
}

// 位置は動かさない（以前は fseek(SEEK_END) で末尾へ動いていた）
DECLARE_NATIVE("java/io", RandomAccessFile, length0, "()J") {
  struct stat st;
  if (fstat(raf_fd(obj->obj), &st) < 0)
    return raise_io(thread, "length");
  return (stack_value){.l = st.st_size};
}

DECLARE_NATIVE("java/io", RandomAccessFile, setLength0, "(J)V") {
  s32 fd = raf_fd(obj->obj);
  off_t pos = lseek(fd, 0, SEEK_CUR);
  if (ftruncate(fd, (off_t)args[0].l) < 0)
    return raise_io(thread, "setLength");
  // 本物と同じく、位置が新しい長さを超えていれば末尾へ寄せる
  if (pos > args[0].l)
    lseek(fd, (off_t)args[0].l, SEEK_SET);
  return value_null();
}

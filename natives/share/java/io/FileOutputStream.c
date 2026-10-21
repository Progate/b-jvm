#include <errno.h>
#include <natives-dsl.h>
#ifdef EMSCRIPTEN
#include <fcntl.h>
#else
#include <sys/fcntl.h>
#endif
#include <unistd.h>

DECLARE_NATIVE("java/io", FileOutputStream, initIDs, "()V") { return value_null(); }

// new FileOutputStream(path[, append])。kotlinc が .class を書き出すのはここを通る
DECLARE_NATIVE("java/io", FileOutputStream, open0, "(Ljava/lang/String;Z)V") {
  if (!args[0].handle->obj)
    return value_null();
  heap_string filename = AsHeapString(args[0].handle->obj, on_oom);

  int flags = O_WRONLY | O_CREAT | (args[1].i ? O_APPEND : O_TRUNC);
  s32 unix_fd = open(filename.chars, flags, 0666);
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);
  StoreFieldInt(fd, "fd", unix_fd);
  if (unix_fd < 0) {
    INIT_STACK_STRING(msg, 1024);
    msg = bprintf(msg, "%.*s (%s)", fmt_slice(filename), strerror(errno));
    raise_vm_exception(thread, STR("java/io/FileNotFoundException"), msg);
  }
  free_heap_str(filename);

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/io", FileOutputStream, writeBytes, "([BIIZ)V") {
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  s32 unix_fd = LoadFieldInt(fd, "fd");

  obj_header *bytes = args[0].handle->obj;
  s32 offset = args[1].i;
  s32 length = args[2].i;
  [[maybe_unused]] bool append = args[3].i; // todo: append to first advance the position to the end of file
  char *data = (char *)ArrayData(bytes);

  if (offset < 0 || length < 0 || (long)offset + length > ArrayLength(bytes)) {
    raise_vm_exception_no_msg(thread, STR("java/lang/ArrayIndexOutOfBoundsException"));
    return value_null();
  }

  char *buf = data + offset;

  if (unix_fd == 1 && thread->vm->write_stdout) {
    thread->vm->write_stdout(buf, length, thread->vm->stdio_override_param);
  } else if (unix_fd == 2 && thread->vm->write_stderr) {
    thread->vm->write_stderr(buf, length, thread->vm->stdio_override_param);
  } else {              // do an actual syscall
    if (unix_fd == 2) { // for some reason stderr needs this??
      fprintf(stderr, "%.*s", length, buf);
    } else {
      while (length > 0) {
        s32 written = (s32)write(unix_fd, buf, length);

        if (written < 0) {
          raise_vm_exception(thread, STR("java/io/IOException"), STR("Error writing file"));
          return value_null();
        }

        length -= written;
        buf += written;
      }
    }
  }
  return value_null();
}

DECLARE_NATIVE("java/io", FileOutputStream, close0, "()V") {
  // this method does no allocations or yielding, so we can use the same pointer
  obj_header *fd = LoadFieldObject(obj->obj, "java/io/FileDescriptor", "fd");
  DCHECK(fd);

  s32 unix_fd = LoadFieldInt(fd, "fd");
  if (unix_fd != -1)
    close(unix_fd);
  StoreFieldInt(fd, "fd", -1);

  return value_null();
}

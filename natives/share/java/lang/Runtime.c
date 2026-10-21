#include <natives-dsl.h>

DECLARE_NATIVE("java/lang", Runtime, availableProcessors, "()I") { return (stack_value){.i = 1}; }

DECLARE_NATIVE("java/lang", Runtime, totalMemory, "()J") { return (stack_value){.l = (s64)thread->vm->heap_used}; }

DECLARE_NATIVE("java/lang", Runtime, freeMemory, "()J") { return (stack_value){.l = (s64)(thread->vm->heap_capacity - thread->vm->heap_used)}; }

DECLARE_NATIVE("java/lang", Runtime, maxMemory, "()J") { return (stack_value){.l = LLONG_MAX}; }

DECLARE_NATIVE("java/lang", Runtime, gc, "()V") {
  major_gc(thread->vm);
  return value_null();
}

DECLARE_NATIVE("java/lang", Shutdown, beforeHalt, "()V") { return value_null(); }

// We cannot actually halt -- there is no way to unwind the interpreter from here -- but recording the
// status lets the embedder report the exit code of a main() that ends in System.exit, which is how
// batch tools such as a compiler signal failure.
DECLARE_NATIVE("java/lang", Shutdown, halt0, "(I)V") {
  // Truncate to 8 bits like a process exit status would be. Besides being faithful, it keeps -1
  // usable as "System.exit was not called": System.exit(-1) is a common way for a batch tool to
  // report failure, and it becomes 255 here just as it would for the java launcher.
  thread->vm->exit_status = args[0].i & 0xff;
  return value_null();
}
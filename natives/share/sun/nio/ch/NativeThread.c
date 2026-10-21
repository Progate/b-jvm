#include <natives-dsl.h>

DECLARE_NATIVE("sun/nio/ch", NativeThread, init, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/ch", NativeThread, current0, "()J") { return (stack_value){.l = (s64)thread->tid}; }

DECLARE_NATIVE("sun/nio/ch", NativeThread, signal0, "(J)V") { return value_null(); }

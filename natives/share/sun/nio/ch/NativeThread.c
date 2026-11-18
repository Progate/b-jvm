#include <natives-dsl.h>

DECLARE_NATIVE("sun/nio/ch", NativeThread, init, "()V") { return value_null(); }

DECLARE_NATIVE("sun/nio/ch", NativeThread, current0, "()J") { return (stack_value){.l = (s64)thread->tid}; }

DECLARE_NATIVE("sun/nio/ch", NativeThread, signal0, "(J)V") { return value_null(); }

// signal0 cannot interrupt a thread blocked in a syscall here, so tell the JDK not to rely on
// signals to wake such threads when a channel is closed (it then waits for the I/O to return)
DECLARE_NATIVE("sun/nio/ch", NativeThread, supportPendingSignals0, "()Z") { return (stack_value){.i = 0}; }

#include <arrays.h>
#include <natives-dsl.h>

DECLARE_NATIVE("java/lang", ProcessEnvironment, environ, "()[[B") {
  // Create empty array of byte arrays
  obj_header *array = CreateObjectArray1D(thread, bootstrap_lookup_class(thread, STR("[B")), 0);
  return (stack_value){.obj = array};
}

DECLARE_NATIVE("java/lang", ProcessImpl, init, "()V") { return value_null(); }

// private native int forkAndExec(int mode, byte[] helperpath,
// byte[] prog,
// byte[] argBlock, int argc,
// byte[] envBlock, int envc,
// byte[] dir,
// int[] fds,
// boolean redirectErrorStream)
DECLARE_NATIVE("java/lang", ProcessImpl, forkAndExec, "(I[B[B[BI[BI[B[IZ)I") {
  // Spawning processes is not supported; pretend the fork failed without noisily warning
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("java/lang", ProcessHandleImpl, initNative, "()V") { return value_null(); }

DECLARE_NATIVE("java/lang", ProcessHandleImpl, getCurrentPid0, "()J") { return (stack_value){.l = 1}; }

DECLARE_NATIVE("java/lang", ProcessHandleImpl, isAlive0, "(J)J") { return (stack_value){.i = 0}; }

DECLARE_NATIVE("java/lang", ProcessHandleImpl, destroy0, "(JJZ)Z") { return (stack_value){.i = 1}; }

DECLARE_NATIVE("java/lang", ProcessHandleImpl, waitForProcessExit0, "(JZ)I") { return (stack_value){.i = 0}; }
// A baseline JIT that compiles hot methods to wasm functions (wasm32 Linux builds only). See jit.c.
#pragma once

#include "bjvm.h"

typedef enum {
  // The method returned. The result (if any) is in frame->stack[0]
  JIT_STATUS_RETURN = 0,
  // An exception was raised at frame->program_counter (thread->current_exception is set)
  JIT_STATUS_EXCEPTION = 1,
  // Continue in the interpreter at frame->program_counter. All locals and stack values are in the frame
  JIT_STATUS_BAIL = 2,
  // As JIT_STATUS_BAIL, at an instruction that was unresolved when the method was compiled
  JIT_STATUS_BAIL_UNRESOLVED = 3,
} jit_status;

// Runs the compiled code of the frame's method from frame->program_counter, compiling the method first once it is hot.
// Returns -1 when there is no compiled code to run (the interpreter carries on as usual), otherwise a jit_status.
int jit_try_run(vm_thread *thread, stack_frame *frame);

typedef enum {
  JIT_INVOKE_DONE = 0,      // the call completed and its result (if any) is on the caller's stack
  JIT_INVOKE_EXCEPTION = 1, // an exception was raised at the invoke (thread->current_exception)
  JIT_INVOKE_PENDING = 2,   // the callee's frame was pushed and the driver must continue it (it returns to pc + 1)
  JIT_INVOKE_INTERPRET = 3, // the interpreter should execute the invoke instruction itself
} jit_invoke_result;

// The invoke at pc of an interpreter frame, for compiled code (defined in interpreter2.c next to the interpreter's
// invoke handlers). Runs intrinsics inline and calls compiled callees directly.
int jit_rt_invoke(vm_thread *thread, stack_frame *frame, int pc);

// Compiled calls nested inside compiled code (on the host's stack), and the most allowed: deeper calls go through
// the driver instead
extern int jit_nesting;
#define JIT_MAX_NESTING 64

// Loads a module of `count` compiled methods (see jit_load.c) and writes the table index of each exported function
// `f<i>` to indices[i]. Returns false when the module could not be loaded
bool jit_load(const u8 *bytes, u32 len, int count, s32 *indices);

// When BJVM_JIT_STATS is set, prints to stderr how many compiled methods were loaded and how many failed to load
void jit_report_stats(void);

// The interpreter stopped the frame for a fuel check (it ran a long loop): compile its method right away
void jit_note_hot_loop(stack_frame *frame);

// Call-site intrinsics (cp_method.call_intrinsic, see find_call_intrinsic in interpreter2.c): small library methods
// that the interpreter, and compiled code, run inline instead of pushing a frame
enum {
  CALL_INTRINSIC_NONE,
  CALL_INTRINSIC_CHECK_NOT_NULL, // kotlin.jvm.internal.Intrinsics.checkNotNull* (Object[, String])V
  CALL_INTRINSIC_ARE_EQUAL,      // kotlin.jvm.internal.Intrinsics.areEqual(Object, Object)Z
  CALL_INTRINSIC_STRING_EQUALS,  // java.lang.String.equals(Object)Z
  CALL_INTRINSIC_GETTER,         // any `aload_0; getfield; xreturn` method, once its getfield has been resolved
  CALL_INTRINSIC_CHECK_INDEX,    // jdk.internal.util.Preconditions.checkIndex(IILBiFunction;)I
  CALL_INTRINSIC_ITR_HAS_NEXT,   // java.util.ArrayList$Itr.hasNext()Z
  CALL_INTRINSIC_ITR_CHECK_MOD,  // java.util.ArrayList$Itr.checkForComodification()V
};

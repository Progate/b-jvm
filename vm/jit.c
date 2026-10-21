// A baseline JIT: compiles a hot method to a wasm function and hands it to the host to instantiate.
//
// The compiled code works on the interpreter's own frame: every local and operand stack value lives in the frame
// memory, in the slots the interpreter uses, at every instruction boundary. That keeps three things simple:
//
// - Leaving compiled code is just storing the pc. Whatever the compiled code does not handle (invokes, monitors,
//   unresolved instructions...) returns JIT_STATUS_BAIL, and the interpreter continues the same frame from that pc.
//   The interpreter later re-enters the compiled code at any block leader (after a call returns, at an exception
//   handler, when resuming after a fuel check).
// - The moving GC: values are never kept in wasm locals across anything that allocates, so the GC relocates them in
//   the frame as it does for interpreted frames. Object addresses are never baked into the code.
// - Exceptions: the compiled code raises the exception, stores the pc and returns; the interpreter's driver looks
//   for a handler as usual.
//
// Control flow uses a dispatch loop: one block per basic block, entered through a br_table indexed by pc. Forward
// branches are direct `br`s; backward branches decrement the thread's fuel (as the interpreter's branches do) and go
// through the dispatch.
//
// The host side is a single import, bjvm_jit.install(bytes, length), which compiles the module (importing this
// module's memory and function table), appends its function to the table and returns the table index. Runtime
// helpers are called with call_indirect on their C function pointers, which are indices into the same table.

#include "jit.h"

#include "arrays.h"
#include "classfile.h"
#include "exceptions.h"
#include "objects.h"
#include "analysis.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__wasi__)
#define JIT_SUPPORTED 1
__attribute__((import_module("bjvm_jit"), import_name("install"))) int32_t jit_host_install(const u8 *bytes, u32 len);
#else
#define JIT_SUPPORTED 0
#endif

// Interpreter entries before a method is compiled (BJVM_JIT_THRESHOLD overrides it)
#define JIT_DEFAULT_THRESHOLD 1000
static u32 JIT_THRESHOLD = JIT_DEFAULT_THRESHOLD;
// Larger methods are left to the interpreter (the dispatch br_table has one entry per instruction)
#define JIT_MAX_INSNS 6000
// Bails at instructions that were unresolved at compile time before the method is recompiled, and the most
// recompiles per method
#define JIT_RECOMPILE_BAILS 64
#define JIT_MAX_COMPILES 3

typedef int (*jit_function)(vm_thread *thread, stack_frame *frame, int pc);

/* ------------------------------------------------------------------------------------------------------------------
 * Runtime helpers, called from compiled code. Each returns 0 / nullptr when it raised an exception.
 * ------------------------------------------------------------------------------------------------------------------ */

__attribute__((noinline, used)) static void jit_rt_npe(vm_thread *thread) { raise_null_pointer_exception(thread); }

__attribute__((noinline, used)) static void jit_rt_div0(vm_thread *thread) { raise_div0_arithmetic_exception(thread); }

__attribute__((noinline, used)) static void jit_rt_array_oob(vm_thread *thread, int index, int length) {
  raise_array_index_oob_exception(thread, index, length);
}

__attribute__((noinline, used)) static obj_header *jit_rt_new(vm_thread *thread, classdesc *cd) {
  return new_object(thread, cd);
}

__attribute__((noinline, used)) static obj_header *jit_rt_newarray(vm_thread *thread, int type, int count) {
  if (count < 0) {
    raise_negative_array_size_exception(thread, count);
    return nullptr;
  }
  return CreatePrimitiveArray1D(thread, (type_kind)type, count);
}

__attribute__((noinline, used)) static obj_header *jit_rt_anewarray(vm_thread *thread, classdesc *cd, int count) {
  if (count < 0) {
    raise_negative_array_size_exception(thread, count);
    return nullptr;
  }
  return CreateObjectArray1D(thread, cd, count);
}

// obj is non-null and not exactly of class cd
__attribute__((noinline, used)) static int jit_rt_instanceof(obj_header *obj, classdesc *cd) {
  return instanceof(obj->descriptor, cd);
}

__attribute__((noinline, used)) static int jit_rt_checkcast(vm_thread *thread, obj_header *obj, classdesc *cd) {
  if (instanceof(obj->descriptor, cd))
    return 1;
  raise_class_cast_exception(thread, obj->descriptor, cd);
  return 0;
}

// value is non-null and not exactly of the array's component class
__attribute__((noinline, used)) static int jit_rt_aastore_check(vm_thread *thread, obj_header *array,
                                                                obj_header *value) {
  if (instanceof(value->descriptor, array->descriptor->one_fewer_dim))
    return 1;
  raise_array_store_exception(thread, value->descriptor->name);
  return 0;
}

__attribute__((noinline, used)) static float jit_rt_frem(float a, float b) { return fmodf(a, b); }
__attribute__((noinline, used)) static double jit_rt_drem(double a, double b) { return fmod(a, b); }

#define HELPER(fn) ((s32)(uintptr_t)(void *)&(fn))

/* ------------------------------------------------------------------------------------------------------------------
 * Byte buffer and LEB128
 * ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  u8 *data;
  size_t len, cap;
} jbuf;

static void jb_byte(jbuf *b, u8 x) {
  if (b->len == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 1024;
    b->data = realloc(b->data, b->cap);
  }
  b->data[b->len++] = x;
}

static void jb_bytes(jbuf *b, const void *bytes, size_t n) {
  for (size_t i = 0; i < n; ++i)
    jb_byte(b, ((const u8 *)bytes)[i]);
}

static void jb_uleb(jbuf *b, u64 v) {
  do {
    u8 x = v & 0x7f;
    v >>= 7;
    if (v)
      x |= 0x80;
    jb_byte(b, x);
  } while (v);
}

static void jb_sleb(jbuf *b, s64 v) {
  while (true) {
    u8 x = v & 0x7f;
    v >>= 7;
    if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) {
      jb_byte(b, x);
      return;
    }
    jb_byte(b, x | 0x80);
  }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Wasm opcodes
 * ------------------------------------------------------------------------------------------------------------------ */

enum {
  OP_UNREACHABLE = 0x00,
  OP_BLOCK = 0x02,
  OP_LOOP = 0x03,
  OP_IF = 0x04,
  OP_ELSE = 0x05,
  OP_END = 0x0b,
  OP_BR = 0x0c,
  OP_BR_IF = 0x0d,
  OP_BR_TABLE = 0x0e,
  OP_RETURN = 0x0f,
  OP_CALL_INDIRECT = 0x11,
  OP_DROP = 0x1a,
  OP_SELECT = 0x1b,
  OP_LOCAL_GET = 0x20,
  OP_LOCAL_SET = 0x21,
  OP_LOCAL_TEE = 0x22,
  OP_I32_LOAD = 0x28,
  OP_I64_LOAD = 0x29,
  OP_F32_LOAD = 0x2a,
  OP_F64_LOAD = 0x2b,
  OP_I32_LOAD8_S = 0x2c,
  OP_I32_LOAD16_S = 0x2e,
  OP_I32_LOAD16_U = 0x2f,
  OP_I32_STORE = 0x36,
  OP_I64_STORE = 0x37,
  OP_F32_STORE = 0x38,
  OP_F64_STORE = 0x39,
  OP_I32_STORE8 = 0x3a,
  OP_I32_STORE16 = 0x3b,
  OP_I32_CONST = 0x41,
  OP_I64_CONST = 0x42,
  OP_F32_CONST = 0x43,
  OP_F64_CONST = 0x44,
  OP_I32_EQZ = 0x45,
  OP_I32_EQ = 0x46,
  OP_I32_NE = 0x47,
  OP_I32_LT_S = 0x48,
  OP_I32_GT_S = 0x4a,
  OP_I32_LE_S = 0x4c,
  OP_I32_GE_S = 0x4e,
  OP_I32_GE_U = 0x4f,
  OP_I64_EQ = 0x51,
  OP_I64_LT_S = 0x53,
  OP_I64_GT_S = 0x55,
  OP_F32_EQ = 0x5b,
  OP_F32_NE = 0x5c,
  OP_F32_LT = 0x5d,
  OP_F32_GT = 0x5e,
  OP_F64_EQ = 0x61,
  OP_F64_NE = 0x62,
  OP_F64_LT = 0x63,
  OP_F64_GT = 0x64,
  OP_I32_ADD = 0x6a,
  OP_I32_SUB = 0x6b,
  OP_I32_MUL = 0x6c,
  OP_I32_DIV_S = 0x6d,
  OP_I32_REM_S = 0x6f,
  OP_I32_AND = 0x71,
  OP_I32_OR = 0x72,
  OP_I32_XOR = 0x73,
  OP_I32_SHL = 0x74,
  OP_I32_SHR_S = 0x75,
  OP_I32_SHR_U = 0x76,
  OP_I64_ADD = 0x7c,
  OP_I64_SUB = 0x7d,
  OP_I64_MUL = 0x7e,
  OP_I64_DIV_S = 0x7f,
  OP_I64_REM_S = 0x81,
  OP_I64_AND = 0x83,
  OP_I64_OR = 0x84,
  OP_I64_XOR = 0x85,
  OP_I64_SHL = 0x86,
  OP_I64_SHR_S = 0x87,
  OP_I64_SHR_U = 0x88,
  OP_F32_NEG = 0x8c,
  OP_F32_ADD = 0x92,
  OP_F32_SUB = 0x93,
  OP_F32_MUL = 0x94,
  OP_F32_DIV = 0x95,
  OP_F64_NEG = 0x9a,
  OP_F64_ADD = 0xa0,
  OP_F64_SUB = 0xa1,
  OP_F64_MUL = 0xa2,
  OP_F64_DIV = 0xa3,
  OP_I32_WRAP_I64 = 0xa7,
  OP_I64_EXTEND_I32_S = 0xac,
  OP_I64_EXTEND_I32_U = 0xad,
  OP_F32_CONVERT_I32_S = 0xb2,
  OP_F32_CONVERT_I64_S = 0xb4,
  OP_F32_DEMOTE_F64 = 0xb6,
  OP_F64_CONVERT_I32_S = 0xb7,
  OP_F64_CONVERT_I64_S = 0xb9,
  OP_F64_PROMOTE_F32 = 0xbb,
  OP_I32_EXTEND8_S = 0xc0,
  OP_I32_EXTEND16_S = 0xc1,
  OP_PREFIX_FC = 0xfc,
};

enum { FC_I32_TRUNC_SAT_F32_S = 0, FC_I32_TRUNC_SAT_F64_S = 2, FC_I64_TRUNC_SAT_F32_S = 4, FC_I64_TRUNC_SAT_F64_S = 6 };

enum { VT_I32 = 0x7f, VT_I64 = 0x7e, VT_F32 = 0x7d, VT_F64 = 0x7c, BLOCK_VOID = 0x40 };

// Wasm locals of the compiled function. Parameters first.
enum {
  L_THREAD = 0,
  L_FRAME = 1,
  L_PC = 2,
  L_BASE = 3, // frame locals start: every frame slot is at a constant, positive offset from here
  L_NEXT = 4, // dispatch target pc
  L_A = 5,    // i32 temporaries
  L_B = 6,
  L_C = 7,
  L_D = 8,
  L_E = 9,
  L_X = 10, // i64 temporaries
  L_Y = 11,
  L_Z = 12,
  L_W = 13,
  L_COUNT_I32 = 7, // L_BASE .. L_E
  L_COUNT_I64 = 4, // L_X .. L_W
};

/* ------------------------------------------------------------------------------------------------------------------
 * Compilation context
 * ------------------------------------------------------------------------------------------------------------------ */

#define MAX_SIGS 16

typedef struct {
  cp_method *method;
  attribute_code *code;
  u16 *sd; // stack depth before each instruction
  int insn_count;

  int *block_of_pc; // block index if the pc starts a basic block, else -1
  int block_count;
  int current_block; // the block whose code is being emitted
  int nesting;       // blocks/ifs opened inside the current block's code

  u32 locals_count;
  u32 stack_offset; // offset of frame->stack[0] from L_BASE

  const char *sigs[MAX_SIGS];
  int sig_count;

  jbuf body;
  bool failed;
} jctx;

static void op(jctx *c, u8 x) { jb_byte(&c->body, x); }
static void op_u(jctx *c, u64 v) { jb_uleb(&c->body, v); }

static void local_get(jctx *c, int index) {
  op(c, OP_LOCAL_GET);
  op_u(c, index);
}

static void local_set(jctx *c, int index) {
  op(c, OP_LOCAL_SET);
  op_u(c, index);
}

static void i32_const(jctx *c, s32 v) {
  op(c, OP_I32_CONST);
  jb_sleb(&c->body, v);
}

static void i64_const(jctx *c, s64 v) {
  op(c, OP_I64_CONST);
  jb_sleb(&c->body, v);
}

// A memory access with the natural alignment hint (the hint is not a requirement; unaligned is fine)
static void mem(jctx *c, u8 opcode, u32 align_log2, u32 offset) {
  op(c, opcode);
  op_u(c, align_log2);
  op_u(c, offset);
}

static void fc(jctx *c, u8 sub) {
  op(c, OP_PREFIX_FC);
  op_u(c, sub);
}

// Index of a function type, e.g. "iii:i" (params : results; i = i32, l = i64, f = f32, d = f64)
static int sig_index(jctx *c, const char *sig) {
  for (int i = 0; i < c->sig_count; ++i)
    if (strcmp(c->sigs[i], sig) == 0)
      return i;
  CHECK(c->sig_count < MAX_SIGS);
  c->sigs[c->sig_count] = sig;
  return c->sig_count++;
}

// Calls a runtime helper (arguments already pushed)
static void call_helper(jctx *c, s32 fn, const char *sig) {
  i32_const(c, fn);
  op(c, OP_CALL_INDIRECT);
  op_u(c, sig_index(c, sig));
  op_u(c, 0); // table 0
}

/* Frame slots ----------------------------------------------------------------------------------------------------- */

static u32 local_offset(jctx *c, int index) { return 8 * index; }
static u32 stack_offset(jctx *c, int depth) { return c->stack_offset + 8 * depth; }

static void load_i32(jctx *c, u32 offset) {
  local_get(c, L_BASE);
  mem(c, OP_I32_LOAD, 2, offset);
}

static void load_i64(jctx *c, u32 offset) {
  local_get(c, L_BASE);
  mem(c, OP_I64_LOAD, 3, offset);
}

static void load_f32(jctx *c, u32 offset) {
  local_get(c, L_BASE);
  mem(c, OP_F32_LOAD, 2, offset);
}

static void load_f64(jctx *c, u32 offset) {
  local_get(c, L_BASE);
  mem(c, OP_F64_LOAD, 3, offset);
}

// Stores to a frame slot: push the base first (begin_store), then the value, then finish with store_*
static void begin_store(jctx *c) { local_get(c, L_BASE); }

// An int as the interpreter keeps it in a slot: sign-extended to 64 bits
static void store_int(jctx *c, u32 offset) {
  op(c, OP_I64_EXTEND_I32_S);
  mem(c, OP_I64_STORE, 3, offset);
}

// A reference: zero-extended
static void store_ref(jctx *c, u32 offset) {
  op(c, OP_I64_EXTEND_I32_U);
  mem(c, OP_I64_STORE, 3, offset);
}

static void store_i64(jctx *c, u32 offset) { mem(c, OP_I64_STORE, 3, offset); }
static void store_f32(jctx *c, u32 offset) { mem(c, OP_F32_STORE, 2, offset); }
static void store_f64(jctx *c, u32 offset) { mem(c, OP_F64_STORE, 3, offset); }

static void copy_slot(jctx *c, u32 from, u32 to) {
  begin_store(c);
  load_i64(c, from);
  store_i64(c, to);
}

/* Control flow ---------------------------------------------------------------------------------------------------- */

// Label depth of the start of block j's code, from inside the current block's code (j must be later)
static int depth_of_block(jctx *c, int j) { return j - c->current_block - 1 + c->nesting; }
static int depth_of_dispatch(jctx *c) { return c->block_count - 1 - c->current_block + c->nesting; }

static void open_if(jctx *c) {
  op(c, OP_IF);
  op(c, BLOCK_VOID);
  c->nesting++;
}

static void close(jctx *c) {
  op(c, OP_END);
  c->nesting--;
}

static void store_pc(jctx *c, int pc) {
  local_get(c, L_FRAME);
  i32_const(c, pc);
  mem(c, OP_I32_STORE16, 1, offsetof(stack_frame, program_counter));
}

// Leave the compiled code: the interpreter continues the frame at pc
static void bail(jctx *c, int pc, jit_status status) {
  store_pc(c, pc);
  i32_const(c, status);
  op(c, OP_RETURN);
}

// Leave with the exception the helper just raised at pc
static void return_exception(jctx *c) {
  i32_const(c, JIT_STATUS_EXCEPTION);
  op(c, OP_RETURN);
}

// Throws a NullPointerException at pc when the i32 in local `l` is null
static void null_check(jctx *c, int l, int pc) {
  local_get(c, l);
  op(c, OP_I32_EQZ);
  open_if(c);
  store_pc(c, pc);
  local_get(c, L_THREAD);
  call_helper(c, HELPER(jit_rt_npe), "i:");
  return_exception(c);
  close(c);
}

// Jump to target (consumes nothing). Backward jumps check the fuel first, as the interpreter's branches do
static void jump(jctx *c, int pc, int target) {
  int block = c->block_of_pc[target];
  DCHECK(block >= 0);
  if (target > pc && block > c->current_block) {
    op(c, OP_BR);
    op_u(c, depth_of_block(c, block));
    return;
  }
  // fuel: when it has run out, let the interpreter execute the branch (it suspends for the refuel check there)
  local_get(c, L_THREAD);
  mem(c, OP_I32_LOAD, 2, offsetof(vm_thread, fuel));
  local_set(c, L_A);
  local_get(c, L_A);
  op(c, OP_I32_EQZ);
  open_if(c);
  bail(c, pc, JIT_STATUS_BAIL);
  close(c);
  local_get(c, L_THREAD);
  local_get(c, L_A);
  i32_const(c, 1);
  op(c, OP_I32_SUB);
  mem(c, OP_I32_STORE, 2, offsetof(vm_thread, fuel));
  i32_const(c, target);
  local_set(c, L_NEXT);
  op(c, OP_BR);
  op_u(c, depth_of_dispatch(c));
}

// Conditional jump on the i32 condition on the wasm stack
static void jump_if(jctx *c, int pc, int target) {
  int block = c->block_of_pc[target];
  if (target > pc && block > c->current_block) {
    op(c, OP_BR_IF);
    op_u(c, depth_of_block(c, block));
    return;
  }
  open_if(c);
  jump(c, pc, target);
  close(c);
}

static int branch_target(int pc, const bytecode_insn *insn) { return pc + insn->delta / (int)sizeof(bytecode_insn); }

/* ------------------------------------------------------------------------------------------------------------------
 * Instructions
 * ------------------------------------------------------------------------------------------------------------------ */

static bool is_branch(insn_code_kind kind) {
  switch (kind) {
  case insn_goto:
  case insn_if_acmpeq:
  case insn_if_acmpne:
  case insn_if_icmpeq:
  case insn_if_icmpne:
  case insn_if_icmplt:
  case insn_if_icmpge:
  case insn_if_icmpgt:
  case insn_if_icmple:
  case insn_ifeq:
  case insn_ifne:
  case insn_iflt:
  case insn_ifge:
  case insn_ifgt:
  case insn_ifle:
  case insn_ifnonnull:
  case insn_ifnull:
    return true;
  default:
    return false;
  }
}

// Instructions after which the interpreter may hand the frame back (an invoke returns to pc + 1)
static bool may_resume_after(insn_code_kind kind) {
  switch (kind) {
  case insn_invokevirtual:
  case insn_invokespecial:
  case insn_invokestatic:
  case insn_invokeinterface:
  case insn_invokedynamic:
  case insn_invokevtable_monomorphic:
  case insn_invokevtable_polymorphic:
  case insn_invokeitable_monomorphic:
  case insn_invokeitable_polymorphic:
  case insn_invokespecial_resolved:
  case insn_invokestatic_resolved:
  case insn_invokecallsite:
  case insn_invokesigpoly:
    return true;
  default:
    return false;
  }
}

static bool ends_block(insn_code_kind kind) {
  switch (kind) {
  case insn_tableswitch:
  case insn_lookupswitch:
  case insn_return:
  case insn_ireturn:
  case insn_lreturn:
  case insn_freturn:
  case insn_dreturn:
  case insn_areturn:
  case insn_athrow:
    return true;
  default:
    return is_branch(kind) || may_resume_after(kind);
  }
}

static void binop_int(jctx *c, int sd, u8 opcode) {
  begin_store(c);
  load_i32(c, stack_offset(c, sd - 2));
  load_i32(c, stack_offset(c, sd - 1));
  op(c, opcode);
  store_int(c, stack_offset(c, sd - 2));
}

static void binop_long(jctx *c, int sd, u8 opcode) {
  begin_store(c);
  load_i64(c, stack_offset(c, sd - 2));
  load_i64(c, stack_offset(c, sd - 1));
  op(c, opcode);
  store_i64(c, stack_offset(c, sd - 2));
}

// lshl/lshr/lushr: the shift distance is an int
static void shift_long(jctx *c, int sd, u8 opcode) {
  begin_store(c);
  load_i64(c, stack_offset(c, sd - 2));
  load_i32(c, stack_offset(c, sd - 1));
  op(c, OP_I64_EXTEND_I32_U);
  op(c, opcode);
  store_i64(c, stack_offset(c, sd - 2));
}

static void binop_float(jctx *c, int sd, u8 opcode) {
  begin_store(c);
  load_f32(c, stack_offset(c, sd - 2));
  load_f32(c, stack_offset(c, sd - 1));
  op(c, opcode);
  store_f32(c, stack_offset(c, sd - 2));
}

static void binop_double(jctx *c, int sd, u8 opcode) {
  begin_store(c);
  load_f64(c, stack_offset(c, sd - 2));
  load_f64(c, stack_offset(c, sd - 1));
  op(c, opcode);
  store_f64(c, stack_offset(c, sd - 2));
}

// idiv/irem/ldiv/lrem: ArithmeticException on 0, and MIN / -1 = MIN, MIN % -1 = 0 (wasm div_s traps there)
static void divide(jctx *c, int pc, int sd, bool is_long, bool is_rem) {
  u32 a = stack_offset(c, sd - 2), b = stack_offset(c, sd - 1);
  if (is_long) {
    load_i64(c, b);
    op(c, 0x50); // i64.eqz
  } else {
    load_i32(c, b);
    op(c, OP_I32_EQZ);
  }
  open_if(c);
  store_pc(c, pc);
  local_get(c, L_THREAD);
  call_helper(c, HELPER(jit_rt_div0), "i:");
  return_exception(c);
  close(c);

  begin_store(c);
  if (is_long) {
    load_i64(c, b);
    i64_const(c, -1);
    op(c, OP_I64_EQ);
  } else {
    load_i32(c, b);
    i32_const(c, -1);
    op(c, OP_I32_EQ);
  }
  op(c, OP_IF);
  op(c, is_long ? VT_I64 : VT_I32);
  c->nesting++;
  // divisor -1: the negation (wrapping), or 0 for the remainder
  if (is_long) {
    if (is_rem) {
      i64_const(c, 0);
    } else {
      i64_const(c, 0);
      load_i64(c, a);
      op(c, OP_I64_SUB);
    }
  } else {
    if (is_rem) {
      i32_const(c, 0);
    } else {
      i32_const(c, 0);
      load_i32(c, a);
      op(c, OP_I32_SUB);
    }
  }
  op(c, OP_ELSE);
  if (is_long) {
    load_i64(c, a);
    load_i64(c, b);
    op(c, is_rem ? OP_I64_REM_S : OP_I64_DIV_S);
  } else {
    load_i32(c, a);
    load_i32(c, b);
    op(c, is_rem ? OP_I32_REM_S : OP_I32_DIV_S);
  }
  close(c);
  if (is_long)
    store_i64(c, a);
  else
    store_int(c, a);
}

// fcmpl/fcmpg/dcmpl/dcmpg: (a > b) - (a < b), or nan_result when either is NaN
static void float_compare(jctx *c, int sd, bool is_double, int nan_result) {
  u32 a = stack_offset(c, sd - 2), b = stack_offset(c, sd - 1);
  void (*load)(jctx *, u32) = is_double ? load_f64 : load_f32;
  u8 gt = is_double ? OP_F64_GT : OP_F32_GT, lt = is_double ? OP_F64_LT : OP_F32_LT,
     ne = is_double ? OP_F64_NE : OP_F32_NE;
  begin_store(c);
  i32_const(c, nan_result);
  load(c, a);
  load(c, b);
  op(c, gt);
  load(c, a);
  load(c, b);
  op(c, lt);
  op(c, OP_I32_SUB);
  // NaN: a != a or b != b
  load(c, a);
  load(c, a);
  op(c, ne);
  load(c, b);
  load(c, b);
  op(c, ne);
  op(c, OP_I32_OR);
  op(c, OP_SELECT);
  store_int(c, a);
}

static void convert(jctx *c, int sd, void (*load)(jctx *, u32), const u8 *ops, int op_count,
                    void (*store)(jctx *, u32)) {
  u32 slot = stack_offset(c, sd - 1);
  begin_store(c);
  load(c, slot);
  for (int i = 0; i < op_count; ++i) {
    if (ops[i] >= 0xf0) // FC_* + 0xf0
      fc(c, ops[i] - 0xf0);
    else
      op(c, ops[i]);
  }
  store(c, slot);
}

#define CONVERT(load, store, ...)                                                                                      \
  do {                                                                                                                 \
    static const u8 ops_[] = {__VA_ARGS__};                                                                            \
    convert(c, sd, load, ops_, sizeof(ops_), store);                                                                   \
  } while (0)

// The array in local L_A, the index in L_B; throws NPE / AIOOBE at pc
static void array_checks(jctx *c, int pc) {
  null_check(c, L_A, pc);
  local_get(c, L_B);
  local_get(c, L_A);
  mem(c, OP_I32_LOAD, 2, kArrayLengthOffset);
  op(c, OP_I32_GE_U); // also catches negative indices
  open_if(c);
  store_pc(c, pc);
  local_get(c, L_THREAD);
  local_get(c, L_B);
  local_get(c, L_A);
  mem(c, OP_I32_LOAD, 2, kArrayLengthOffset);
  call_helper(c, HELPER(jit_rt_array_oob), "iii:");
  return_exception(c);
  close(c);
}

// Address of element L_B of array L_A, with elements of 1 << size_log2 bytes
static void element_address(jctx *c, int size_log2) {
  local_get(c, L_A);
  local_get(c, L_B);
  if (size_log2) {
    i32_const(c, size_log2);
    op(c, OP_I32_SHL);
  }
  op(c, OP_I32_ADD);
}

static void array_load(jctx *c, int pc, int sd, insn_code_kind kind) {
  load_i32(c, stack_offset(c, sd - 2));
  local_set(c, L_A);
  load_i32(c, stack_offset(c, sd - 1));
  local_set(c, L_B);
  array_checks(c, pc);
  u32 dst = stack_offset(c, sd - 2);
  begin_store(c);
  switch (kind) {
  case insn_iaload:
    element_address(c, 2);
    mem(c, OP_I32_LOAD, 2, kArrayDataOffset);
    store_int(c, dst);
    break;
  case insn_aaload:
    element_address(c, 2);
    mem(c, OP_I32_LOAD, 2, kArrayDataOffset);
    store_ref(c, dst);
    break;
  case insn_laload:
    element_address(c, 3);
    mem(c, OP_I64_LOAD, 3, kArrayDataOffset);
    store_i64(c, dst);
    break;
  case insn_faload:
    element_address(c, 2);
    mem(c, OP_F32_LOAD, 2, kArrayDataOffset);
    store_f32(c, dst);
    break;
  case insn_daload:
    element_address(c, 3);
    mem(c, OP_F64_LOAD, 3, kArrayDataOffset);
    store_f64(c, dst);
    break;
  case insn_baload:
    element_address(c, 0);
    mem(c, OP_I32_LOAD8_S, 0, kArrayDataOffset);
    store_int(c, dst);
    break;
  case insn_saload:
    element_address(c, 1);
    mem(c, OP_I32_LOAD16_S, 1, kArrayDataOffset);
    store_int(c, dst);
    break;
  case insn_caload:
    element_address(c, 1);
    mem(c, OP_I32_LOAD16_U, 1, kArrayDataOffset);
    store_int(c, dst);
    break;
  default:
    UNREACHABLE();
  }
}

static void array_store(jctx *c, int pc, int sd, insn_code_kind kind) {
  load_i32(c, stack_offset(c, sd - 3));
  local_set(c, L_A);
  load_i32(c, stack_offset(c, sd - 2));
  local_set(c, L_B);
  array_checks(c, pc);
  u32 value = stack_offset(c, sd - 1);
  if (kind == insn_aastore) {
    // ArrayStoreException unless null or exactly the component class (else ask instanceof)
    load_i32(c, value);
    local_set(c, L_C);
    local_get(c, L_C);
    open_if(c);
    local_get(c, L_C);
    mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
    local_get(c, L_A);
    mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
    mem(c, OP_I32_LOAD, 2, offsetof(classdesc, one_fewer_dim));
    op(c, OP_I32_NE);
    open_if(c);
    store_pc(c, pc);
    local_get(c, L_THREAD);
    local_get(c, L_A);
    local_get(c, L_C);
    call_helper(c, HELPER(jit_rt_aastore_check), "iii:i");
    op(c, OP_I32_EQZ);
    open_if(c);
    return_exception(c);
    close(c);
    close(c);
    close(c);
  }
  switch (kind) {
  case insn_iastore:
  case insn_aastore:
    element_address(c, 2);
    load_i32(c, value);
    mem(c, OP_I32_STORE, 2, kArrayDataOffset);
    break;
  case insn_lastore:
    element_address(c, 3);
    load_i64(c, value);
    mem(c, OP_I64_STORE, 3, kArrayDataOffset);
    break;
  case insn_fastore:
    element_address(c, 2);
    load_f32(c, value);
    mem(c, OP_F32_STORE, 2, kArrayDataOffset);
    break;
  case insn_dastore:
    element_address(c, 3);
    load_f64(c, value);
    mem(c, OP_F64_STORE, 3, kArrayDataOffset);
    break;
  case insn_bastore:
    element_address(c, 0);
    load_i32(c, value);
    mem(c, OP_I32_STORE8, 0, kArrayDataOffset);
    break;
  case insn_sastore:
  case insn_castore:
    element_address(c, 1);
    load_i32(c, value);
    mem(c, OP_I32_STORE16, 1, kArrayDataOffset);
    break;
  default:
    UNREACHABLE();
  }
}

// getfield_X: the object at sd - 1 is replaced by the field
static void emit_getfield(jctx *c, int pc, int sd, const bytecode_insn *insn) {
  u32 slot = stack_offset(c, sd - 1);
  u32 offset = (u32)(uintptr_t)insn->ic2;
  load_i32(c, slot);
  local_set(c, L_A);
  null_check(c, L_A, pc);
  begin_store(c);
  local_get(c, L_A);
  switch (insn->kind) {
  case insn_getfield_B:
  case insn_getfield_Z:
    mem(c, OP_I32_LOAD8_S, 0, offset);
    store_int(c, slot);
    break;
  case insn_getfield_C:
    mem(c, OP_I32_LOAD16_U, 1, offset);
    store_int(c, slot);
    break;
  case insn_getfield_S:
    mem(c, OP_I32_LOAD16_S, 1, offset);
    store_int(c, slot);
    break;
  case insn_getfield_I:
    mem(c, OP_I32_LOAD, 2, offset);
    store_int(c, slot);
    break;
  case insn_getfield_L:
    mem(c, OP_I32_LOAD, 2, offset);
    store_ref(c, slot);
    break;
  case insn_getfield_J:
    mem(c, OP_I64_LOAD, 3, offset);
    store_i64(c, slot);
    break;
  case insn_getfield_F:
    mem(c, OP_F32_LOAD, 2, offset);
    store_f32(c, slot);
    break;
  case insn_getfield_D:
    mem(c, OP_F64_LOAD, 3, offset);
    store_f64(c, slot);
    break;
  default:
    UNREACHABLE();
  }
}

// putfield_X: object at sd - 2, value at sd - 1
static void emit_putfield(jctx *c, int pc, int sd, const bytecode_insn *insn) {
  u32 offset = (u32)(uintptr_t)insn->ic2;
  u32 value = stack_offset(c, sd - 1);
  load_i32(c, stack_offset(c, sd - 2));
  local_set(c, L_A);
  null_check(c, L_A, pc);
  local_get(c, L_A);
  switch (insn->kind) {
  case insn_putfield_B:
  case insn_putfield_Z:
    load_i32(c, value);
    mem(c, OP_I32_STORE8, 0, offset);
    break;
  case insn_putfield_C:
  case insn_putfield_S:
    load_i32(c, value);
    mem(c, OP_I32_STORE16, 1, offset);
    break;
  case insn_putfield_I:
  case insn_putfield_L:
    load_i32(c, value);
    mem(c, OP_I32_STORE, 2, offset);
    break;
  case insn_putfield_J:
    load_i64(c, value);
    mem(c, OP_I64_STORE, 3, offset);
    break;
  case insn_putfield_F:
    load_f32(c, value);
    mem(c, OP_F32_STORE, 2, offset);
    break;
  case insn_putfield_D:
    load_f64(c, value);
    mem(c, OP_F64_STORE, 3, offset);
    break;
  default:
    UNREACHABLE();
  }
}

// getstatic_X / putstatic_X: the field lives at the fixed address insn->ic
static void emit_getstatic(jctx *c, int sd, const bytecode_insn *insn) {
  u32 slot = stack_offset(c, sd);
  begin_store(c);
  i32_const(c, (s32)(uintptr_t)insn->ic);
  switch (insn->kind) {
  case insn_getstatic_B:
  case insn_getstatic_Z:
    mem(c, OP_I32_LOAD8_S, 0, 0);
    store_int(c, slot);
    break;
  case insn_getstatic_C:
    mem(c, OP_I32_LOAD16_U, 1, 0);
    store_int(c, slot);
    break;
  case insn_getstatic_S:
    mem(c, OP_I32_LOAD16_S, 1, 0);
    store_int(c, slot);
    break;
  case insn_getstatic_I:
    mem(c, OP_I32_LOAD, 2, 0);
    store_int(c, slot);
    break;
  case insn_getstatic_L:
    mem(c, OP_I32_LOAD, 2, 0);
    store_ref(c, slot);
    break;
  case insn_getstatic_J:
    mem(c, OP_I64_LOAD, 3, 0);
    store_i64(c, slot);
    break;
  case insn_getstatic_F:
    mem(c, OP_F32_LOAD, 2, 0);
    store_f32(c, slot);
    break;
  case insn_getstatic_D:
    mem(c, OP_F64_LOAD, 3, 0);
    store_f64(c, slot);
    break;
  default:
    UNREACHABLE();
  }
}

static void emit_putstatic(jctx *c, int sd, const bytecode_insn *insn) {
  u32 value = stack_offset(c, sd - 1);
  i32_const(c, (s32)(uintptr_t)insn->ic);
  switch (insn->kind) {
  case insn_putstatic_B:
  case insn_putstatic_Z:
    load_i32(c, value);
    mem(c, OP_I32_STORE8, 0, 0);
    break;
  case insn_putstatic_C:
  case insn_putstatic_S:
    load_i32(c, value);
    mem(c, OP_I32_STORE16, 1, 0);
    break;
  case insn_putstatic_I:
  case insn_putstatic_L:
    load_i32(c, value);
    mem(c, OP_I32_STORE, 2, 0);
    break;
  case insn_putstatic_J:
    load_i64(c, value);
    mem(c, OP_I64_STORE, 3, 0);
    break;
  case insn_putstatic_F:
    load_f32(c, value);
    mem(c, OP_F32_STORE, 2, 0);
    break;
  case insn_putstatic_D:
    load_f64(c, value);
    mem(c, OP_F64_STORE, 3, 0);
    break;
  default:
    UNREACHABLE();
  }
}

// A helper returning an object (0 when it raised): store it at slot or leave with the exception
static void store_object_or_throw(jctx *c, u32 slot) {
  local_set(c, L_A);
  local_get(c, L_A);
  op(c, OP_I32_EQZ);
  open_if(c);
  return_exception(c);
  close(c);
  begin_store(c);
  local_get(c, L_A);
  store_ref(c, slot);
}

static void compare_branch(jctx *c, int pc, int sd, const bytecode_insn *insn) {
  int target = branch_target(pc, insn);
  switch (insn->kind) {
  case insn_ifeq:
  case insn_ifne:
  case insn_iflt:
  case insn_ifge:
  case insn_ifgt:
  case insn_ifle:
  case insn_ifnull:
  case insn_ifnonnull:
    load_i32(c, stack_offset(c, sd - 1));
    i32_const(c, 0);
    break;
  case insn_goto:
    jump(c, pc, target);
    return;
  default:
    load_i32(c, stack_offset(c, sd - 2));
    load_i32(c, stack_offset(c, sd - 1));
    break;
  }
  u8 cmp;
  switch (insn->kind) {
  case insn_ifeq:
  case insn_ifnull:
  case insn_if_icmpeq:
  case insn_if_acmpeq:
    cmp = OP_I32_EQ;
    break;
  case insn_ifne:
  case insn_ifnonnull:
  case insn_if_icmpne:
  case insn_if_acmpne:
    cmp = OP_I32_NE;
    break;
  case insn_iflt:
  case insn_if_icmplt:
    cmp = OP_I32_LT_S;
    break;
  case insn_ifge:
  case insn_if_icmpge:
    cmp = OP_I32_GE_S;
    break;
  case insn_ifgt:
  case insn_if_icmpgt:
    cmp = OP_I32_GT_S;
    break;
  case insn_ifle:
  case insn_if_icmple:
    cmp = OP_I32_LE_S;
    break;
  default:
    UNREACHABLE();
  }
  op(c, cmp);
  jump_if(c, pc, target);
}

static void table_switch(jctx *c, int pc, int sd, const bytecode_insn *insn) {
  struct tableswitch_data *t = insn->tableswitch;
  int count = t->high - t->low + 1;
  // block (default) { block t_{count-1} { ... block t_0 { br_table } t_0 code } ... } default code
  op(c, OP_BLOCK);
  op(c, BLOCK_VOID);
  c->nesting++;
  for (int i = 0; i < count; ++i) {
    op(c, OP_BLOCK);
    op(c, BLOCK_VOID);
    c->nesting++;
  }
  load_i32(c, stack_offset(c, sd - 1));
  i32_const(c, t->low);
  op(c, OP_I32_SUB);
  op(c, OP_BR_TABLE);
  op_u(c, count);
  for (int i = 0; i < count; ++i)
    op_u(c, i);
  op_u(c, count); // default
  for (int i = 0; i < count; ++i) {
    close(c);
    jump(c, pc, t->targets[i]);
  }
  close(c);
  jump(c, pc, t->default_target);
}

static void lookup_switch(jctx *c, int pc, int sd, const bytecode_insn *insn) {
  struct lookupswitch_data *t = insn->lookupswitch;
  for (int i = 0; i < t->keys_count; ++i) {
    load_i32(c, stack_offset(c, sd - 1));
    i32_const(c, t->keys[i]);
    op(c, OP_I32_EQ);
    jump_if(c, pc, t->targets[i]);
  }
  jump(c, pc, t->default_target);
}

// The value returned by the frame goes to frame->stack[0], where the driver picks it up
static void emit_return(jctx *c, int sd, insn_code_kind kind) {
  if (kind == insn_ireturn) {
    begin_store(c);
    load_i32(c, stack_offset(c, sd - 1));
    store_int(c, stack_offset(c, 0));
  } else if (kind == insn_areturn) {
    begin_store(c);
    load_i32(c, stack_offset(c, sd - 1));
    store_ref(c, stack_offset(c, 0));
  } else if (kind != insn_return) {
    copy_slot(c, stack_offset(c, sd - 1), stack_offset(c, 0));
  }
  i32_const(c, JIT_STATUS_RETURN);
  op(c, OP_RETURN);
}

// The general path of an invoke: jit_rt_invoke looks at the instruction as it is now (it may have been resolved since)
static void emit_invoke_helper(jctx *c, int pc) {
  local_get(c, L_THREAD);
  local_get(c, L_FRAME);
  i32_const(c, pc);
  call_helper(c, HELPER(jit_rt_invoke), "iii:i");
  local_set(c, L_A);
  local_get(c, L_A);
  open_if(c);
  // EXCEPTION, or BAIL with the pc at the invoke: the driver continues a pushed callee or executes the instruction
  local_get(c, L_A);
  i32_const(c, JIT_INVOKE_EXCEPTION);
  op(c, OP_I32_EQ);
  open_if(c);
  return_exception(c);
  close(c);
  i32_const(c, JIT_STATUS_BAIL);
  op(c, OP_RETURN);
  close(c);
}

static void open_block(jctx *c) {
  op(c, OP_BLOCK);
  op(c, BLOCK_VOID);
  c->nesting++;
}

// Branch to the enclosing block opened `outward` blocks/ifs above the innermost one (0 = innermost)
static void br(jctx *c, int depth) {
  op(c, OP_BR);
  op_u(c, depth);
}

static void br_if(jctx *c, int depth) {
  op(c, OP_BR_IF);
  op_u(c, depth);
}

// A trivial getter (`aload_0; getfield; xreturn`) whose getfield is resolved: read the field of the receiver in L_A
// and put it where the receiver was. Returns false if the getter's field is not resolved yet
static bool inline_getter(jctx *c, cp_method *target, u32 slot) {
  const bytecode_insn *getfield = &target->code->code[1];
  u32 offset = (u32)(uintptr_t)getfield->ic2;
  begin_store(c);
  local_get(c, L_A);
  switch (getfield->kind) {
  case insn_getfield_B:
  case insn_getfield_Z:
    mem(c, OP_I32_LOAD8_S, 0, offset);
    store_int(c, slot);
    return true;
  case insn_getfield_C:
    mem(c, OP_I32_LOAD16_U, 1, offset);
    store_int(c, slot);
    return true;
  case insn_getfield_S:
    mem(c, OP_I32_LOAD16_S, 1, offset);
    store_int(c, slot);
    return true;
  case insn_getfield_I:
    mem(c, OP_I32_LOAD, 2, offset);
    store_int(c, slot);
    return true;
  case insn_getfield_L:
    mem(c, OP_I32_LOAD, 2, offset);
    store_ref(c, slot);
    return true;
  case insn_getfield_J:
    mem(c, OP_I64_LOAD, 3, offset);
    store_i64(c, slot);
    return true;
  case insn_getfield_F:
    mem(c, OP_F32_LOAD, 2, offset);
    store_f32(c, slot);
    return true;
  case insn_getfield_D:
    mem(c, OP_F64_LOAD, 3, offset);
    store_f64(c, slot);
    return true;
  default:
    op(c, OP_DROP); // not resolved: undo the pushes
    op(c, OP_DROP);
    return false;
  }
}

// Calls the method in L_D directly when it is compiled: push its frame over the arguments (as push_plain_frame does),
// call its function, pop. Everything about the target is read at run time (it may have been compiled after this
// code, or be the result of a vtable lookup). Branches to the enclosing $slow block (depth 0) when the target is not a
// plain compiled method, the frame would not fit or the calls are nested too deeply; leaves the compiled code (BAIL)
// when the callee did not return
static void inline_call(jctx *c, u32 args_slot, bool returns) {
  // plain: not native, not synchronized (the driver handles those), no call-site intrinsic (jit_rt_invoke runs it)
  local_get(c, L_D);
  mem(c, OP_I32_LOAD16_U, 1, offsetof(cp_method, access_flags));
  i32_const(c, ACCESS_NATIVE | ACCESS_SYNCHRONIZED);
  op(c, OP_I32_AND);
  br_if(c, 0);
  local_get(c, L_D);
  mem(c, 0x2d /* i32.load8_u */, 0, offsetof(cp_method, call_intrinsic));
  br_if(c, 0);
  // compiled: L_E = its function
  local_get(c, L_D);
  mem(c, OP_I32_LOAD, 2, offsetof(cp_method, jit_function));
  local_set(c, L_E);
  local_get(c, L_E);
  i32_const(c, 0);
  op(c, OP_I32_LE_S);
  br_if(c, 0);
  // L_C = code, L_B = callee frame = args + max_locals
  local_get(c, L_D);
  mem(c, OP_I32_LOAD, 2, offsetof(cp_method, code));
  local_set(c, L_C);
  local_get(c, L_BASE);
  i32_const(c, args_slot);
  op(c, OP_I32_ADD);
  local_get(c, L_C);
  mem(c, OP_I32_LOAD16_U, 1, offsetof(attribute_code, max_locals));
  i32_const(c, 3);
  op(c, OP_I32_SHL);
  op(c, OP_I32_ADD);
  local_set(c, L_B);
  // room on the thread's frame stack
  local_get(c, L_B);
  local_get(c, L_C);
  mem(c, OP_I32_LOAD, 2, offsetof(attribute_code, frame_size));
  op(c, OP_I32_ADD);
  local_get(c, L_THREAD);
  mem(c, OP_I32_LOAD, 2, offsetof(vm_thread, stack.frame_buffer_end));
  op(c, 0x4b); // i32.gt_u
  br_if(c, 0);
  // nesting on the host stack
  i32_const(c, (s32)(uintptr_t)&jit_nesting);
  mem(c, OP_I32_LOAD, 2, 0);
  i32_const(c, JIT_MAX_NESTING);
  op(c, OP_I32_GE_S);
  br_if(c, 0);

  // frame->method, frame->prev = thread top, thread top = frame, then the rest from the method's template frame
  local_get(c, L_B);
  local_get(c, L_D);
  mem(c, OP_I32_STORE, 2, offsetof(stack_frame, method));
  local_get(c, L_B);
  local_get(c, L_THREAD);
  mem(c, OP_I32_LOAD, 2, offsetof(vm_thread, stack.top));
  mem(c, OP_I32_STORE, 2, offsetof(stack_frame, prev));
  local_get(c, L_THREAD);
  local_get(c, L_B);
  mem(c, OP_I32_STORE, 2, offsetof(vm_thread, stack.top));
  static_assert((sizeof(stack_frame) - offsetof(stack_frame, kind)) % 4 == 0);
  for (size_t at = offsetof(stack_frame, kind); at < sizeof(stack_frame); at += 4) {
    local_get(c, L_B);
    local_get(c, L_D);
    mem(c, OP_I32_LOAD, 2, offsetof(cp_method, template_frame) + at);
    mem(c, OP_I32_STORE, 2, at);
  }

  i32_const(c, (s32)(uintptr_t)&jit_nesting);
  i32_const(c, (s32)(uintptr_t)&jit_nesting);
  mem(c, OP_I32_LOAD, 2, 0);
  i32_const(c, 1);
  op(c, OP_I32_ADD);
  mem(c, OP_I32_STORE, 2, 0);
  local_get(c, L_THREAD);
  local_get(c, L_B);
  i32_const(c, 0);
  local_get(c, L_E);
  op(c, OP_CALL_INDIRECT);
  op_u(c, 0); // type 0: the compiled function
  op_u(c, 0);
  local_set(c, L_C);
  i32_const(c, (s32)(uintptr_t)&jit_nesting);
  i32_const(c, (s32)(uintptr_t)&jit_nesting);
  mem(c, OP_I32_LOAD, 2, 0);
  i32_const(c, 1);
  op(c, OP_I32_SUB);
  mem(c, OP_I32_STORE, 2, 0);

  // Not returned (bailed out or threw): the callee's frame is the driver's now; the pc is at the invoke
  local_get(c, L_C);
  open_if(c);
  i32_const(c, JIT_STATUS_BAIL);
  op(c, OP_RETURN);
  close(c);
  // Pop, then put the result where the arguments were. In this order: without arguments, the callee's frame header
  // starts at the arguments' slot, and the result would overwrite its prev
  if (returns) {
    local_get(c, L_B);
    mem(c, OP_I64_LOAD, 3, offsetof(stack_frame, stack));
    local_set(c, L_X);
  }
  local_get(c, L_THREAD);
  local_get(c, L_B);
  mem(c, OP_I32_LOAD, 2, offsetof(stack_frame, prev));
  mem(c, OP_I32_STORE, 2, offsetof(vm_thread, stack.top));
  if (returns) {
    begin_store(c);
    local_get(c, L_X);
    store_i64(c, args_slot);
  }
}

// An invoke gets a fast path in front of jit_rt_invoke. With a target known now (static, special, or a monomorphic
// cache, guarded by the receiver's class): an inline getter or null check, or a direct call. With a polymorphic
// cache: the vtable/itable lookup here, then a direct call. A failed guard, or a target that is not compiled, takes
// the general path through jit_rt_invoke
static void emit_invoke(jctx *c, int pc) {
  const bytecode_insn *insn = c->code->code + pc;
  store_pc(c, pc);

  cp_method *target = nullptr;
  classdesc *receiver_class = nullptr;
  bool has_receiver = false, vtable = false, itable = false;
  switch (insn->kind) {
  case insn_invokestatic_resolved:
    target = insn->ic;
    if (target->is_signature_polymorphic)
      return emit_invoke_helper(c, pc);
    break;
  case insn_invokespecial_resolved:
    target = insn->ic;
    has_receiver = true;
    break;
  case insn_invokevtable_monomorphic:
  case insn_invokeitable_monomorphic:
    target = insn->ic;
    receiver_class = insn->ic2;
    has_receiver = true;
    break;
  case insn_invokevtable_polymorphic:
    vtable = has_receiver = true;
    break;
  case insn_invokeitable_polymorphic:
    itable = has_receiver = true;
    break;
  default:
    return emit_invoke_helper(c, pc);
  }

  bool getter = target && target->call_intrinsic == CALL_INTRINSIC_GETTER && has_receiver &&
                target->code->code[1].kind >= insn_getfield_B && target->code->code[1].kind <= insn_getfield_L;
  bool null_check = target && target->call_intrinsic == CALL_INTRINSIC_CHECK_NOT_NULL;
  if (target && target->call_intrinsic && !getter && !null_check)
    return emit_invoke_helper(c, pc); // other intrinsics run in jit_rt_invoke
  if (target && (!target->code || target->access_flags & (ACCESS_NATIVE | ACCESS_SYNCHRONIZED)))
    return emit_invoke_helper(c, pc);

  u32 args_slot = stack_offset(c, c->sd[pc] - insn->args);
  // block $done { block $slow { fast path; br $done } general path }
  open_block(c);
  open_block(c);
  if (has_receiver) {
    load_i32(c, args_slot);
    local_set(c, L_A);
    local_get(c, L_A);
    op(c, OP_I32_EQZ);
    br_if(c, 0);
    if (receiver_class) {
      local_get(c, L_A);
      mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
      i32_const(c, (s32)(uintptr_t)receiver_class);
      op(c, OP_I32_NE);
      br_if(c, 0);
    }
  }
  if (getter) {
    inline_getter(c, target, args_slot);
  } else if (null_check) {
    load_i32(c, args_slot);
    op(c, OP_I32_EQZ);
    br_if(c, 0); // null: the real method throws
  } else {
    if (vtable) {
      // L_D = receiver->descriptor->vtable.methods[index]
      local_get(c, L_A);
      mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
      mem(c, OP_I32_LOAD, 2, offsetof(classdesc, vtable.methods));
      mem(c, OP_I32_LOAD, 2, 4 * (u32)(uintptr_t)insn->ic2);
      local_set(c, L_D);
    } else if (itable) {
      local_get(c, L_A);
      mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
      i32_const(c, (s32)(uintptr_t)insn->ic);
      i32_const(c, (s32)(uintptr_t)insn->ic2);
      call_helper(c, HELPER(itable_lookup), "iii:i");
      local_set(c, L_D);
      local_get(c, L_D);
      op(c, OP_I32_EQZ);
      br_if(c, 0); // AbstractMethodError
    } else {
      i32_const(c, (s32)(uintptr_t)target);
      local_set(c, L_D);
    }
    inline_call(c, args_slot, insn->returns);
  }
  br(c, 1);
  close(c);
  emit_invoke_helper(c, pc);
  close(c);
}

// Emits one instruction. Anything not handled here hands the frame to the interpreter
static void emit_insn(jctx *c, int pc) {
  const bytecode_insn *insn = c->code->code + pc;
  int sd = c->sd[pc];
  switch (insn->kind) {
  case insn_nop:
  case insn_pop:
  case insn_pop2:
    return;
  case insn_aconst_null:
    begin_store(c);
    i64_const(c, 0);
    store_i64(c, stack_offset(c, sd));
    return;
  case insn_iconst:
  case insn_lconst:
    begin_store(c);
    i64_const(c, insn->integer_imm);
    store_i64(c, stack_offset(c, sd));
    return;
  case insn_fconst:
    begin_store(c);
    op(c, OP_F32_CONST);
    jb_bytes(&c->body, &insn->f_imm, 4);
    store_f32(c, stack_offset(c, sd));
    return;
  case insn_dconst:
    begin_store(c);
    op(c, OP_F64_CONST);
    jb_bytes(&c->body, &insn->d_imm, 8);
    store_f64(c, stack_offset(c, sd));
    return;

  // Locals: the slot is (stack_value *)frame - delta
  case insn_iload:
    begin_store(c);
    load_i32(c, local_offset(c, c->locals_count - insn->delta));
    store_int(c, stack_offset(c, sd));
    return;
  case insn_aload:
    begin_store(c);
    load_i32(c, local_offset(c, c->locals_count - insn->delta));
    store_ref(c, stack_offset(c, sd));
    return;
  case insn_lload:
  case insn_fload:
  case insn_dload:
    copy_slot(c, local_offset(c, c->locals_count - insn->delta), stack_offset(c, sd));
    return;
  case insn_istore:
  case insn_astore:
  case insn_lstore:
  case insn_fstore:
  case insn_dstore:
    copy_slot(c, stack_offset(c, sd - 1), local_offset(c, c->locals_count - insn->delta));
    return;
  case insn_iinc: {
    u32 slot = local_offset(c, insn->iinc.index);
    begin_store(c);
    load_i32(c, slot);
    i32_const(c, insn->iinc.const_);
    op(c, OP_I32_ADD);
    store_int(c, slot);
    return;
  }

  // Stack manipulation, slot by slot as the interpreter does
  case insn_dup:
    copy_slot(c, stack_offset(c, sd - 1), stack_offset(c, sd));
    return;
  case insn_dup_x1:
  case insn_dup_x2:
  case insn_dup2:
  case insn_dup2_x1:
  case insn_dup2_x2:
  case insn_swap: {
    // read val1 (top) .. val4 into temporaries, then write the result pattern
    int n = insn->kind == insn_dup2_x2 ? 4 : (insn->kind == insn_dup_x2 || insn->kind == insn_dup2_x1) ? 3 : 2;
    for (int i = 0; i < n; ++i) {
      load_i64(c, stack_offset(c, sd - 1 - i));
      local_set(c, L_X + i);
    }
    // patterns: values listed from the lowest slot written (sd - n) upward; 1 = val1 (top), 2 = val2 ...
    static const char *patterns[] = {
        [insn_dup_x1] = "121", [insn_dup_x2] = "1321", [insn_dup2] = "2121" /* from sd - 2 */,
        [insn_dup2_x1] = "21321", [insn_dup2_x2] = "214321", [insn_swap] = "12"};
    const char *p = patterns[insn->kind];
    for (int i = 0; p[i]; ++i) {
      begin_store(c);
      local_get(c, L_X + (p[i] - '1'));
      store_i64(c, stack_offset(c, sd - n + i));
    }
    return;
  }

  // Arithmetic
  case insn_iadd:
    return binop_int(c, sd, OP_I32_ADD);
  case insn_isub:
    return binop_int(c, sd, OP_I32_SUB);
  case insn_imul:
    return binop_int(c, sd, OP_I32_MUL);
  case insn_iand:
    return binop_int(c, sd, OP_I32_AND);
  case insn_ior:
    return binop_int(c, sd, OP_I32_OR);
  case insn_ixor:
    return binop_int(c, sd, OP_I32_XOR);
  case insn_ishl:
    return binop_int(c, sd, OP_I32_SHL);
  case insn_ishr:
    return binop_int(c, sd, OP_I32_SHR_S);
  case insn_iushr:
    return binop_int(c, sd, OP_I32_SHR_U);
  case insn_ladd:
    return binop_long(c, sd, OP_I64_ADD);
  case insn_lsub:
    return binop_long(c, sd, OP_I64_SUB);
  case insn_lmul:
    return binop_long(c, sd, OP_I64_MUL);
  case insn_land:
    return binop_long(c, sd, OP_I64_AND);
  case insn_lor:
    return binop_long(c, sd, OP_I64_OR);
  case insn_lxor:
    return binop_long(c, sd, OP_I64_XOR);
  case insn_lshl:
    return shift_long(c, sd, OP_I64_SHL);
  case insn_lshr:
    return shift_long(c, sd, OP_I64_SHR_S);
  case insn_lushr:
    return shift_long(c, sd, OP_I64_SHR_U);
  case insn_fadd:
    return binop_float(c, sd, OP_F32_ADD);
  case insn_fsub:
    return binop_float(c, sd, OP_F32_SUB);
  case insn_fmul:
    return binop_float(c, sd, OP_F32_MUL);
  case insn_fdiv:
    return binop_float(c, sd, OP_F32_DIV);
  case insn_dadd:
    return binop_double(c, sd, OP_F64_ADD);
  case insn_dsub:
    return binop_double(c, sd, OP_F64_SUB);
  case insn_dmul:
    return binop_double(c, sd, OP_F64_MUL);
  case insn_ddiv:
    return binop_double(c, sd, OP_F64_DIV);
  case insn_idiv:
    return divide(c, pc, sd, false, false);
  case insn_irem:
    return divide(c, pc, sd, false, true);
  case insn_ldiv:
    return divide(c, pc, sd, true, false);
  case insn_lrem:
    return divide(c, pc, sd, true, true);
  case insn_frem:
    begin_store(c);
    load_f32(c, stack_offset(c, sd - 2));
    load_f32(c, stack_offset(c, sd - 1));
    call_helper(c, HELPER(jit_rt_frem), "ff:f");
    store_f32(c, stack_offset(c, sd - 2));
    return;
  case insn_drem:
    begin_store(c);
    load_f64(c, stack_offset(c, sd - 2));
    load_f64(c, stack_offset(c, sd - 1));
    call_helper(c, HELPER(jit_rt_drem), "dd:d");
    store_f64(c, stack_offset(c, sd - 2));
    return;
  case insn_ineg:
    begin_store(c);
    i32_const(c, 0);
    load_i32(c, stack_offset(c, sd - 1));
    op(c, OP_I32_SUB);
    store_int(c, stack_offset(c, sd - 1));
    return;
  case insn_lneg:
    begin_store(c);
    i64_const(c, 0);
    load_i64(c, stack_offset(c, sd - 1));
    op(c, OP_I64_SUB);
    store_i64(c, stack_offset(c, sd - 1));
    return;
  case insn_fneg:
    CONVERT(load_f32, store_f32, OP_F32_NEG);
    return;
  case insn_dneg:
    CONVERT(load_f64, store_f64, OP_F64_NEG);
    return;
  case insn_lcmp: {
    u32 a = stack_offset(c, sd - 2), b = stack_offset(c, sd - 1);
    begin_store(c);
    load_i64(c, a);
    load_i64(c, b);
    op(c, OP_I64_GT_S);
    load_i64(c, a);
    load_i64(c, b);
    op(c, OP_I64_LT_S);
    op(c, OP_I32_SUB);
    store_int(c, a);
    return;
  }
  case insn_fcmpl:
    return float_compare(c, sd, false, -1);
  case insn_fcmpg:
    return float_compare(c, sd, false, 1);
  case insn_dcmpl:
    return float_compare(c, sd, true, -1);
  case insn_dcmpg:
    return float_compare(c, sd, true, 1);

  // Conversions (the float to int ones saturate and map NaN to 0, as Java does)
  case insn_i2l:
    CONVERT(load_i32, store_i64, OP_I64_EXTEND_I32_S);
    return;
  case insn_l2i:
    CONVERT(load_i64, store_int, OP_I32_WRAP_I64);
    return;
  case insn_i2f:
    CONVERT(load_i32, store_f32, OP_F32_CONVERT_I32_S);
    return;
  case insn_i2d:
    CONVERT(load_i32, store_f64, OP_F64_CONVERT_I32_S);
    return;
  case insn_l2f:
    CONVERT(load_i64, store_f32, OP_F32_CONVERT_I64_S);
    return;
  case insn_l2d:
    CONVERT(load_i64, store_f64, OP_F64_CONVERT_I64_S);
    return;
  case insn_f2i:
    CONVERT(load_f32, store_int, 0xf0 + FC_I32_TRUNC_SAT_F32_S);
    return;
  case insn_f2l:
    CONVERT(load_f32, store_i64, 0xf0 + FC_I64_TRUNC_SAT_F32_S);
    return;
  case insn_d2i:
    CONVERT(load_f64, store_int, 0xf0 + FC_I32_TRUNC_SAT_F64_S);
    return;
  case insn_d2l:
    CONVERT(load_f64, store_i64, 0xf0 + FC_I64_TRUNC_SAT_F64_S);
    return;
  case insn_f2d:
    CONVERT(load_f32, store_f64, OP_F64_PROMOTE_F32);
    return;
  case insn_d2f:
    CONVERT(load_f64, store_f32, OP_F32_DEMOTE_F64);
    return;
  case insn_i2b:
    CONVERT(load_i32, store_int, OP_I32_EXTEND8_S);
    return;
  case insn_i2s:
    CONVERT(load_i32, store_int, OP_I32_EXTEND16_S);
    return;
  case insn_i2c: {
    u32 slot = stack_offset(c, sd - 1);
    begin_store(c);
    load_i32(c, slot);
    i32_const(c, 0xffff);
    op(c, OP_I32_AND);
    store_int(c, slot);
    return;
  }

  // Branches
  case insn_goto:
  case insn_if_acmpeq:
  case insn_if_acmpne:
  case insn_if_icmpeq:
  case insn_if_icmpne:
  case insn_if_icmplt:
  case insn_if_icmpge:
  case insn_if_icmpgt:
  case insn_if_icmple:
  case insn_ifeq:
  case insn_ifne:
  case insn_iflt:
  case insn_ifge:
  case insn_ifgt:
  case insn_ifle:
  case insn_ifnonnull:
  case insn_ifnull:
    return compare_branch(c, pc, sd, insn);
  case insn_tableswitch:
    return table_switch(c, pc, sd, insn);
  case insn_lookupswitch:
    return lookup_switch(c, pc, sd, insn);
  case insn_return:
  case insn_ireturn:
  case insn_lreturn:
  case insn_freturn:
  case insn_dreturn:
  case insn_areturn:
    return emit_return(c, sd, insn->kind);
  case insn_athrow:
    load_i32(c, stack_offset(c, sd - 1));
    local_set(c, L_A);
    null_check(c, L_A, pc);
    store_pc(c, pc);
    local_get(c, L_THREAD);
    local_get(c, L_A);
    mem(c, OP_I32_STORE, 2, offsetof(vm_thread, current_exception));
    return_exception(c);
    return;

  // Arrays
  case insn_arraylength:
    load_i32(c, stack_offset(c, sd - 1));
    local_set(c, L_A);
    null_check(c, L_A, pc);
    begin_store(c);
    local_get(c, L_A);
    mem(c, OP_I32_LOAD, 2, kArrayLengthOffset);
    store_int(c, stack_offset(c, sd - 1));
    return;
  case insn_iaload:
  case insn_laload:
  case insn_faload:
  case insn_daload:
  case insn_aaload:
  case insn_baload:
  case insn_saload:
  case insn_caload:
    return array_load(c, pc, sd, insn->kind);
  case insn_iastore:
  case insn_lastore:
  case insn_fastore:
  case insn_dastore:
  case insn_aastore:
  case insn_bastore:
  case insn_sastore:
  case insn_castore:
    return array_store(c, pc, sd, insn->kind);
  case insn_newarray:
    store_pc(c, pc);
    local_get(c, L_THREAD);
    i32_const(c, insn->array_type);
    load_i32(c, stack_offset(c, sd - 1));
    call_helper(c, HELPER(jit_rt_newarray), "iii:i");
    store_object_or_throw(c, stack_offset(c, sd - 1));
    return;
  case insn_anewarray_resolved:
    store_pc(c, pc);
    local_get(c, L_THREAD);
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    load_i32(c, stack_offset(c, sd - 1));
    call_helper(c, HELPER(jit_rt_anewarray), "iii:i");
    store_object_or_throw(c, stack_offset(c, sd - 1));
    return;

  // Objects (classdescs are malloc'd and never move; objects are never baked in)
  case insn_new_resolved:
    store_pc(c, pc);
    local_get(c, L_THREAD);
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    call_helper(c, HELPER(jit_rt_new), "ii:i");
    store_object_or_throw(c, stack_offset(c, sd));
    return;
  case insn_checkcast_resolved:
    load_i32(c, stack_offset(c, sd - 1));
    local_set(c, L_A);
    local_get(c, L_A);
    open_if(c);
    local_get(c, L_A);
    mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    op(c, OP_I32_NE);
    open_if(c);
    store_pc(c, pc);
    local_get(c, L_THREAD);
    local_get(c, L_A);
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    call_helper(c, HELPER(jit_rt_checkcast), "iii:i");
    op(c, OP_I32_EQZ);
    open_if(c);
    return_exception(c);
    close(c);
    close(c);
    close(c);
    return;
  case insn_instanceof_resolved: {
    u32 slot = stack_offset(c, sd - 1);
    load_i32(c, slot);
    local_set(c, L_A);
    begin_store(c);
    local_get(c, L_A);
    op(c, OP_IF);
    op(c, VT_I32);
    c->nesting++;
    local_get(c, L_A);
    mem(c, OP_I32_LOAD, 2, offsetof(obj_header, descriptor));
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    op(c, OP_I32_EQ);
    op(c, OP_IF);
    op(c, VT_I32);
    c->nesting++;
    i32_const(c, 1);
    op(c, OP_ELSE);
    local_get(c, L_A);
    i32_const(c, (s32)(uintptr_t)insn->classdesc);
    call_helper(c, HELPER(jit_rt_instanceof), "ii:i");
    close(c);
    op(c, OP_ELSE);
    i32_const(c, 0);
    close(c);
    store_int(c, slot);
    return;
  }

  // Fields
  case insn_getfield_B:
  case insn_getfield_C:
  case insn_getfield_S:
  case insn_getfield_I:
  case insn_getfield_J:
  case insn_getfield_F:
  case insn_getfield_D:
  case insn_getfield_Z:
  case insn_getfield_L:
    return emit_getfield(c, pc, sd, insn);
  case insn_putfield_B:
  case insn_putfield_C:
  case insn_putfield_S:
  case insn_putfield_I:
  case insn_putfield_J:
  case insn_putfield_F:
  case insn_putfield_D:
  case insn_putfield_Z:
  case insn_putfield_L:
    return emit_putfield(c, pc, sd, insn);
  case insn_getstatic_B:
  case insn_getstatic_C:
  case insn_getstatic_S:
  case insn_getstatic_I:
  case insn_getstatic_J:
  case insn_getstatic_F:
  case insn_getstatic_D:
  case insn_getstatic_Z:
  case insn_getstatic_L:
    return emit_getstatic(c, sd, insn);
  case insn_putstatic_B:
  case insn_putstatic_C:
  case insn_putstatic_S:
  case insn_putstatic_I:
  case insn_putstatic_J:
  case insn_putstatic_F:
  case insn_putstatic_D:
  case insn_putstatic_Z:
  case insn_putstatic_L:
    return emit_putstatic(c, sd, insn);

  // ldc of a string or class: once created, the object is kept in the constant pool entry (read it at run time,
  // it moves with the GC); before that, the interpreter creates it
  case insn_ldc: {
    cp_entry *ent = insn->cp;
    void **cached;
    if (ent->kind == CP_KIND_STRING)
      cached = &ent->string.interned;
    else if (ent->kind == CP_KIND_CLASS)
      cached = &ent->class_info.vm_object;
    else
      break;
    i32_const(c, (s32)(uintptr_t)cached);
    mem(c, OP_I32_LOAD, 2, 0);
    local_set(c, L_A);
    local_get(c, L_A);
    op(c, OP_I32_EQZ);
    open_if(c);
    bail(c, pc, JIT_STATUS_BAIL);
    close(c);
    begin_store(c);
    local_get(c, L_A);
    store_ref(c, stack_offset(c, sd));
    return;
  }

  // Not resolved yet when the method was compiled: the interpreter resolves it
  case insn_getfield:
  case insn_putfield:
  case insn_getstatic:
  case insn_putstatic:
  case insn_new:
  case insn_anewarray:
  case insn_checkcast:
  case insn_instanceof:
    bail(c, pc, JIT_STATUS_BAIL_UNRESOLVED);
    return;

  default:
    if (may_resume_after(insn->kind))
      return emit_invoke(c, pc);
    break;
  }
  // Monitors, multianewarray...
  bail(c, pc, JIT_STATUS_BAIL);
}

/* ------------------------------------------------------------------------------------------------------------------
 * Module
 * ------------------------------------------------------------------------------------------------------------------ */

static u8 value_type(char t) {
  switch (t) {
  case 'i':
    return VT_I32;
  case 'l':
    return VT_I64;
  case 'f':
    return VT_F32;
  case 'd':
    return VT_F64;
  default:
    UNREACHABLE();
  }
}

static void section(jbuf *module, u8 id, jbuf *contents) {
  jb_byte(module, id);
  jb_uleb(module, contents->len);
  jb_bytes(module, contents->data, contents->len);
  contents->len = 0;
}

static void name(jbuf *b, const char *s) {
  jb_uleb(b, strlen(s));
  jb_bytes(b, s, strlen(s));
}

static jbuf build_module(jctx *c) {
  jbuf module = {0}, s = {0};
  static const u8 header[] = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
  jb_bytes(&module, header, sizeof(header));

  // types
  jb_uleb(&s, c->sig_count);
  for (int i = 0; i < c->sig_count; ++i) {
    const char *sig = c->sigs[i], *colon = strchr(sig, ':');
    jb_byte(&s, 0x60);
    jb_uleb(&s, colon - sig);
    for (const char *p = sig; p < colon; ++p)
      jb_byte(&s, value_type(*p));
    jb_uleb(&s, strlen(colon + 1));
    for (const char *p = colon + 1; *p; ++p)
      jb_byte(&s, value_type(*p));
  }
  section(&module, 1, &s);

  // imports: env.memory, env.table
  jb_uleb(&s, 2);
  name(&s, "env");
  name(&s, "memory");
  jb_byte(&s, 0x02); // memory
  jb_byte(&s, 0x00); // no maximum
  jb_uleb(&s, 0);
  name(&s, "env");
  name(&s, "table");
  jb_byte(&s, 0x01); // table
  jb_byte(&s, 0x70); // funcref
  jb_byte(&s, 0x00);
  jb_uleb(&s, 0);
  section(&module, 2, &s);

  // functions: one, of type 0
  jb_uleb(&s, 1);
  jb_uleb(&s, 0);
  section(&module, 3, &s);

  // exports: "f"
  jb_uleb(&s, 1);
  name(&s, "f");
  jb_byte(&s, 0x00);
  jb_uleb(&s, 0);
  section(&module, 7, &s);

  // code
  jbuf fn = {0};
  jb_uleb(&fn, 2); // local groups
  jb_uleb(&fn, L_COUNT_I32);
  jb_byte(&fn, VT_I32);
  jb_uleb(&fn, L_COUNT_I64);
  jb_byte(&fn, VT_I64);
  jb_bytes(&fn, c->body.data, c->body.len);
  jb_uleb(&s, 1);
  jb_uleb(&s, fn.len);
  jb_bytes(&s, fn.data, fn.len);
  free(fn.data);
  section(&module, 10, &s);

  free(s.data);
  return module;
}

static bool compilable(cp_method *method) {
  attribute_code *code = method->code;
  if (!code || !method->code_analysis || code->insn_count > JIT_MAX_INSNS)
    return false;
  for (int pc = 0; pc < code->insn_count; ++pc) {
    insn_code_kind kind = code->code[pc].kind;
    if (kind == insn_jsr || kind == insn_ret)
      return false;
  }
  return true;
}

// Returns the table index of the compiled function, or -1
static s32 compile(cp_method *method) {
  if (!compilable(method))
    return -1;

  attribute_code *code = method->code;
  jctx ctx = {
      .method = method,
      .code = code,
      .sd = method->code_analysis->insn_index_to_sd,
      .insn_count = code->insn_count,
      .locals_count = code->max_locals,
  };
  jctx *c = &ctx;
  c->stack_offset = 8 * c->locals_count + offsetof(stack_frame, stack);
  sig_index(c, "iii:i"); // type 0: the compiled function

  // Basic block leaders
  int n = c->insn_count;
  bool *leader = calloc(n + 1, sizeof(bool));
  leader[0] = true;
  for (int pc = 0; pc < n; ++pc) {
    const bytecode_insn *insn = code->code + pc;
    if (is_branch(insn->kind)) {
      leader[branch_target(pc, insn)] = true;
      leader[pc] = true; // resumed here after a fuel check
    } else if (insn->kind == insn_tableswitch) {
      leader[insn->tableswitch->default_target] = true;
      for (int i = 0; i < insn->tableswitch->targets_count; ++i)
        leader[insn->tableswitch->targets[i]] = true;
    } else if (insn->kind == insn_lookupswitch) {
      leader[insn->lookupswitch->default_target] = true;
      for (int i = 0; i < insn->lookupswitch->targets_count; ++i)
        leader[insn->lookupswitch->targets[i]] = true;
    }
    if (ends_block(insn->kind))
      leader[pc + 1] = true;
  }
  if (code->exception_table)
    for (int i = 0; i < code->exception_table->entries_count; ++i)
      leader[code->exception_table->entries[i].handler_insn] = true;

  c->block_of_pc = malloc(sizeof(int) * n);
  for (int pc = 0; pc < n; ++pc)
    c->block_of_pc[pc] = leader[pc] ? c->block_count++ : -1;
  free(leader);

  // function body: block $entry { loop $dispatch { block B_{n-1} { ... block B_0 { br_table } code_0 } ... } }
  int blocks = c->block_count;
  local_get(c, L_FRAME);
  i32_const(c, 8 * (s32)c->locals_count);
  op(c, OP_I32_SUB);
  local_set(c, L_BASE);
  local_get(c, L_PC);
  local_set(c, L_NEXT);

  op(c, OP_BLOCK);
  op(c, BLOCK_VOID);
  op(c, OP_LOOP);
  op(c, BLOCK_VOID);
  for (int i = 0; i < blocks; ++i) {
    op(c, OP_BLOCK);
    op(c, BLOCK_VOID);
  }
  local_get(c, L_NEXT);
  op(c, OP_BR_TABLE);
  op_u(c, n);
  for (int pc = 0; pc < n; ++pc)
    op_u(c, c->block_of_pc[pc] >= 0 ? c->block_of_pc[pc] : blocks + 1);
  op_u(c, blocks + 1); // default: not an entry point

  for (int pc = 0; pc < n; ++pc) {
    if (c->block_of_pc[pc] >= 0) {
      DCHECK(c->nesting == 0);
      op(c, OP_END);
      c->current_block = c->block_of_pc[pc];
    }
    emit_insn(c, pc);
  }
  op(c, OP_END); // loop
  op(c, OP_END); // entry block
  // Entered at a pc that is not a block leader: the interpreter runs it
  i32_const(c, JIT_STATUS_BAIL);
  op(c, OP_END);

  jbuf module = build_module(c);
  free(c->body.data);
  free(c->block_of_pc);

  // jit_host_install は wasm 側の import 関数なので、ネイティブビルド（JIT_SUPPORTED=0）
  // ではそもそも宣言自体が存在しない
  s32 index = -1;
#if JIT_SUPPORTED
  index = c->failed ? -1 : jit_host_install(module.data, module.len);
#endif
  free(module.data);
  return index > 0 ? index : -1;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Entry from the interpreter
 * ------------------------------------------------------------------------------------------------------------------ */

static int jit_enabled = -1;
int jit_try_run(vm_thread *thread, stack_frame *frame) {
#if JIT_SUPPORTED
  if (unlikely(jit_enabled < 0)) {
    const char *env = getenv("BJVM_JIT");
    jit_enabled = !(env && strcmp(env, "0") == 0);
    const char *threshold = getenv("BJVM_JIT_THRESHOLD");
    if (threshold && atoi(threshold) > 0)
      JIT_THRESHOLD = atoi(threshold);
  }
  if (!jit_enabled)
    return -1;
  cp_method *method = frame->method;
  if (unlikely(method->jit_function <= 0)) {
    if (method->jit_function < 0)
      return -1;
    if (++method->jit_hotness < JIT_THRESHOLD)
      return -1;
    method->jit_compiles++;
    method->jit_function = compile(method);
    if (method->jit_function < 0)
      return -1;
  }
  int status = ((jit_function)(uintptr_t)method->jit_function)(thread, frame, frame->program_counter);
  if (unlikely(status == JIT_STATUS_BAIL_UNRESOLVED) && ++method->jit_unresolved_bails >= JIT_RECOMPILE_BAILS) {
    // The instruction has most likely been resolved by now: compile again once the method has run a while more
    method->jit_unresolved_bails = 0;
    method->jit_function = method->jit_compiles >= JIT_MAX_COMPILES ? -1 : 0;
    method->jit_hotness = JIT_THRESHOLD / 2;
  }
  return status;
#else
  (void)thread;
  (void)frame;
  return -1;
#endif
}

void jit_note_hot_loop(stack_frame *frame) {
  if (frame->method->jit_function == 0)
    frame->method->jit_hotness = JIT_THRESHOLD;
}

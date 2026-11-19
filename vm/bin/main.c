//
// Created by Cowpox on 12/10/24.
//

#include <stdlib.h>
#include <unistd.h>
#include "arrays.h"
#include "cached_classdescs.h"
#include "exceptions.h"
#include "objects.h"
#include "jit.h"
#include "reflection.h"

#include "../bjvm.h"
#ifdef EMSCRIPTEN
#include <emscripten/emscripten.h>
#endif

#include <roundrobin_scheduler.h>
#include <tgmath.h>

EMSCRIPTEN_KEEPALIVE
vm *ffi_create_vm(const char *classpath, size_t heap_size, write_bytes stdout_, write_bytes stderr_) {
  // cwd は BrowserOS のカーネルがプロセスごとに持つ（open(2) も相対パスをそこから解く）ので、
  // ここで chdir する必要は無い
  vm_options options = default_vm_options();
  options.classpath = (slice){.chars = (char *)classpath, .len = (int)strlen(classpath)};
  options.heap_size = heap_size;
  options.write_stdout = stdout_;
  options.write_stderr = stderr_;
  options.stdio_override_param = nullptr;

  vm *vm = create_vm(options);
  return vm;
}

EMSCRIPTEN_KEEPALIVE
vm_thread *ffi_create_thread(vm *vm) {
  vm_thread *thr = create_main_thread(vm, default_thread_options());
  return thr;
}

EMSCRIPTEN_KEEPALIVE
classdesc *ffi_get_class(vm_thread *thr, const char *name) {
  classdesc *clazz = bootstrap_lookup_class(thr, (slice){.chars = (char *)name, .len = (int)strlen(name)});
  if (!clazz)
    return nullptr;
  initialize_class_t ctx = {.args = {thr, clazz}};
  future_t fut = initialize_class(&ctx);
  DCHECK(fut.status == FUTURE_READY);
  return clazz;
}

EMSCRIPTEN_KEEPALIVE
obj_header *ffi_get_current_exception(vm_thread *thr) { return thr->current_exception; }

EMSCRIPTEN_KEEPALIVE
void ffi_clear_current_exception(vm_thread *thr) { thr->current_exception = nullptr; }

EMSCRIPTEN_KEEPALIVE
classdesc *ffi_get_classdesc(obj_header *obj) { return obj->descriptor; }

bool check_casts(vm_thread *thread, cp_method *method, stack_value *args) {
  // Perform the moral equivalent of a checkcast instruction
  int argc = method_argc(method);
  for (int i = 0; i < argc; i++) {
    field_descriptor *arg;
    if (method->access_flags & ACCESS_STATIC) {
      arg = method->descriptor->args + i;
    } else {
      arg = i != 0 ? method->descriptor->args + (i - 1) : nullptr;
    }

    if (arg && arg->repr_kind != TYPE_KIND_REFERENCE)
      continue;
    if (!args[i].obj) // null
      continue;

    classdesc *class;
    if (arg) {
      load_class_of_field_descriptor_t ctx = {.args = {thread, get_current_classloader(thread), arg->unparsed}};
      thread->stack.synchronous_depth++;
      future_t fut = load_class_of_field_descriptor(&ctx);
      CHECK(fut.status == FUTURE_READY);
      thread->stack.synchronous_depth--;
      class = ctx._result;
    } else {
      class = method->my_class;
    }

    if (!instanceof(args[i].obj->descriptor, class)) {
      raise_class_cast_exception(thread, args[i].obj->descriptor, class);
      return true;
    }
  }
  return false;
}

EMSCRIPTEN_KEEPALIVE
rr_scheduler *ffi_create_rr_scheduler(vm *vm) {
  rr_scheduler *scheduler = malloc(sizeof(rr_scheduler));
  rr_scheduler_init(scheduler, vm);
  vm->scheduler = scheduler;
  return scheduler;
}

EMSCRIPTEN_KEEPALIVE
u32 ffi_rr_scheduler_wait_for_us(rr_scheduler *scheduler) { return rr_scheduler_may_sleep_us(scheduler); }

EMSCRIPTEN_KEEPALIVE
scheduler_status_t ffi_rr_scheduler_step(rr_scheduler *scheduler) { return rr_scheduler_step(scheduler); }

EMSCRIPTEN_KEEPALIVE
bool ffi_rr_record_is_ready(execution_record *record) { return record->status == SCHEDULER_RESULT_DONE; }

EMSCRIPTEN_KEEPALIVE
execution_record *ffi_rr_schedule(vm_thread *thread, cp_method *method, stack_value *args) {
  call_interpreter_t call = {.args = {thread, method, args}};
  CHECK(thread->vm->scheduler && "No scheduler set");
  return rr_scheduler_run(thread->vm->scheduler, call);
}

EMSCRIPTEN_KEEPALIVE
stack_value *ffi_get_execution_record_result_pointer(execution_record *record) {
  if (record->js_handle != -1) {
    void *result = (void *)deref_js_handle(record->vm, record->js_handle);
    record->returned.obj = result;
  }
  return &record->returned;
}

EMSCRIPTEN_KEEPALIVE
int ffi_get_execution_record_js_handle(execution_record *record) { return record->js_handle; }

EMSCRIPTEN_KEEPALIVE
scheduler_status_t ffi_execute_immediately(execution_record *record) {
  return rr_scheduler_execute_immediately(record);
}

EMSCRIPTEN_KEEPALIVE
void ffi_free_execution_record(execution_record *record) { free_execution_record(record); }

enum ArrayClassification : s32 {
  NOT_AN_ARRAY = -1,
  BYTE_ARRAY = 0,
  SHORT_ARRAY = 1,
  INT_ARRAY = 2,
  LONG_ARRAY = 3,
  FLOAT_ARRAY = 4,
  DOUBLE_ARRAY = 5,
  CHAR_ARRAY = 6,
  BOOLEAN_ARRAY = 7,
  OBJECT_ARRAY = 8
};

EMSCRIPTEN_KEEPALIVE
enum ArrayClassification ffi_classify_array(object obj) {
  switch (obj->descriptor->kind) {
  case CD_KIND_ORDINARY:
    return NOT_AN_ARRAY;
  case CD_KIND_ORDINARY_ARRAY:
    return OBJECT_ARRAY;
  case CD_KIND_PRIMITIVE_ARRAY: {
    switch (obj->descriptor->primitive_component) {
    case TYPE_KIND_BOOLEAN:
      return BOOLEAN_ARRAY;
    case TYPE_KIND_CHAR:
      return CHAR_ARRAY;
    case TYPE_KIND_FLOAT:
      return FLOAT_ARRAY;
    case TYPE_KIND_DOUBLE:
      return DOUBLE_ARRAY;
    case TYPE_KIND_BYTE:
      return BYTE_ARRAY;
    case TYPE_KIND_SHORT:
      return SHORT_ARRAY;
    case TYPE_KIND_INT:
      return INT_ARRAY;
    case TYPE_KIND_LONG:
      return LONG_ARRAY;
    case TYPE_KIND_VOID:
    case TYPE_KIND_REFERENCE:
      UNREACHABLE();
    }
  }
  case CD_KIND_PRIMITIVE:
  default:
    UNREACHABLE();
  }
}

EMSCRIPTEN_KEEPALIVE
uintptr_t ffi_get_element_ptr(vm_thread *thread, object obj, int index) {
  int length = ArrayLength(obj);
  if (index < 0 || index >= length) {
    raise_array_index_oob_exception(thread, index, length);
    return 0;
  }
  if (obj->descriptor->kind == CD_KIND_ORDINARY_ARRAY) {
    return (uintptr_t)((object *)ArrayData(obj) + index);
  }
  return (uintptr_t)ArrayData(obj) + index * sizeof_type_kind(obj->descriptor->primitive_component);
}

EMSCRIPTEN_KEEPALIVE
int ffi_get_array_length(object obj) { return ArrayLength(obj); }

void ffi_free_rr_scheduler(rr_scheduler *scheduler) { rr_scheduler_uninit(scheduler); }

EMSCRIPTEN_KEEPALIVE
call_interpreter_t *ffi_async_run(vm_thread *thread, cp_method *method, stack_value *args) {
  call_interpreter_t *ctx = malloc(sizeof(call_interpreter_t));

  if (check_casts(thread, method, args)) {
    return nullptr;
  }

  *ctx = (call_interpreter_t){.args = {thread, method, args}};
  return ctx;
}

EMSCRIPTEN_KEEPALIVE
object ffi_allocate_object(vm_thread *thr, cp_method *method) {
  classdesc *clazz = method->my_class;
  return AllocateObject(thr, clazz, clazz->instance_bytes);
}

EMSCRIPTEN_KEEPALIVE
object ffi_create_string(vm_thread *thread, const char *str, size_t len) {
  return MakeJStringFromModifiedUTF8(thread, (slice){.chars = (char *)str, .len = len}, false);
}

EMSCRIPTEN_KEEPALIVE
bool ffi_is_string(object obj) { return obj && utf8_equals(obj->descriptor->name, "java/lang/String"); }

// Status passed to System.exit since the last call (0-255), or -1 if there was none. Reading clears
// it, so a host driving several calls in one VM does not see a stale status from an earlier one.
// System.exit が呼ばれたか（記録を消さずに見る）。呼んだスレッドは halt0 の中で眠り続けるので、
// ホストはこれを見て駆動ループを抜ける
EMSCRIPTEN_KEEPALIVE
int ffi_exit_requested(vm *vm) { return vm->exit_status >= 0; }

EMSCRIPTEN_KEEPALIVE
int ffi_take_exit_status(vm *vm) {
  int status = vm->exit_status;
  vm->exit_status = -1;
  return status;
}

/**
 * Build a java.lang.String[] from `count` NUL-terminated modified-UTF-8 strings laid end to end in
 * `packed`. Returns nullptr if any allocation fails, with the pending exception left on the thread.
 *
 * The host cannot assemble this array itself. Every MakeJStringFromModifiedUTF8 may trigger a GC,
 * and the GC compacts, so an array pointer handed out to JS goes stale the moment the next element
 * is created. Keeping the loop on this side lets the array live in a handle, which the GC updates.
 *
 * Taking the whole batch at once (rather than exposing an allocate-array plus a store-element pair)
 * is what makes that possible: there is no point at which the host holds a bare object pointer.
 */
EMSCRIPTEN_KEEPALIVE
object ffi_create_string_array(vm_thread *thread, const char *packed, int count) {
  DCHECK(count >= 0);

  object array = CreateObjectArray1D(thread, cached_classes(thread->vm)->string, count);
  if (!array)
    return nullptr;

  handle *array_handle = make_handle(thread, array);
  const char *cursor = packed;

  for (int i = 0; i < count; i++) {
    size_t length = strlen(cursor);
    object string = MakeJStringFromModifiedUTF8(thread, (slice){.chars = (char *)cursor, .len = (int)length}, false);
    if (!string) {
      drop_handle(thread, array_handle);
      return nullptr;
    }
    // Re-read the array through the handle: creating the string above may have moved it.
    ((object *)ArrayData(array_handle->obj))[i] = string;
    cursor += length + 1;
  }

  object result = array_handle->obj;
  drop_handle(thread, array_handle);
  return result;
}

/**
 * Read element `index` of an Object[] created by ffi_create_string_array. Only safe to call while no
 * Java allocation can happen on the JS side: the returned bare pointer goes stale at the next GC, so
 * it must be handed to the scheduler (whose pending calls are GC roots) before anything else runs.
 */
EMSCRIPTEN_KEEPALIVE
object ffi_object_array_get(object array, int index) {
  assert(array);
  return ((object *)ArrayData(array))[index];
}

EMSCRIPTEN_KEEPALIVE
uint8_t *ffi_get_string_data(object str) {
  assert(str);
  assert(utf8_equals(str->descriptor->name, "java/lang/String"));
  object array = ((struct native_String *)str)->value;
  return ArrayData(array);
}

EMSCRIPTEN_KEEPALIVE
size_t ffi_get_string_len(object str) {
  assert(str);
  assert(utf8_equals(str->descriptor->name, "java/lang/String"));
  object array = ((struct native_String *)str)->value;
  return ArrayLength(array);
}

EMSCRIPTEN_KEEPALIVE
u8 ffi_get_string_coder(object str) {
  assert(str);
  assert(utf8_equals(str->descriptor->name, "java/lang/String"));
  return ((struct native_String *)str)->coder;
}

EMSCRIPTEN_KEEPALIVE
bool ffi_instanceof(object obj, classdesc *target) { return !obj || instanceof(obj->descriptor, target); }

EMSCRIPTEN_KEEPALIVE
bool ffi_run_step(call_interpreter_t *ctx, stack_value *result) {
  future_t fut = call_interpreter(ctx);
  if (fut.status == FUTURE_READY && result) {
    *result = ctx->_result;
  }
  return fut.status == FUTURE_READY;
}

EMSCRIPTEN_KEEPALIVE
void ffi_free_async_run_ctx(call_interpreter_t *ctx) { free(ctx); }

#define INSERT_METHOD()                                                                                                \
  char key[sizeof(void *)];                                                                                            \
  memcpy(key, &method, sizeof(void *));                                                                                \
  if (hash_table_insert(&field_names, key, sizeof(void *), (void *)1)) {                                               \
    continue;                                                                                                          \
  }

// Returns a TS ClassInfo as JSON
EMSCRIPTEN_KEEPALIVE
char *ffi_get_class_json(classdesc *desc) {
  cp_field **fields = nullptr;
  cp_method **methods = nullptr;

  // Collect fields from the class and its super classes
  for (classdesc *s = desc; s->super_class; s = s->super_class->classdesc) {
    for (int i = 0; i < s->fields_count; i++) {
      cp_field *f = &s->fields[i];
      if (f->access_flags & ACCESS_PUBLIC) {
        arrput(fields, f);
      }
    }
  }

  string_hash_table field_names = make_hash_table(nullptr, 0.75, 16);

  // Collect methods from the vtable/itable. Only include non-abstract methods. Keep track of duplicate methods.
  for (int i = 0; i < arrlen(desc->vtable.methods); i++) {
    cp_method *method = desc->vtable.methods[i];
    if (method->access_flags & ACCESS_ABSTRACT) {
      continue;
    }
    INSERT_METHOD()
    arrput(methods, method);
  }

  for (int i = 0; i < arrlen(desc->itables.entries); i++) {
    itable itable = desc->itables.entries[i];
    for (int j = 0; j < arrlen(itable.methods); j++) {
      itable_method_t method = itable.methods[j];
      if (method & ITABLE_METHOD_BIT_INVALID) {
        continue;
      }
      INSERT_METHOD()
      arrput(methods, (cp_method *)method);
    }
  }

  // Also collect static methods and constructors
  for (int i = 0; i < desc->methods_count; i++) {
    cp_method *method = &desc->methods[i];
    if ((method->access_flags & ACCESS_STATIC && !method->is_clinit) || method->is_ctor) {
      INSERT_METHOD()
      arrput(methods, method);
    }
  }

  free_hash_table(field_names);

  int field_index = 0;
  int method_index = 0;

  string_builder out;
  string_builder_init(&out);
  string_builder_append(&out, R"({"binaryName":"%s","fields":[)", desc->name.chars);

  for (int i = 0; i < arrlen(fields); i++) {
    cp_field *f = fields[i];
    if (i > 0)
      string_builder_append(&out, ",");
    string_builder_append(&out, R"({"name":"%s","type":"%.*s","accessFlags":%d,"byteOffset":%d,"index":%d})",
                          f->name.chars, fmt_slice(f->parsed_descriptor.unparsed), f->access_flags, f->byte_offset,
                          field_index++);
  }

  string_builder_append(&out, R"(],"methods":[)");

  for (int i = 0; i < arrlen(methods); i++) {
    cp_method *m = methods[i];
    if (i > 0)
      string_builder_append(&out, ",");
    string_builder_append(
        &out, R"({"name":"%s","index":%d,"descriptor":"%s","methodPointer":%d,"accessFlags":%d,"parameterNames":[)",
        m->name.chars, method_index++, m->unparsed_descriptor.chars, (intptr_t)m, m->access_flags);

    for (int attrib_i = 0; attrib_i < m->attributes_count; ++attrib_i) {
      if (m->attributes[attrib_i].kind == ATTRIBUTE_KIND_METHOD_PARAMETERS) {
        attribute_method_parameters *params = &m->attributes[attrib_i].method_parameters;

        for (int j = 0; j < params->count; j++) {
          if (j > 0)
            string_builder_append(&out, ",");
          string_builder_append(&out, "\"%s\"", params->params[j].name.chars);
        }

        goto found;
      }
    }

    for (int j = 0; j < m->descriptor->args_count; j++) {
      if (j > 0)
        string_builder_append(&out, ",");
      string_builder_append(&out, "\"arg%d\"", j);
    }

  found:
    string_builder_append(&out, "]}");
  }

  string_builder_append(&out, "]}");
  char *result = strdup(out.data);
  string_builder_free(&out);
  return result;
}

EMSCRIPTEN_KEEPALIVE
void ffi_set_preemption_frequency_usec(rr_scheduler *scheduler, double us) {
  us = fabs(us);
  scheduler->preemption_us = us > (double)UINT64_MAX ? UINT64_MAX : (u64)us;
}

#ifdef EMSCRIPTEN
// Emscripten のビルドは JS のグルーが ffi_* を呼んで VM を動かすので、main は初期化の印だけでよい
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}
#else
/*
 * Linux のプログラムとしての入口（wasm32-unknown-linux-musl。BrowserOS のカーネルが execve(2) で起こす）。
 *
 * 引数は `java` の起動の後半だけを受ける。`-jar`・`-m`・`@引数ファイル`・`JDK_JAVA_OPTIONS` のような、
 * どのクラスを走らせるかを決めるまでの前半は、ランチャー（@progate/browser-java の `/usr/bin/java`）が片付けてから
 * ここへ execve(2) で入れ替わる:
 *
 *   bjvm [-cp <クラスパス>] [-Xmx<大きさ>] [-D<名前>=<値>]... [-javaagent:<jar>[=<引数>]]... [--dry-run]
 *        [--and-then <n>] <main クラス> [引数...] [次の bjvm の引数 n 個]
 *
 * `--and-then <n>` は、main が終了コード 0 で終わったら、末尾の n 個を引数にして自分（/proc/self/exe）へ execve(2) で
 * 入れ替わる。`java Foo.java` がこれで「ECJ でコンパイルしてから、別の VM で走らせる」を 1 つのプロセスで済ませる
 * （ECJ は System.exit で終わるので、同じ VM では続けられない）
 *
 * JDK 本体の jar は環境変数 BOOT_CLASSPATH で渡る（→ vm/bjvm.c の get_default_boot_cp）。
 * 終了コードは本物の java と同じ: System.exit の値、捕まらなかった例外なら 1、起動の誤りなら 1。
 * Ctrl-C と kill(2) はカーネルが既定の動作でこのプロセスを止める
 */
#include <errno.h>
#include <stdio.h>
#include <time.h>

// 全スレッドが期限の無い待ちに入ったとき（Object.wait・入出力の待ちなど）に、いちど眠る長さ。
// 待ちを解くのは同じプロセスの中のスレッドだけなので、短く眠って見直す
#define LAUNCHER_IDLE_SLEEP_US 10000

// 本物の java のランチャーの既定（DEFAULT_HEAP_SIZE と同じ 64MB）
#define LAUNCHER_DEFAULT_HEAP (1u << 26)
#define LAUNCHER_DEFAULT_PREEMPTION_US 10000

typedef struct {
  vm *vm;
  rr_scheduler *scheduler;
  vm_thread *thread;
} launcher;

static slice cslice(const char *s) { return (slice){.chars = (char *)s, .len = (int)strlen(s)}; }

// `-Xmx512m` の `512m`。接尾辞は k・m・g（大文字も）。読めなければ 0
static u64 parse_size(const char *text) {
  char *end;
  unsigned long long value = strtoull(text, &end, 10);
  if (end == text)
    return 0;
  switch (*end) {
  case 'k':
  case 'K':
    value <<= 10;
    end++;
    break;
  case 'm':
  case 'M':
    value <<= 20;
    end++;
    break;
  case 'g':
  case 'G':
    value <<= 30;
    end++;
    break;
  default:
    break;
  }
  return *end ? 0 : (u64)value;
}

static void sleep_us(u64 us) {
  struct timespec ts = {.tv_sec = (time_t)(us / 1000000), .tv_nsec = (long)(us % 1000000) * 1000};
  nanosleep(&ts, nullptr);
}

/*
 * メソッドをスケジューラに載せ、終わるまで回す。main が return しても、デーモンでないスレッドが残っていれば
 * JVM は生きている（java ランチャーの決まり）ので、記録の完了ではなくスケジューラの DONE まで回す。
 * System.exit（halt0）のあとは呼んだスレッドが眠り続けるだけなので、そこで止める
 *
 * `args` は呼ぶ側が用意する。スケジューラは中身を写して持つだけで、この生ポインタは GC の根にならない
 */
static void pump(launcher *l, cp_method *method, stack_value *args) {
  execution_record *record = rr_scheduler_run(l->scheduler, (call_interpreter_t){.args = {l->thread, method, args}});
  for (;;) {
    if (rr_scheduler_step(l->scheduler) == SCHEDULER_RESULT_DONE)
      break;
    if (l->vm->exit_status >= 0)
      break;
    u64 us = rr_scheduler_may_sleep_us(l->scheduler);
    if (us > 0)
      sleep_us(us > LAUNCHER_IDLE_SLEEP_US ? LAUNCHER_IDLE_SLEEP_US : us);
  }
  free_execution_record(record);
}

// 引数が String だけの static メソッドを、その場で最後まで走らせる（main の前に済ませる System.setProperty など）。
// 例外を投げた・クラスかメソッドが無ければ false（例外はスレッドに残る）
static bool call_static_strings(launcher *l, const char *class_name, const char *name, const char *descriptor,
                                const char *const *strings, int count) {
  classdesc *cd = ffi_get_class(l->thread, class_name);
  if (!cd || l->thread->current_exception)
    return false;
  cp_method *method = method_lookup(cd, cslice(name), cslice(descriptor), false, false);
  if (!method)
    return false;

  size_t packed_len = 1;
  for (int i = 0; i < count; i++)
    packed_len += strlen(strings[i]) + 1;
  char *packed = malloc(packed_len);
  char *cursor = packed;
  for (int i = 0; i < count; i++) {
    size_t len = strlen(strings[i]);
    memcpy(cursor, strings[i], len + 1);
    cursor += len + 1;
  }
  // String は C の側でまとめて作る（作るたびに GC が動きうるので、1 つずつだと前の生ポインタが古くなる）。
  // 取り出してから載せるまでの間に Java の確保は無い
  object array = ffi_create_string_array(l->thread, packed, count);
  free(packed);
  if (!array)
    return false;
  stack_value args[8] = {};
  for (int i = 0; i < count && i < 8; i++)
    args[i].obj = ((object *)ArrayData(array))[i];

  execution_record *record = rr_scheduler_run(l->scheduler, (call_interpreter_t){.args = {l->thread, method, args}});
  rr_scheduler_execute_immediately(record);
  free_execution_record(record);
  return !l->thread->current_exception;
}

static char *dotted(const char *internal) {
  char *copy = strdup(internal);
  for (char *p = copy; *p; p++)
    if (*p == '/')
      *p = '.';
  return copy;
}

// 投げっぱなしの例外を、本物の java ランチャーと同じ形で出す。例外が無ければ 0、あれば 1
static int report_exception(launcher *l) {
  obj_header *exception = l->thread->current_exception;
  if (!exception)
    return 0;
  // printStackTrace は Java のヒープを確保するので、例外はハンドルで持つ（GC が動かすと生ポインタは古くなる）
  handle *h = make_handle(l->thread, exception);
  // 例外を持ったまま Java を呼ぶとすぐに伝わるので、先に降ろす
  l->thread->current_exception = nullptr;
  fputs("Exception in thread \"main\" ", stderr);
  cp_method *print = method_lookup(h->obj->descriptor, STR("printStackTrace"), STR("()V"), true, false);
  if (!print) {
    char *name = dotted(h->obj->descriptor->name.chars);
    fprintf(stderr, "%s\n", name);
    free(name);
  } else {
    stack_value args[1] = {{.obj = h->obj}};
    pump(l, print, args);
    // トレースを出す途中の二次的な例外は捨てる（元の失敗のほうが大事）
    l->thread->current_exception = nullptr;
  }
  drop_handle(l->thread, h);
  return 1;
}

// クラスを読み込み、初期化（<clinit>）まで済ませる。無ければ本物と同じ文面を出して nullptr
static classdesc *load_main_class(launcher *l, const char *name) {
  classdesc *cd = ffi_get_class(l->thread, name);
  if (!cd) {
    l->thread->current_exception = nullptr;
    char *d = dotted(name);
    fprintf(stderr, "Error: Could not find or load main class %s\nCaused by: java.lang.ClassNotFoundException: %s\n", d, d);
    free(d);
  }
  return cd;
}

static int run_main(launcher *l, const char *class_name, int argc, char **argv) {
  classdesc *cd = load_main_class(l, class_name);
  if (!cd)
    return 1;
  // <clinit> が例外で止まった
  if (l->thread->current_exception)
    return report_exception(l);

  cp_method *main_method = method_lookup(cd, STR("main"), STR("([Ljava/lang/String;)V"), true, false);
  if (!main_method || !(main_method->access_flags & ACCESS_STATIC)) {
    char *d = dotted(class_name);
    fprintf(stderr,
            "Error: Main method not found in class %s, please define the main method as:\n"
            "   public static void main(String[] args)\n"
            "or a JavaFX application class must extend javafx.application.Application\n",
            d);
    free(d);
    return 1;
  }
  if (!(main_method->access_flags & ACCESS_PUBLIC)) {
    char *d = dotted(class_name);
    fprintf(stderr,
            "Error: Main method is not public in class %s, please define the main method as:\n"
            "   public static void main(String[] args)\n",
            d);
    free(d);
    return 1;
  }

  size_t packed_len = 1;
  for (int i = 0; i < argc; i++)
    packed_len += strlen(argv[i]) + 1;
  char *packed = malloc(packed_len);
  char *cursor = packed;
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(argv[i]);
    memcpy(cursor, argv[i], len + 1);
    cursor += len + 1;
  }
  // String[] はスケジューラに載せる直前に作る（あいだに Java の確保を挟むと、GC が動かして古くなる）
  object array = ffi_create_string_array(l->thread, packed, argc);
  free(packed);
  if (!array) {
    fputs("Error: failed to allocate the argument array\n", stderr);
    return 1;
  }
  stack_value args[1] = {{.obj = array}};
  pump(l, main_method, args);

  int pending = report_exception(l);
  // System.exit(n) が呼ばれていれば、それが終了コード（コンパイラのような道具はこれでしか失敗を伝えない）
  int status = ffi_take_exit_status(l->vm);
  return status >= 0 ? status : pending;
}

static int usage(void) {
  fputs("Usage: bjvm [-cp <classpath>] [-Xmx<size>] [-D<name>=<value>]... [-javaagent:<jar>[=<options>]]... "
        "[--dry-run] [--and-then <n>] <main class> [args...] [next bjvm args (n)]\n",
        stderr);
  return 1;
}

static int launch(int argc, char **argv) {
  const char *classpath = ".";
  u64 heap_size = LAUNCHER_DEFAULT_HEAP;
  bool dry_run = false;
  int then_count = 0;
  char **properties = nullptr; // -D の中身（`名前=値`）
  char **agents = nullptr;     // -javaagent: の中身
  int i = 1;
  for (; i < argc; i++) {
    const char *arg = argv[i];
    if (arg[0] != '-')
      break;
    if (!strcmp(arg, "-cp") || !strcmp(arg, "-classpath") || !strcmp(arg, "--class-path")) {
      if (++i >= argc)
        return usage();
      classpath = argv[i];
    } else if (!strncmp(arg, "-Xmx", 4)) {
      heap_size = parse_size(arg + 4);
      if (!heap_size) {
        fprintf(stderr, "Invalid maximum heap size: %s\nError: Could not create the Java Virtual Machine.\n", arg);
        return 1;
      }
    } else if (!strncmp(arg, "-D", 2)) {
      arrput(properties, (char *)arg + 2);
    } else if (!strncmp(arg, "-javaagent:", 11)) {
      arrput(agents, (char *)arg + 11);
    } else if (!strcmp(arg, "--dry-run")) {
      dry_run = true;
    } else if (!strcmp(arg, "--and-then")) {
      if (++i >= argc || (then_count = atoi(argv[i])) < 0)
        return usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", arg);
      return usage();
    }
  }
  if (i >= argc - then_count)
    return usage();
  const char *main_class = argv[i++];
  int main_argc = argc - then_count - i;
  char **then_argv = argv + argc - then_count;

  vm *vm = ffi_create_vm(classpath, heap_size, nullptr, nullptr);
  if (!vm) {
    fputs("Error occurred during initialization of VM\nCould not reserve enough space for object heap\n", stderr);
    return 1;
  }
  launcher l = {.vm = vm, .scheduler = ffi_create_rr_scheduler(vm)};
  const char *preemption = getenv("BJVM_PREEMPTION_US");
  ffi_set_preemption_frequency_usec(l.scheduler, preemption && atoi(preemption) > 0 ? atoi(preemption)
                                                                                    : LAUNCHER_DEFAULT_PREEMPTION_US);
  l.thread = ffi_create_thread(vm);
  if (!l.thread) {
    fputs("Error occurred during initialization of VM\nfailed to create the main thread\n", stderr);
    return 1;
  }

  // -D は VM を作ったあと、main の前に System.setProperty で上書きする（b-jvm はプロパティを VM を作るときに固める）
  for (int p = 0; p < arrlen(properties); p++) {
    char *eq = strchr(properties[p], '=');
    char *name = eq ? strndup(properties[p], eq - properties[p]) : strdup(properties[p]);
    const char *strings[2] = {name, eq ? eq + 1 : ""};
    if (!call_static_strings(&l, "java/lang/System", "setProperty",
                             "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", strings, 2))
      l.thread->current_exception = nullptr;
    free(name);
  }
  arrfree(properties);

  // -javaagent は main の前に premain を起こす。失敗すると本物と同じく VM の起動の失敗で終わる
  for (int a = 0; a < arrlen(agents); a++) {
    const char *strings[1] = {agents[a]};
    if (!call_static_strings(&l, "sun/instrument/InstrumentationImpl", "loadAgent", "(Ljava/lang/String;)V", strings,
                             1)) {
      fputs("Error occurred during initialization of VM\nagent library failed to init: instrument\n", stderr);
      report_exception(&l);
      return 1;
    }
  }
  arrfree(agents);

  if (dry_run)
    // クラスの読み込みまで確かめて終わる（main は走らせない）
    return load_main_class(&l, main_class) ? 0 : 1;
  int code = run_main(&l, main_class, main_argc, argv + i);
  if (code != 0 || then_count == 0)
    return code;
  // 次の VM へ入れ替わる。argv[0] は自分と同じ名前、末尾は NULL
  char **next = calloc(then_count + 2, sizeof(char *));
  next[0] = argv[0];
  memcpy(next + 1, then_argv, then_count * sizeof(char *));
  fflush(nullptr);
  execv("/proc/self/exe", next);
  fprintf(stderr, "bjvm: cannot run the next VM: %s\n", strerror(errno));
  return 126;
}

/*
 * 引数は 2 つにする。clang は wasm で引数を取る main を `__main_argc_argv` という名前にするが、
 * それは引数が 2 つのときだけで、`envp` まで取る 3 引数の main は `main` のまま出る。crt1 は
 * `__main_argc_argv` か `__main_void` しか呼ばないので、3 引数にすると main が呼ばれない
 */
int main(int argc, char **argv) {
  int code = launch(argc, argv);
  /*
   * 生きている VM の下でデストラクタ（fini_modules）を回さずに終わる。本物の JVM も、Java の終わりの処理
   * （シャットダウンフック）を済ませてから exit する。Java の標準出力は write(2) で直に出ているので、
   * 書き残しは C の stdio の分だけである
   */
  fflush(nullptr);
  jit_report_stats();
  _exit(code);
}
#endif

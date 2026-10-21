#include "analysis.h"

#include <linkage.h>
#include <natives-dsl.h>

// Returns true if the frame is currently constructing the given object.
static bool is_frame_part_of_throwable_construction(stack_frame *frame, object obj) {
  if (utf8_equals(frame->method->name, "fillInStackTrace")) { // hack for now
    return true;
  }
  if (!frame->method->is_ctor || is_frame_native(frame) || frame->num_locals < 1) {
    return false;
  }
  return frame_locals(frame)[0].obj == obj;
}

// Implementation-dependent field where we can store the stack trace
obj_header **backtrace_object(obj_header *throwable) { return &((struct native_Throwable *)throwable)->backtrace; }

/**
 * `frame` から根元までの `n_frames` 枚を StackTraceElement[] にする。配列を確保できなければ nullptr
 * （例外は保留されている）。要素の確保に途中で失敗したら、そこまで埋めた配列を返す。
 * 例外の fillInStackTrace と、ほかのスレッドの Thread.getStackTrace（getStackTrace0）が使う
 */
object build_stack_trace(vm_thread *thread, stack_frame *frame, int n_frames) {
  classdesc *StackTraceElement = cached_classes(thread->vm)->stack_trace_element;
  handle *stack_trace = make_handle(thread, CreateObjectArray1D(thread, StackTraceElement, n_frames));
  if (!stack_trace->obj) { // Failed to allocate
    drop_handle(thread, stack_trace);
    return nullptr;
  }

  for (int j = 0; n_frames > 0; --n_frames, ++j) {
    DCHECK(frame);

    // Create the stack trace element
    handle *e = make_handle(thread, new_object(thread, StackTraceElement));
    if (!e->obj) { // Failed to allocate StackTraceElement
      drop_handle(thread, e);
      break;
    }

    cp_method *method = frame->method;

#define E ((struct native_StackTraceElement *)e->obj)
    int line = is_frame_native(frame) ? -1 : get_line_number(method->code, frame->program_counter);
    object o = (void *)get_class_mirror(thread, method->my_class);
    if (!o)
      goto oom;
    E->declaringClassObject = o;
    // HotSpot と同じく binary name（ドット区切り）にする。kotlin.jvm.internal.Intrinsics は
    // getStackTrace() の中から自分を getClassName() の一致で探すので、スラッシュのままだと
    // 見つからずに配列の外まで走る
    heap_string binary_name = make_heap_str_from(method->my_class->name);
    for (u32 k = 0; k < binary_name.len; ++k)
      if (binary_name.chars[k] == '/')
        binary_name.chars[k] = '.';
    o = MakeJStringFromModifiedUTF8(thread, hslc(binary_name), true);
    free_heap_str(binary_name);
    if (!o)
      goto oom;
    E->declaringClass = o;
    o = MakeJStringFromModifiedUTF8(thread, method->name, true);
    if (!o)
      goto oom;
    E->methodName = o;
    attribute_source_file *sf = method->my_class->source_file;
    o = nullptr; // SourceFile 属性が無ければ fileName は null（メソッド名を使い回さない）
    if (sf) {
      o = MakeJStringFromModifiedUTF8(thread, sf->name, true);
      if (!o)
        goto oom;
    }

    E->fileName = o;
    E->lineNumber = line;
    *((void **)ArrayData(stack_trace->obj) + j) = e->obj;
#undef E
    drop_handle(thread, e);
    frame = frame->prev;
    continue;
  oom:
    drop_handle(thread, e);
    break;
  }

  object result = stack_trace->obj;
  drop_handle(thread, stack_trace);
  return result;
}

int count_frames(stack_frame *frame) {
  int n_frames = 0;
  for (; frame; frame = frame->prev)
    ++n_frames;
  return n_frames;
}

DECLARE_NATIVE("java/lang", Throwable, fillInStackTrace, "(I)Ljava/lang/Throwable;") {
  // Called in the constructor of Throwable. We therefore need to ignore
  // frames which are constructing the current object, which we can do by
  // inspecting the stack.
  stack_frame *frame = thread->stack.top;
  while (frame) {
    if (!is_frame_part_of_throwable_construction(frame, obj->obj)) {
      break;
    }
    frame = frame->prev;
  }

  int n_frames = count_frames(frame);
  object stack_trace = build_stack_trace(thread, frame, n_frames);
  if (stack_trace) {
    ((struct native_Throwable *)obj->obj)->depth = n_frames;
    *backtrace_object(obj->obj) = stack_trace;
  }
  return (stack_value){.obj = obj->obj};
}

/**
 * ほかのスレッドのスタック（`Thread.getStackTrace()`）。スレッドダンプや、JUnit のタイムアウトの報告、
 * 走っているプログラムを外から覗くサンプラーが使う。走っていないスレッドなら null（JDK が空の配列にする）。
 *
 * b-jvm のスレッドは 1 つの wasm スレッドの上で順に回るので、ほかのスレッドは必ず止まった点（譲った点）
 * にいて、そのフレームはそのまま読める
 */
DECLARE_NATIVE("java/lang", Thread, getStackTrace0, "()Ljava/lang/Object;") {
  vm_thread *target = nullptr;
  for (int i = 0; i < arrlen(thread->vm->active_threads); ++i) {
    if ((object)thread->vm->active_threads[i]->thread_obj == obj->obj) {
      target = thread->vm->active_threads[i];
      break;
    }
  }
  if (!target)
    return value_null();
  stack_frame *frame = target->stack.top;
  return (stack_value){.obj = build_stack_trace(thread, frame, count_frames(frame))};
}

DECLARE_NATIVE("java/lang", Throwable, getStackTraceDepth, "()I") {
  DCHECK(argc == 0);
  return (stack_value){.i = ArrayLength(*backtrace_object(obj->obj))};
}

DECLARE_NATIVE("java/lang", Throwable, getStackTraceElement, "(I)Ljava/lang/StackTraceElement;") {
  DCHECK(argc == 1);
  obj_header *stack_trace = *backtrace_object(obj->obj);
  int index = args[0].i;
  if (index < 0 || index >= ArrayLength(stack_trace)) {
    return value_null();
  }
  obj_header *element = *((obj_header **)ArrayData(stack_trace) + index);
  return (stack_value){.obj = element};
}

DECLARE_NATIVE("java/lang", StackTraceElement, initStackTraceElements,
               "([Ljava/lang/StackTraceElement;Ljava/lang/Object;I)V") {
  handle *stack_trace = args[1].handle;
  int depth = ArrayLength(stack_trace->obj);
  int array_length = ArrayLength(args[0].handle->obj);
  if (array_length < depth) {
    depth = array_length;
  }
  for (int i = 0; i < depth; ++i) {
    obj_header *element = *((obj_header **)ArrayData(stack_trace->obj) + i);
    ReferenceArrayStore(args[0].handle->obj, i, element);
  }
  return value_null();
}
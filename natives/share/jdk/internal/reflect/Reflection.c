#include "natives-dsl.h"

static bool starts_with(slice s, const char *prefix) {
  size_t len = strlen(prefix);
  return s.len >= len && strncmp(s.chars, prefix, len) == 0;
}

// HotSpot の Method::is_ignored_by_security_stack_walk と同じものを飛ばす: Method.invoke、
// 反射の accessor の実装（jdk/internal/reflect）、MethodHandle の intrinsic、
// LambdaForm が生んだフレーム。
// java/lang/invoke の下を丸ごと飛ばすと、MethodHandleImpl$BindCaller.checkCallerClass のように
// 自分の呼び出し元が java/lang/invoke のクラスであることを確かめる箇所が
// "found java.lang.reflect.Method, expected java.lang.invoke.MethodHandleImpl$BindCaller" で落ちる
bool ignore_frame_for_get_caller_class(stack_frame *frame) {
  cp_method *method = frame->method;
  slice name = method->my_class->name;
  if (utf8_equals(name, "java/lang/reflect/Method") && utf8_equals(method->name, "invoke"))
    return true;
  // MethodHandle.invokeExact / invoke / invokeBasic / linkTo*（HotSpot の method handle intrinsic）
  if (method->is_signature_polymorphic)
    return true;
  if (starts_with(name, "jdk/internal/reflect/"))
    return true;
  if (starts_with(name, "java/lang/invoke/LambdaForm$") || utf8_equals(name, "java/lang/invoke/LambdaForm$Holder") ||
      utf8_equals(name, "java/lang/invoke/DirectMethodHandle$Holder") ||
      utf8_equals(name, "java/lang/invoke/DelegatingMethodHandle$Holder") ||
      utf8_equals(name, "java/lang/invoke/Invokers$Holder"))
    return true;
  if (starts_with(name, "sun/util/invoke"))
    return true;
  return false;
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, getCallerClass, "()Ljava/lang/Class;") {
  // Look a couple frames before the current frame
  int i = 2;
  stack_frame *frame = thread->stack.top;
  while (frame && i > 0) {
    frame = frame->prev;
    --i;
  }
  while (frame && ignore_frame_for_get_caller_class(frame))
    frame = frame->prev;
  if (frame == nullptr)
    return value_null();
  return (stack_value){.obj = (void *)get_class_mirror(thread, frame->method->my_class)};
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, getClassAccessFlags, "(Ljava/lang/Class;)I") {
  obj_header *obj_ = args[0].handle->obj;
  classdesc *classdesc = unmirror_class(obj_);
  return (stack_value){.i = classdesc->access_flags};
}

DECLARE_NATIVE("jdk/internal/reflect", Reflection, areNestMates, "(Ljava/lang/Class;Ljava/lang/Class;)Z") {
  // TODO
  return (stack_value){.i = 1};
}
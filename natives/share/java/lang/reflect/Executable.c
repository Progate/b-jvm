#include "bjvm.h"
#include <natives-dsl.h>
#include <reflection.h>

DECLARE_NATIVE("java/lang/reflect", Executable, getParameters0, "()[Ljava/lang/reflect/Parameter;") {
  DCHECK(argc == 0);
  // Could be a Method or a Constructor, check which one
  obj_header *executable = obj->obj;
  cp_method *method;
  slice name = executable->descriptor->name;
  if (utf8_equals(name, "java/lang/reflect/Method")) {
    method = unmirror_method((void *)executable);
  } else if (utf8_equals(name, "java/lang/reflect/Constructor")) {
    method = unmirror_ctor((void *)executable);
  } else {
    return value_null(); // wtf
  }
  obj_header *parameters = reflect_get_method_parameters(thread, method);
  return (stack_value){.obj = parameters};
}

// The raw RuntimeVisibleTypeAnnotations of the method or constructor, or null when it has none
// (like HotSpot's JVM_GetMethodTypeAnnotations). Spring reads these through getAnnotatedReturnType etc.
DECLARE_NATIVE("java/lang/reflect", Executable, getTypeAnnotationBytes0, "()[B") {
  DCHECK(argc == 0);
  obj_header *executable = obj->obj;
  cp_method *method;
  slice name = executable->descriptor->name;
  if (utf8_equals(name, "java/lang/reflect/Method")) {
    method = unmirror_method((void *)executable);
  } else if (utf8_equals(name, "java/lang/reflect/Constructor")) {
    method = unmirror_ctor((void *)executable);
  } else {
    return value_null();
  }
  for (int i = 0; i < method->attributes_count; ++i) {
    attribute *attr = method->attributes + i;
    if (attr->kind != ATTRIBUTE_KIND_RUNTIME_VISIBLE_TYPE_ANNOTATIONS)
      continue;
    attribute_runtime_visible_type_annotations r = attr->type_annotations;
    obj_header *array = CreatePrimitiveArray1D(thread, TYPE_KIND_BYTE, r.length);
    if (!array)
      return value_null();
    memcpy(ArrayData(array), r.data, r.length);
    return (stack_value){.obj = array};
  }
  return value_null();
}

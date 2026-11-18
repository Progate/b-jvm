#include "bjvm.h"
#include <natives-dsl.h>
#include <reflection.h>

// The raw RuntimeVisibleTypeAnnotations of the field, or null when it has none
// (like HotSpot's JVM_GetFieldTypeAnnotations)
DECLARE_NATIVE("java/lang/reflect", Field, getTypeAnnotationBytes0, "()[B") {
  DCHECK(argc == 0);
  cp_field *field = *unmirror_field(obj->obj);
  for (int i = 0; i < field->attributes_count; ++i) {
    attribute *attr = field->attributes + i;
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

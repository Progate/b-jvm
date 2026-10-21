#include "arrays.h"
#include "objects.h"

#include <natives-dsl.h>

/**
*Module module,
boolean isOpen,
String version,
String location,
Object[] pns
*/
DECLARE_NATIVE("java/lang", Module, defineModule0,
               "(Ljava/lang/Module;ZLjava/lang/String;Ljava/lang/String;[Ljava/lang/Object;)V") {
  DCHECK(argc == 5);

  obj_header *name = LoadFieldObject(args[0].handle->obj, "java/lang/String", "name");
  heap_string s;
  if (read_string_to_utf8(thread, &s, name)) {
    return value_null(); // oom
  }

  module *mod = define_module(thread->vm, hslc(s), args[0].handle->obj);
  free_heap_str(s);

  // pns lists the module's packages in dotted form. Record them so that classes loaded later (and already-loaded
  // ones) are attributed to this module rather than to java.base
  obj_header *pns = args[4].handle->obj;
  if (mod && pns) {
    for (int i = 0; i < ArrayLength(pns); ++i) {
      obj_header *pn = ReferenceArrayLoad(pns, i);
      if (!pn)
        continue;
      heap_string package_name;
      if (read_string_to_utf8(thread, &package_name, pn)) {
        return value_null(); // oom
      }
      for (u32 j = 0; j < package_name.len; ++j)
        if (package_name.chars[j] == '.')
          package_name.chars[j] = '/';
      add_module_package(thread->vm, mod, hslc(package_name));
      free_heap_str(package_name);
    }
  }

  return value_null();
}

DECLARE_NATIVE("java/lang", Module, addReads0, "(Ljava/lang/Module;Ljava/lang/Module;)V") {
  DCHECK(argc == 2);
  return value_null();
}

DECLARE_NATIVE("java/lang", Module, addExportsToAll0, "(Ljava/lang/Module;Ljava/lang/String;)V") {
  DCHECK(argc == 2);
  return value_null();
}

DECLARE_NATIVE("java/lang", Module, addExports0, "(Ljava/lang/Module;Ljava/lang/String;Ljava/lang/Module;)V") {
  DCHECK(argc == 3);
  return value_null();
}
/**
 * bjvm.SystemClassLoader のリソース検索。
 *
 * クラスパス上の各エントリ（ディレクトリ・jar）からリソース名を探し、
 * 実ファイルとして読めるパスの一覧を返す。jar 内のエントリは
 * /bjvm-resources/ 配下に展開してからパスを返す
 */
#include <natives-dsl.h>

#define BJVM_RESOURCE_DIR "/bjvm-resources"

DECLARE_NATIVE("bjvm", SystemClassLoader, findResources0, "(Ljava/lang/String;)[Ljava/lang/String;") {
  DCHECK(argc == 1);
  heap_string name = AsHeapString(args[0].handle->obj, on_oom);
  heap_string *paths = classpath_find_resources(&thread->vm->bootstrap_classpath, hslc(name), BJVM_RESOURCE_DIR);
  free_heap_str(name);

  int count = arrlen(paths);
  handle *result = make_handle(thread, CreateObjectArray1D(thread, cached_classes(thread->vm)->string, count));
  if (!result->obj)
    goto on_oom;
  for (int i = 0; i < count; ++i) {
    object s = make_jstring_modified_utf8(thread, hslc(paths[i]));
    if (!s) {
      drop_handle(thread, result);
      goto on_oom;
    }
    ReferenceArrayStore(result->obj, i, s);
    free_heap_str(paths[i]);
  }
  arrfree(paths);
  return (stack_value){.obj = result->obj};

on_oom:
  return value_null();
}

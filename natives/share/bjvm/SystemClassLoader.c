/**
 * bjvm.SystemClassLoader のリソース検索。
 *
 * クラスパス上の各エントリ（ディレクトリ・jar）からリソース名を探し、在りかの一覧を返す。
 * jar 内のものは `jar:<jar の絶対パス>!/<名前>`、ディレクトリのものは実ファイルのパス
 * （URL に組むのは SystemClassLoader.java）
 */
#include <natives-dsl.h>

DECLARE_NATIVE("bjvm", SystemClassLoader, findResources0, "(Ljava/lang/String;)[Ljava/lang/String;") {
  DCHECK(argc == 1);
  heap_string name = AsHeapString(args[0].handle->obj, on_oom);
  heap_string *paths = classpath_find_resources(&thread->vm->bootstrap_classpath, hslc(name));
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

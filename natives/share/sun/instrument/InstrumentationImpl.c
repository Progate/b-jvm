// sun.instrument.InstrumentationImpl の VM 連携面。
// JVMTI を持たないので、バイトコード変換（retransform/redefine/prefix）は
// 「この環境では未対応」を返す。一方で -javaagent の premain 起動や
// appendToClassLoaderSearch・getAllLoadedClasses といった「変換しない系」は本実装にする
#include <natives-dsl.h>
#include <classpath.h>
#include <classloader.h>
#include <ctype.h>
#include <string.h>

// ネイティブエージェント（libinstrument）は wasm にはロードできないので、nativeAgent の
// ダミー値として使う ID。JVMTI 相当の処理が必要なメソッドは別途未対応を返す
#define FAKE_NATIVE_AGENT 1

// MANIFEST.MF のメインセクション（最初の空行まで）から `name:` の値を取り出す。
// 名前の比較は大文字小文字を区別しない（JAR 仕様）。継続行は先頭 1 スペースで連結される
static heap_string manifest_main_attr(const u8 *bytes, size_t len, slice name) {
  size_t pos = 0;
  while (pos < len) {
    // 1 行の終わり（CRLF / LF 両対応）を探す
    size_t eol = pos;
    while (eol < len && bytes[eol] != '\n' && bytes[eol] != '\r')
      ++eol;
    size_t next = eol;
    if (next < len && bytes[next] == '\r')
      ++next;
    if (next < len && bytes[next] == '\n')
      ++next;
    if (eol == pos)
      break; // 空行 = メインセクションの終わり
    if (bytes[pos] == ' ') { // 継続行。属性名は取れないのでスキップ
      pos = next;
      continue;
    }
    // `Key: value` の形を持つ行だけを見る
    size_t colon = pos;
    while (colon < eol && bytes[colon] != ':')
      ++colon;
    if (colon < eol) {
      slice key = {.chars = (char *)bytes + pos, .len = colon - pos};
      if (key.len == name.len) {
        bool match = true;
        for (u32 i = 0; i < key.len; ++i) {
          if (tolower((u8)key.chars[i]) != tolower((u8)name.chars[i])) {
            match = false;
            break;
          }
        }
        if (match) {
          size_t v = colon + 1;
          if (v < eol && bytes[v] == ' ')
            ++v;
          // 継続行を畳み込む
          heap_string value = make_heap_str(eol - v);
          memcpy(value.chars, bytes + v, eol - v);
          size_t cont = next;
          while (cont < len && bytes[cont] == ' ') {
            size_t c = cont + 1;
            while (c < len && bytes[c] != '\n' && bytes[c] != '\r')
              ++c;
            heap_str_append(&value, (slice){.chars = (char *)bytes + cont + 1, .len = c - cont - 1});
            cont = c;
            if (cont < len && bytes[cont] == '\r')
              ++cont;
            if (cont < len && bytes[cont] == '\n')
              ++cont;
          }
          return value;
        }
      }
    }
    pos = next;
  }
  return (heap_string){nullptr};
}

// -javaagent:<jar>[=<options>] 相当の文字列が渡る。
// HotSpot の libinstrument と同じく、jar 自体はここ（ネイティブ側）で開いて
// マニフェストを読み、Premain-Class を loadClassAndCallPremain へ渡す。
// jar 内のマニフェストは VM の jar マップ（jar_lookup）が読む
DECLARE_ASYNC_NATIVE("sun/instrument", InstrumentationImpl, loadAgent0, "(Ljava/lang/String;)V",
                     locals(heap_string spec; handle *opts_jstr; handle *premain; handle *inst;),
                     invoked_methods(invoked_method(call_interpreter))) {
  if (read_string_to_utf8(thread, &self->spec, args[0].handle->obj)) {
    ASYNC_RETURN_VOID(); // pending exception
  }

  // 引数の形式は `jarpath=options`（= 以降はすべて options）
  slice path = (slice){.chars = self->spec.chars, .len = self->spec.len};
  slice opts = {.chars = nullptr, .len = 0};
  for (u32 i = 0; i < self->spec.len; ++i) {
    if (self->spec.chars[i] == '=') {
      path.len = i;
      opts = subslice_to(hslc(self->spec), i + 1, self->spec.len);
      break;
    }
  }

  // 先に jar をクラスパスへ積む。アクセス不能/zip 破損はここで弾かれる
  char *cp_error = classpath_add_entry(&thread->vm->bootstrap_classpath, path);
  if (cp_error) {
    heap_string msg = make_heap_str_from(STR("Unable to access jarfile "));
    heap_str_append(&msg, path);
    heap_str_append(&msg, STR(": "));
    heap_str_append(&msg, (slice){.chars = cp_error, .len = strlen(cp_error)});
    free(cp_error);
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), hslc(msg));
    free_heap_str(msg);
    goto done;
  }

  // マニフェストを直接読む（libinstrument の JLI 相当部分）。
  // 他エントリの同名ファイルを拾わないよう、直前に積んだエントリだけを見る
  u8 *man_bytes = nullptr;
  size_t man_len = 0;
  enum jar_lookup_result man_result = JAR_CORRUPT;
  classpath_entry *added = &arrlast(thread->vm->bootstrap_classpath.entries);
  if (added->jar) {
    man_result = jar_lookup(added->jar, STR("META-INF/MANIFEST.MF"), &man_bytes, &man_len);
  }
  if (man_result != JAR_FOUND) {
    heap_string msg = make_heap_str_from(STR("no manifest found in agent jar "));
    heap_str_append(&msg, path);
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), hslc(msg));
    free_heap_str(msg);
    goto done;
  }

  heap_string premain_name = manifest_main_attr(man_bytes, man_len, STR("Premain-Class"));
  // JAR の Can-* 属性をそのままコンストラクタの capability に反映させる。
  // redefine/retransform は JVMTI が無いので常に false（下の ctor 引数も参照）
  if (!premain_name.chars) {
    free(man_bytes);
    heap_string msg = make_heap_str_from(STR("Failed to find Premain-Class manifest attribute in "));
    heap_str_append(&msg, path);
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), hslc(msg));
    free_heap_str(msg);
    goto done;
  }
  free(man_bytes);

  self->opts_jstr = opts.chars ? make_handle(thread, MakeJStringFromModifiedUTF8(thread, opts, true))
                             : &thread->null_handle;
  self->premain = make_handle(thread, MakeJStringFromModifiedUTF8(thread, hslc(premain_name), true));
  free_heap_str(premain_name);
  if ((opts.chars && !self->opts_jstr->obj) || !self->premain->obj) {
    goto done; // OOM は pending exception で伝わる
  }

  // JVMTI が無いので redefine/retransform/native-prefix はすべて未対応として生成する
  self->inst = make_handle(thread, new_object(thread, bootstrap_lookup_class(thread, STR("sun/instrument/InstrumentationImpl"))));
  if (thread->current_exception) {
    goto done;
  }
  {
    classdesc *Impl = self->inst->obj->descriptor;
    cp_method *init = method_lookup(Impl, STR("<init>"), STR("(JZZZ)V"), false, false);
    DCHECK(init);
    stack_value ctor_args[] = {{.obj = self->inst->obj}, {.l = FAKE_NATIVE_AGENT}, {.i = 0}, {.i = 0}, {.i = 0}};
    AWAIT(call_interpreter, thread, init, ctor_args);
    if (thread->current_exception) {
      goto done;
    }
    cp_method *premain = method_lookup(Impl, STR("loadClassAndCallPremain"),
                                     STR("(Ljava/lang/String;Ljava/lang/String;)V"), false, false);
    DCHECK(premain);
    stack_value call_args[] = {{.obj = self->inst->obj},
                               {.obj = self->premain->obj},
                               {.obj = self->opts_jstr->obj}};
    AWAIT(call_interpreter, thread, premain, call_args);
  }

done:
  free_heap_str(self->spec);
  ASYNC_END_VOID();
}

// native 側エージェントの jar パス。libinstrument が無いので保持していない
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, jarFile, "(J)Ljava/lang/String;") {
  return value_null();
}

// どのクラスも「概念的には」変形可能（実際の変換は無いが、API としては modifiable 扱い）
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, isModifiableClass0, "(JLjava/lang/Class;)Z") {
  return (stack_value){.i = 1};
}

// JVMTI を持たないので再変換は未対応
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, isRetransformClassesSupported0, "(J)Z") {
  return (stack_value){.i = 0};
}

DECLARE_NATIVE("sun/instrument", InstrumentationImpl, setHasTransformers, "(JZ)V") { return value_null(); }

DECLARE_NATIVE("sun/instrument", InstrumentationImpl, setHasRetransformableTransformers, "(JZ)V") {
  return value_null();
}

DECLARE_NATIVE("sun/instrument", InstrumentationImpl, retransformClasses0, "(J[Ljava/lang/Class;)V") {
  raise_vm_exception_no_msg(thread, STR("java/lang/UnsupportedOperationException"));
  return value_null();
}

DECLARE_NATIVE("sun/instrument", InstrumentationImpl, redefineClasses0,
               "(J[Ljava/lang/instrument/ClassDefinition;)V") {
  raise_vm_exception_no_msg(thread, STR("java/lang/UnsupportedOperationException"));
  return value_null();
}

// クラスローダの台帳（loaded/initiating）からミラーを集める
static int collect_class_mirrors(vm_thread *thread, string_hash_table *table, obj_header ***out) {
  *out = nullptr;
  hash_table_iterator it = hash_table_get_iterator(table);
  char *key;
  size_t key_len;
  void *value;
  while (hash_table_iterator_has_next(it, &key, &key_len, &value)) {
    classdesc *desc = value;
    if (desc) {
      obj_header *mirror = (obj_header *)get_class_mirror(thread, desc);
      if (mirror) {
        arrput(*out, mirror);
      }
    }
    hash_table_iterator_next(&it);
  }
  return arrlen(*out);
}

static stack_value mirrors_to_array(vm_thread *thread, obj_header **mirrors, int count) {
  obj_header *arr = CreateObjectArray1D(thread, cached_classes(thread->vm)->klass, count);
  for (int i = 0; i < count; ++i) {
    *((void **)ArrayData(arr) + i) = mirrors[i];
  }
  arrfree(mirrors);
  return (stack_value){.obj = arr};
}

DECLARE_NATIVE("sun/instrument", InstrumentationImpl, getAllLoadedClasses0, "(J)[Ljava/lang/Class;") {
  obj_header **mirrors = nullptr;
  for (int i = 0; i < arrlen(thread->vm->active_classloaders); ++i) {
    collect_class_mirrors(thread, &thread->vm->active_classloaders[i]->loaded, &mirrors);
  }
  return mirrors_to_array(thread, mirrors, arrlen(mirrors));
}

// 引数のローダが「initiate」したクラス。initiating テーブルを使う
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, getInitiatedClasses0,
               "(JLjava/lang/ClassLoader;)[Ljava/lang/Class;") {
  classloader *cl = unmirror_classloader(thread->vm, args[1].handle->obj);
  if (!cl) {
    return (stack_value){.obj = nullptr};
  }
  obj_header **mirrors = nullptr;
  int count = collect_class_mirrors(thread, &cl->initiating, &mirrors);
  return mirrors_to_array(thread, mirrors, count);
}

// 浅いサイズ（ヘッダ＋フィールド領域）。配列は要素分も足す。JDK の契約も「近似値」なのでこれで足りる
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, getObjectSize0, "(JLjava/lang/Object;)J") {
  obj_header *o = args[1].handle->obj;
  if (!o) {
    return (stack_value){.l = 0};
  }
  s64 size = (s64)o->descriptor->instance_bytes;
  if (o->descriptor->kind == CD_KIND_PRIMITIVE_ARRAY || o->descriptor->kind == CD_KIND_ORDINARY_ARRAY) {
    // instance_bytes は配列では未使用。ヘッダ + 要素サイズ * 長さ
    classdesc *base = o->descriptor->base_component;
    s64 elem = o->descriptor->kind == CD_KIND_PRIMITIVE_ARRAY ? sizeof_type_kind(base->primitive_component)
                                                            : (s64)sizeof(void *);
    size = sizeof(obj_header) + elem * ArrayLength(o);
  }
  return (stack_value){.l = size};
}

// Boot-Class-Path の追加先。システムクラスローダの検索パス（=bootstrap_classpath のアプリ側）へ積む
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, appendToClassLoaderSearch0, "(JLjava/lang/String;Z)V") {
  heap_string path;
  if (read_string_to_utf8(thread, &path, args[1].handle->obj)) {
    return value_null();
  }
  char *error = classpath_add_entry(&thread->vm->bootstrap_classpath, hslc(path));
  if (error) {
    heap_string msg = make_heap_str_from(STR("Unable to add to classpath: "));
    heap_str_append(&msg, (slice){.chars = error, .len = strlen(error)});
    free(error);
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), hslc(msg));
    free_heap_str(msg);
  }
  free_heap_str(path);
  return value_null();
}

// native メソッドの prefix 付け替えは JVMTI 前提なので未対応（呼ばれても副作用なしにする）
DECLARE_NATIVE("sun/instrument", InstrumentationImpl, setNativeMethodPrefixes, "(J[Ljava/lang/String;Z)V") {
  return value_null();
}

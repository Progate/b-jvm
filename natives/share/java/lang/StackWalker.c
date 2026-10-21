/**
 * java.lang.StackWalker / StackStreamFactory の native 実装。
 *
 * b-jvm はスレッドのフレームを thread->stack から辿れるので、それをスナップショットして
 * JDK のバッファプロトコル（callStackWalk / fetchStackFrames / setContinuation）に合わせて
 * StackFrame オブジェクトとして流す。
 *
 * CLASS_INFO_ONLY モードでは ClassFrameInfo に Class ミラーを、それ以外では
 * StackFrameInfo に ResolvedMethodName を詰める。ResolvedMethodName は
 * declaringClass() が vmholder フィールドを読むだけの薄いラッパなので、
 * VM 注入の vmtarget を持たせず vmholder だけ埋める最小構成にする
 */
#include <natives-dsl.h>

// JDK の StackStreamFactory と同じモードビット
#define CLASS_INFO_ONLY 0x2
#define FILL_LIVE_STACK_FRAMES 0x100

// callStackWalk が切り取ったフレーム列のスナップショット。
// doStackWalk 実行中に Java 側のフレームが伸びても影響されないよう、参照をコピーして持つ
typedef struct {
  stack_frame **frames;
  int n;
  int pos;
} walk_snapshot;

// StackWalker 機構自身のフレーム（walk → beginStackWalk → callStackWalk と続く部分）は
// ストリームに載せない。java/lang/Stack* で始まるクラスをスキップする
static bool is_walker_internal_class(const classdesc *cd) {
  static const char prefix[] = "java/lang/Stack";
  return cd->name.len >= sizeof(prefix) - 1 && memcmp(cd->name.chars, prefix, sizeof(prefix) - 1) == 0;
}

static walk_snapshot *take_snapshot(vm_thread *thread, int skip) {
  int n = 0;
  for (stack_frame *frame = thread->stack.top; frame; frame = frame->prev) {
    if (!is_walker_internal_class(frame->method->my_class)) n++;
  }

  walk_snapshot *snap = malloc(sizeof(walk_snapshot) + n * sizeof(stack_frame *));
  CHECK(snap);
  snap->frames = (stack_frame **)(snap + 1);
  snap->n = n;
  // skip は先頭（= 最新のフレーム）から数える
  snap->pos = skip < n ? skip : n;

  int i = 0;
  for (stack_frame *frame = thread->stack.top; frame && i < n; frame = frame->prev) {
    if (!is_walker_internal_class(frame->method->my_class)) snap->frames[i++] = frame;
  }
  return snap;
}

// java/lang/invoke/ResolvedMethodName のレイアウト。bjvm.c で登録した
// class_padding により、obj_header の直後に vmtarget が来る
struct native_ResolvedMethodName {
  obj_header base;
  cp_method *vmtarget;
  obj_header *vmholder; // Ljava/lang/Class;
};

// StackFrameInfo が必要とする ResolvedMethodName を同期で作る。
// declaringClass() は vmholder、expandStackFrameInfo は vmtarget を参照する
static object make_resolved_method_name_for_frame(vm_thread *thread, const stack_frame *frame) {
  classdesc *RMN = bootstrap_lookup_class(thread, STR("java/lang/invoke/ResolvedMethodName"));
  DCHECK(RMN);
  object rmn = new_object(thread, RMN);
  if (!rmn) return nullptr;

  object mirror = (void *)get_class_mirror(thread, frame->method->my_class);
  if (!mirror) return nullptr;

  struct native_ResolvedMethodName *n = (void *)rmn;
  n->vmtarget = frame->method;
  n->vmholder = mirror;
  return rmn;
}

// frames 配列の startIndex 以降へバッチを詰める。戻り値は詰めた数、失敗は -1
static int fill_batch(vm_thread *thread, walk_snapshot *snap, int mode, int buffer_size, int start_index,
                      object frames_array, classdesc *element_class, bool class_info_only, object cont_scope) {
  int capacity = ArrayLength(frames_array) - start_index;
  if (buffer_size < capacity) capacity = buffer_size;

  int count = 0;
  while (count < capacity && snap->pos < snap->n) {
    stack_frame *frame = snap->frames[snap->pos++];

    // JDK ではフレームバッファがあらかじめ ClassFrameInfo/StackFrameInfo を
    // コンストラクタ付きで詰めて渡すので、VM はそのフィールドを埋めるだけ。
    // 万が一 null の要素があれば新規に割り当てる
    object elem = *((obj_header **)ArrayData(frames_array) + start_index + count);
    if (!elem) {
      elem = new_object(thread, element_class);
      *((obj_header **)ArrayData(frames_array) + start_index + count) = elem;
    }
    handle *info = make_handle(thread, elem);
    if (!info->obj) return -1; // OOM（例外は new_object が投げている）

    // flags: コンストラクタの RETAIN_CLASS_REF_BIT を残しつつ、アクセスフラグを足す
    //（isCallerSensitive / isHidden の判定に使われる）
    stack_value flags = __obj_load_field(info->obj, STR("flags"), STR("I"));
    __obj_store_field(info->obj, STR("flags"),
                      (stack_value){.i = flags.i | (frame->method->access_flags & 0xffff)}, STR("I"));

    if (class_info_only) {
      object mirror = (void *)get_class_mirror(thread, frame->method->my_class);
      if (!mirror) {
        drop_handle(thread, info);
        continue; // ミラーが作れないクラスのフレームは読み飛ばす
      }
      StoreFieldObject(info->obj, "java/lang/Object", "classOrMemberName", mirror);
    } else {
      object rmn = make_resolved_method_name_for_frame(thread, frame);
      if (!rmn) {
        drop_handle(thread, info);
        continue;
      }
      StoreFieldObject(info->obj, "java/lang/Object", "classOrMemberName", rmn);
      int bci = -1;
      if (!is_frame_native(frame) && frame->method->code)
        bci = frame->method->code->code[frame->program_counter].original_pc;
      __obj_store_field(info->obj, STR("bci"), (stack_value){.i = bci}, STR("I"));
      if (cont_scope)
        StoreFieldObject(info->obj, "jdk/internal/vm/ContinuationScope", "contScope", cont_scope);
    }

    drop_handle(thread, info);
    count++;
  }
  (void)mode;
  return count;
}

// mode に応じた要素クラスを返す
static classdesc *frame_info_class(vm_thread *thread, int mode) {
  if (mode & CLASS_INFO_ONLY) return bootstrap_lookup_class(thread, STR("java/lang/ClassFrameInfo"));
  if (mode & FILL_LIVE_STACK_FRAMES)
    return bootstrap_lookup_class(thread, STR("java/lang/LiveStackFrameInfo"));
  return bootstrap_lookup_class(thread, STR("java/lang/StackFrameInfo"));
}

static int ensure_initialized(vm_thread *thread, classdesc *cd) {
  if (!cd) return -1;
  initialize_class_t ctx = {.args = {thread, cd}};
  thread->stack.synchronous_depth++;
  future_t f = initialize_class(&ctx);
  thread->stack.synchronous_depth--;
  return f.status == FUTURE_READY && !ctx._result ? 0 : -1;
}

DECLARE_NATIVE("java/lang", StackStreamFactory, checkStackWalkModes, "()Z") {
  return (stack_value){.i = true};
}

DECLARE_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, callStackWalk,
               "(IILjdk/internal/vm/ContinuationScope;Ljdk/internal/vm/Continuation;II[Ljava/lang/Object;)"
               "Ljava/lang/Object;") {
  int mode = args[0].i;
  int skip = args[1].i;
  // args[2] = ContinuationScope, args[3] = Continuation（virtual thread 未対応なので無視）
  int buffer_size = args[4].i;
  int start_index = args[5].i;
  handle *frames_array = args[6].handle;

  bool class_info_only = mode & CLASS_INFO_ONLY;
  classdesc *element_class = frame_info_class(thread, mode);
  if (ensure_initialized(thread, element_class)) return value_null();
  if (!class_info_only &&
      ensure_initialized(thread, bootstrap_lookup_class(thread, STR("java/lang/invoke/ResolvedMethodName"))))
    return value_null();

  walk_snapshot *snap = take_snapshot(thread, skip);
  int count = fill_batch(thread, snap, mode, buffer_size, start_index, frames_array->obj, element_class,
                         class_info_only, args[2].handle->obj);
  if (count < 0) {
    free(snap);
    return value_null();
  }
  // doStackWalk が anchor==0/-1 を「無効なストリーム」とみなすので、スナップショットを
  // 使い切っていても 0 以外の値を渡す必要がある。使い切り時に Java 側が追加の
  // fetchStackFrames を呼ばない場合はスナップショットがリークする（1ウォークあたり
  // 数十〜数百バイト程度なので許容とする）
  s64 anchor = (s64)(uintptr_t)snap;

  // AbstractStackWalker::doStackWalk(anchor, skipFrames, numFrames, bufStartIndex, bufEndIndex)
  // を呼び、その戻り値をそのまま返す
  cp_method *do_stack_walk =
      method_lookup(obj->obj->descriptor, STR("doStackWalk"), STR("(JIIII)Ljava/lang/Object;"), true, false);
  DCHECK(do_stack_walk);
  return call_interpreter_synchronous(
      thread, do_stack_walk,
      (stack_value[]){
          {.obj = obj->obj}, {.l = anchor}, {.i = skip}, {.i = count}, {.i = start_index}, {.i = start_index + count},
      });
}

DECLARE_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, fetchStackFrames,
               "(IJIII[Ljava/lang/Object;)I") {
  int mode = args[0].i;
  walk_snapshot *snap = (walk_snapshot *)(uintptr_t)args[1].l;
  // args[2] = lastBatchFrameCount（VM が batch を引き継ぐための情報。こちらでは不要）
  int buffer_size = args[3].i;
  int start_index = args[4].i;
  handle *frames_array = args[5].handle;

  if (!snap) return (stack_value){.i = 0};

  classdesc *element_class = frame_info_class(thread, mode);
  DCHECK(element_class);
  int count = fill_batch(thread, snap, mode, buffer_size, start_index, frames_array->obj, element_class,
                         mode & CLASS_INFO_ONLY, nullptr);
  if (count < 0 || snap->pos >= snap->n) {
    free(snap);
  }
  return (stack_value){.i = count < 0 ? 0 : count};
}

// StackFrameInfo.expandStackFrameInfo: getMethodName / getMethodType /
// toStackTraceElement などで必要になる name / type フィールドを埋める
DECLARE_NATIVE("java/lang", StackFrameInfo, expandStackFrameInfo, "()V") {
  object rmn = LoadFieldObject(obj->obj, "java/lang/Object", "classOrMemberName");
  if (!rmn) return value_null();
  struct native_ResolvedMethodName *n = (void *)rmn;
  cp_method *method = n->vmtarget;
  DCHECK(method);

  StoreFieldObject(obj->obj, "java/lang/String", "name", make_jstring_modified_utf8(thread, method->name));
  // type は "String or MethodType"。getMethodType() は MethodType 以外を
  // MethodType に変換してくれるので、シグネチャの文字列を入れておく
  StoreFieldObject(obj->obj, "java/lang/Object", "type",
                   make_jstring_modified_utf8(thread, method->unparsed_descriptor));
  return value_null();
}

DECLARE_NATIVE("java/lang", StackStreamFactory$AbstractStackWalker, setContinuation,
               "(J[Ljava/lang/Object;Ljdk/internal/vm/Continuation;)V") {
  // virtual thread の Continuation は未対応。親をたどる必要がないので何もしない
  return value_null();
}

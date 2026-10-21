// Continuation（仮想スレッドの基盤）。
// b-jvm のスタックは VM が管理する連続バッファなので、HotSpot のような継続の
// スタック凍結・復元は実装しない。代わりに「常に pinned」な継続として扱う:
// enterSpecial は呼ばれたスレッドのスタックのまま Continuation.enter を同期的に呼び、
// yield は常に失敗（pinned）を返す。pinned は JEP 444/491 上合法な振る舞いであり、
// synchronized や native 呼出し中の継続と同じくキャリアから降りずに走りきる。
#include <natives-dsl.h>

DECLARE_NATIVE("jdk/internal/vm", ContinuationSupport, isSupported0, "()Z") {
  return (stack_value){.i = 1};
}

DECLARE_NATIVE("jdk/internal/vm", Continuation, registerNatives, "()V") { return value_null(); }

// 継続の本体を現在のスレッド上で実行する。
// enter は内部で enter0（target.run()）を呼び、完了後に finish（done 化）を行う。
// スタックを差し替えないので isContinue（中断からの再開）は来ないが、一応そのまま渡す
DECLARE_ASYNC_NATIVE("jdk/internal/vm", Continuation, enterSpecial, "(Ljdk/internal/vm/Continuation;ZZ)V",
                     locals(), invoked_methods(invoked_method(call_interpreter))) {
  object cont = args[0].handle->obj;
  cp_method *enter =
      method_lookup(cont->descriptor, STR("enter"), STR("(Ljdk/internal/vm/Continuation;Z)V"), true, false);
  DCHECK(enter);
  stack_value enter_args[] = {(stack_value){.obj = cont}, (stack_value){.i = args[1].i}};
  AWAIT(call_interpreter, thread, enter, enter_args);
  ASYNC_END_VOID();
}

// pinned 理由コードは HotSpot の Pinned 列挙（0=未pinned）に 1 加えた値。
// 常に pinned として NATIVE を返すので、postPinnedEvent の表示内容も実情と合う
#define PINNED_REASON_NATIVE 2

// 継続の yield は常に失敗させる。VirtualThread 側は yield 失敗を pinned と解釈して
// アンマウントを諦め、キャリア上で実行を続ける
DECLARE_NATIVE("jdk/internal/vm", Continuation, doYield, "()I") {
  return (stack_value){.i = PINNED_REASON_NATIVE};
}

DECLARE_NATIVE("jdk/internal/vm", Continuation, pin, "()V") { return value_null(); }

DECLARE_NATIVE("jdk/internal/vm", Continuation, unpin, "()V") { return value_null(); }

DECLARE_NATIVE("jdk/internal/vm", Continuation, isPinned0, "(Ljdk/internal/vm/ContinuationScope;)I") {
  return (stack_value){.i = PINNED_REASON_NATIVE};
}

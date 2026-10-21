#include <natives-dsl.h>
#include <roundrobin_scheduler.h>

DECLARE_NATIVE("java/lang", Runtime, availableProcessors, "()I") { return (stack_value){.i = 1}; }

DECLARE_NATIVE("java/lang", Runtime, totalMemory, "()J") { return (stack_value){.l = (s64)thread->vm->heap_used}; }

DECLARE_NATIVE("java/lang", Runtime, freeMemory, "()J") { return (stack_value){.l = (s64)(thread->vm->heap_capacity - thread->vm->heap_used)}; }

DECLARE_NATIVE("java/lang", Runtime, maxMemory, "()J") { return (stack_value){.l = LLONG_MAX}; }

DECLARE_NATIVE("java/lang", Runtime, gc, "()V") {
  major_gc(thread->vm);
  return value_null();
}

DECLARE_NATIVE("java/lang", Shutdown, beforeHalt, "()V") { return value_null(); }

// 終了コードを記録し、呼んだスレッドを二度と再開しない（起きる時刻 0 の sleep は、割り込まれない限り
// 眠り続ける）。解釈器を外から巻き戻す手段は無いので、止めるのはホスト側の駆動ループである:
// ffi_exit_requested を見て回すのをやめ、記録した終了コードを返す（→ browser-java の src/vm.ts）。
// 以前は記録だけして戻っていたので、System.exit のあともコードが走り続けた（kotlinc はそれを検出して
// "System.exit returned normally" の例外を投げ、コンパイルエラーのたびにスタックが出ていた）
DECLARE_ASYNC_NATIVE("java/lang", Shutdown, halt0, "(I)V", locals(rr_wakeup_info wakeup_info), invoked_methods()) {
  // Truncate to 8 bits like a process exit status would be. Besides being faithful, it keeps -1
  // usable as "System.exit was not called": System.exit(-1) is a common way for a batch tool to
  // report failure, and it becomes 255 here just as it would for the java launcher.
  thread->vm->exit_status = args[0].i & 0xff;
  for (;;) {
    thread->thread_obj->interrupted = false;
    self->wakeup_info.kind = RR_WAKEUP_SLEEP;
    self->wakeup_info.wakeup_us = 0;
    ASYNC_YIELD((void *)&self->wakeup_info);
  }
  ASYNC_END_VOID();
}
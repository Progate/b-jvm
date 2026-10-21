// java.lang.VirtualThread の VM 連携面。
// JVMTI 通知系はエージェントが存在しないので通知先がなく、常に no-op でよい。
// takeVirtualThreadListToUnblock はパーク解除待ちの VT 一覧を取り出す仕組みだが、
// b-jvm では VT がキャリア上で pinned に走りきるので滞留リストが作られず null でよい
#include <natives-dsl.h>

DECLARE_NATIVE("java/lang", VirtualThread, registerNatives, "()V") { return value_null(); }

// JFR の jdk.VirtualThreadPinned イベント相当の通知。イベント購読者がいないので捨てる
DECLARE_NATIVE("java/lang", VirtualThread, postPinnedEvent, "(Ljava/lang/String;)V") { return value_null(); }

DECLARE_NATIVE("java/lang", VirtualThread, notifyJvmtiStart, "()V") { return value_null(); }

DECLARE_NATIVE("java/lang", VirtualThread, notifyJvmtiEnd, "()V") { return value_null(); }

DECLARE_NATIVE("java/lang", VirtualThread, notifyJvmtiMount, "(Z)V") { return value_null(); }

DECLARE_NATIVE("java/lang", VirtualThread, notifyJvmtiUnmount, "(Z)V") { return value_null(); }

DECLARE_NATIVE("java/lang", VirtualThread, notifyJvmtiDisableSuspend, "(Z)V") { return value_null(); }

// VT はキャリア上で完走するのでアンブロック待ちの VT は発生しない
DECLARE_NATIVE("java/lang", VirtualThread, takeVirtualThreadListToUnblock, "()Ljava/lang/VirtualThread;") {
  return value_null();
}

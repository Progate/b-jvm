#include <natives-dsl.h>

// ネットワークインタフェース列挙のスタブ。WASI にはインタフェース列挙の概念が
// ないので getAll は空配列を返し、SeedGenerator などの初期化だけ通す。
// 実際のソケット通信は Net/SocketDispatcher 側の実装に依存する。

DECLARE_NATIVE("java/net", NetworkInterface, init, "()V") { return value_null(); }

DECLARE_NATIVE("java/net", NetworkInterface, getAll, "()[Ljava/net/NetworkInterface;") {
  classdesc *ni_class = bootstrap_lookup_class(thread, STR("java/net/NetworkInterface"));
  if (!ni_class)
    return value_null();
  return (stack_value){.obj = CreateObjectArray1D(thread, ni_class, 0)};
}

static stack_value ni_not_found(vm_thread *thread) {
  raise_vm_exception(thread, STR("java/net/SocketException"), STR("no such interface"));
  return value_null();
}

DECLARE_NATIVE("java/net", NetworkInterface, getByName0, "(Ljava/lang/String;)Ljava/net/NetworkInterface;") {
  return ni_not_found(thread);
}
DECLARE_NATIVE("java/net", NetworkInterface, getByIndex0, "(I)Ljava/net/NetworkInterface;") {
  return ni_not_found(thread);
}
DECLARE_NATIVE("java/net", NetworkInterface, boundInetAddress0, "(Ljava/net/InetAddress;)Z") {
  return (stack_value){.i = 0};
}
DECLARE_NATIVE("java/net", NetworkInterface, getByInetAddress0, "(Ljava/net/InetAddress;)Ljava/net/NetworkInterface;") {
  return ni_not_found(thread);
}
DECLARE_NATIVE("java/net", NetworkInterface, isUp0, "(Ljava/lang/String;I)Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("java/net", NetworkInterface, isLoopback0, "(Ljava/lang/String;I)Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("java/net", NetworkInterface, supportsMulticast0, "(Ljava/lang/String;I)Z") {
  return (stack_value){.i = 0};
}
DECLARE_NATIVE("java/net", NetworkInterface, isP2P0, "(Ljava/lang/String;I)Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("java/net", NetworkInterface, getMacAddr0, "([BLjava/lang/String;I)[B") { return value_null(); }
DECLARE_NATIVE("java/net", NetworkInterface, getMTU0, "(Ljava/lang/String;I)I") { return (stack_value){.i = 1500}; }

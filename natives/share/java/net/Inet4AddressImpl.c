#include <natives-dsl.h>
#include "../../sun/nio/ch/bjvm_net.h"

// Name resolution. Guests cannot reach DNS: "localhost" (and the
// local host name, which is also "localhost") resolves to 127.0.0.1 and
// everything else is unknown. Connect-by-name through the gateway is a
// later piece of work.

DECLARE_NATIVE("java/net", Inet4AddressImpl, getLocalHostName, "()Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, lookupAllHostAddr,
               "(Ljava/lang/String;)[Ljava/net/InetAddress;") {
  obj_header *host = args[0].handle->obj;
  heap_string hs = AsHeapString(host, on_oom);
  bool local = utf8_equals(hslc(hs), "localhost");
  if (!local) {
    // The host's own name resolves to loopback too.
    local = utf8_equals(hslc(hs), "localhost.localdomain") || utf8_equals(hslc(hs), "127.0.0.1") ||
            utf8_equals(hslc(hs), "0.0.0.0") || utf8_equals(hslc(hs), "");
  }
  if (!local) {
    raise_vm_exception(thread, STR("java/net/UnknownHostException"), hslc(hs));
    free_heap_str(hs);
    return value_null();
  }
  free_heap_str(hs);

  obj_header *addr = bjvm_make_inet4(thread, 0x7f000001); // 127.0.0.1
  if (!addr)
    return value_null();
  classdesc *inet = bootstrap_lookup_class(thread, STR("java/net/InetAddress"));
  obj_header *arr = CreateObjectArray1D(thread, inet, 1);
  if (!arr)
    return value_null();
  ReferenceArrayStore(arr, 0, addr);
  return (stack_value){.obj = arr};

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, getHostByAddr, "([B)Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet4AddressImpl, isReachable0, "([BI[BI)Z") {
  return (stack_value){.i = 0};
}

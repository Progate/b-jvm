#include <natives-dsl.h>

// IPv6 doesn't exist in this runtime (see sun/nio/ch/bjvm_net.h). These
// exist so the class links; callers that reach them fail cleanly.

DECLARE_NATIVE("java/net", Inet6AddressImpl, getLocalHostName, "()Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, lookupAllHostAddr,
               "(Ljava/lang/String;I)[Ljava/net/InetAddress;") {
  obj_header *host = args[0].handle->obj;
  heap_string hs = AsHeapString(host, on_oom);
  raise_vm_exception(thread, STR("java/net/UnknownHostException"), hslc(hs));
  free_heap_str(hs);
  return value_null();

on_oom:
  return value_null();
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, getHostByAddr, "([B)Ljava/lang/String;") {
  return (stack_value){.obj = MakeJStringFromCString(thread, "localhost", true)};
}

DECLARE_NATIVE("java/net", Inet6AddressImpl, isReachable0, "([BII[BII)Z") {
  return (stack_value){.i = 0};
}

#include <natives-dsl.h>

// Only IPv4 exists in this runtime; see sun/nio/ch/bjvm_net.h for why.
DECLARE_NATIVE("java/net", InetAddress, init, "()V") { return value_null(); }
DECLARE_NATIVE("java/net", InetAddress, isIPv4Available, "()Z") { return (stack_value){.i = 1}; }
DECLARE_NATIVE("java/net", InetAddress, isIPv6Supported, "()Z") { return (stack_value){.i = 0}; }

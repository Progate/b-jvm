#include <natives-dsl.h>

// sockaddr struct constants (IPv4/IPv6 Linux layout). Only used by code
// that packs/unpacks NativeSocketAddress buffers; our Net natives never
// take that path but the class's <clinit> needs real values.

DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, AFINET, "()I") { return (stack_value){.i = 2}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, AFINET6, "()I") { return (stack_value){.i = 10}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, sizeofSockAddr4, "()I") { return (stack_value){.i = 16}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, sizeofSockAddr6, "()I") { return (stack_value){.i = 28}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, sizeofFamily, "()I") { return (stack_value){.i = 2}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetFamily, "()I") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin4Port, "()I") { return (stack_value){.i = 2}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin4Addr, "()I") { return (stack_value){.i = 4}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin6Port, "()I") { return (stack_value){.i = 2}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin6Addr, "()I") { return (stack_value){.i = 8}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin6ScopeId, "()I") { return (stack_value){.i = 24}; }
DECLARE_NATIVE("sun/nio/ch", NativeSocketAddress, offsetSin6FlowInfo, "()I") { return (stack_value){.i = 4}; }

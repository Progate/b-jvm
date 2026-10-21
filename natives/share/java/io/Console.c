#include <natives-dsl.h>

DECLARE_NATIVE("java/io", Console, istty, "()Z") { return (stack_value){.i = 1}; }

// istty() が常に真を返すので、stdin/stdout/stderr の 3 ビット全部を立てて整合させる。
// JDK 22 以降 Console.<clinit> が読む値で、未実装だと JUnit の ConsoleLauncher のように
// Console を触るだけのクラスが起動時に UnsatisfiedLinkError を吐く
DECLARE_NATIVE("java/io", Console, ttyStatus, "()I") { return (stack_value){.i = 0x7}; }

DECLARE_NATIVE("java/io", Console, encoding, "()Ljava/lang/String;") { return value_null(); }
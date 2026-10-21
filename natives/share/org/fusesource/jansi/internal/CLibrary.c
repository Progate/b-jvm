#include <natives-dsl.h>

// jansi（kotlinc が診断の色付けに使う）の JNI。端末かどうかだけを聞かれるので、端末ではないと
// 答える（色の制御文字を出さない）。無いと kotlinc を起動するたびに
// "Unsatisfied link error isatty" が標準エラーに出る
DECLARE_NATIVE("org/fusesource/jansi/internal", CLibrary, isatty, "(I)I") { return (stack_value){.i = 0}; }

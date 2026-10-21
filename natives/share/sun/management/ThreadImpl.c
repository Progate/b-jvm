#include <natives-dsl.h>
#include <time.h>

// sun.management.ThreadImpl のスレッド CPU 時間。b-jvm のスレッドは 1 つの wasm スレッドの上で
// 順に回るので、スレッドごとの CPU 時間は取れない。単調時計の値を返す（計測の差分を取る
// 使い方なら壁時計の経過として読める）

extern bool thread_cpu_time_enabled;

static s64 monotonic_nanos(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (s64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

DECLARE_NATIVE("sun/management", ThreadImpl, setThreadCpuTimeEnabled0, "(Z)V") {
  thread_cpu_time_enabled = args[0].i != 0;
  return value_null();
}

DECLARE_NATIVE("sun/management", ThreadImpl, getThreadTotalCpuTime0, "(J)J") {
  return (stack_value){.l = monotonic_nanos()};
}

DECLARE_NATIVE("sun/management", ThreadImpl, getThreadUserCpuTime0, "(J)J") {
  return (stack_value){.l = monotonic_nanos()};
}

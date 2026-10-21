#include <natives-dsl.h>
#include <time.h>
#include <unistd.h>

// sun.management.VMManagementImpl（jmm 相当）の最小スタブ。
// 計測系は 0/false、バージョンや稼働時間などだけ実値を返す。
// ManagementFactory.getRuntimeMXBean() の初期化がこれを通る。

DECLARE_NATIVE("sun/management", VMManagementImpl, getVersion0, "()Ljava/lang/String;") {
  return (stack_value){.obj = (void *)MakeJStringFromCString(thread, "23", true)};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, initOptionalSupportFields, "()V") { return value_null(); }
DECLARE_NATIVE("sun/management", VMManagementImpl, isThreadContentionMonitoringEnabled, "()Z") {
  return (stack_value){.i = 0};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, isThreadCpuTimeEnabled, "()Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, isThreadAllocatedMemoryEnabled, "()Z") {
  return (stack_value){.i = 0};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getTotalClassCount, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getUnloadedClassCount, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getVerboseClass, "()Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getVerboseGC, "()Z") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getProcessId, "()I") { return (stack_value){.i = 1}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getVmArguments0, "()[Ljava/lang/String;") {
  return (stack_value){.obj = CreateObjectArray1D(thread, cached_classes(thread->vm)->string, 0)};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getStartupTime, "()J") {
  return (stack_value){.l = (s64)time(nullptr) * 1000};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getUptime0, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getAvailableProcessors, "()I") { return (stack_value){.i = 1}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getTotalCompileTime, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getTotalThreadCount, "()J") {
  return (stack_value){.l = arrlen(thread->vm->active_threads)};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getLiveThreadCount, "()I") {
  return (stack_value){.i = (int)arrlen(thread->vm->active_threads)};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getPeakThreadCount, "()I") {
  return (stack_value){.i = (int)arrlen(thread->vm->active_threads)};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getDaemonThreadCount, "()I") { return (stack_value){.i = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getSafepointCount, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getTotalSafepointTime, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getSafepointSyncTime, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getTotalApplicationNonStoppedTime, "()J") {
  return (stack_value){.l = 0};
}
DECLARE_NATIVE("sun/management", VMManagementImpl, getLoadedClassSize, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getUnloadedClassSize, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getClassLoadingTime, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getMethodDataSize, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getInitializedClassCount, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getClassInitializationTime, "()J") { return (stack_value){.l = 0}; }
DECLARE_NATIVE("sun/management", VMManagementImpl, getClassVerificationTime, "()J") { return (stack_value){.l = 0}; }

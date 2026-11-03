//
//  kmod_info.c
//  ECFanSMC
//  生成 _kmod_info 结构，start/stop 指向 Lilu 插件的 kern_start/kern_stop
//
#include <mach/kmod.h>

// 由 Lilu 的 plugin_start.cpp 提供（ADDPR(kern_start) / ADDPR(kern_stop)）
extern kern_return_t ECFanSMC_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t ECFanSMC_kern_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.hanning.ECFanSMC, "0.2.0", ECFanSMC_kern_start, ECFanSMC_kern_stop);

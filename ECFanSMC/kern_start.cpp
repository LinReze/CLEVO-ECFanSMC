//
//  kern_start.cpp
//  ECFanSMC
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/IOLib.h>

#include "ECFanSMC.hpp"

static const char *bootargOff[] {
	"-ecfanoff"
};

static const char *bootargDebug[] {
	"-ecfandbg"
};

static const char *bootargBeta[] {
	"-ecfanbeta"
};

// 手动创建并启动 IOService 实例。
//
// 为什么必须在这里手动 new：
//   本 kext 定义了 LILU_CUSTOM_IOKIT_INIT，意味着 Lilu 不会替我们
//   生成/实例化 PRODUCT_NAME 这个 IOService。同时本 kext 的
//   IOProviderClass=IOResources 的 personality 在 Lilu 插件加载
//   模式下不会被 IOCatalogue 自动匹配（插件是被 Lilu bootstrap 手动
//   加载的，不走 kextd 的 personality 匹配流程）。
//   结果就是 probe/start 从未被调用、FNum 等 SMC key 从未注册，
//   读出来永远是 0。这里补上"实例化 + start"这一步。
static ECFanSMC *gService = nullptr;

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::MountainLion,
	KernelVersion::Sequoia,
	[]() {
		// 实例化 + probe + start，等价于 IOKit 正常匹配时的流程
		gService = new ECFanSMC;
		if (!gService) {
			SYSLOG("ecfan", "failed to allocate ECFanSMC");
			return;
		}

		if (!gService->init(nullptr)) {
			SYSLOG("ecfan", "init failed");
			gService->release();
			gService = nullptr;
			return;
		}

		SInt32 score = 0;
		// probe 恒返回 this（探测逻辑见 ECFanSMC.cpp），这里只触发一次
		if (gService->probe(nullptr, &score) != gService) {
			SYSLOG("ecfan", "probe rejected");
			gService->release();
			gService = nullptr;
			return;
		}

		if (!gService->start(nullptr)) {
			SYSLOG("ecfan", "start failed");
			gService->release();
			gService = nullptr;
			return;
		}

		// 发布进 IORegistry（等价于 IOKit 匹配后的 registerService），
		// 让服务能被 VirtualSMC 及其他客户端发现。
		gService->registerService();

		SYSLOG("ecfan", "manually started ECFanSMC service");
	}
};

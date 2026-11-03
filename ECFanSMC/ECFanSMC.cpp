//
//  ECFanSMC.cpp
//  ECFanSMC
//
//  蓝天(Clevo)/同方模具笔记本 EC 风扇传感器桥接
//
//  访问路径：EC RAM 是「内存映射」（DSDT OperationRegion RAM, SystemMemory, 0xFE0B0100）。
//    不是 0x62/0x66 端口轮询（那是 ECMD 命令通道的底层握手，不是数据区）。
//    通过内核 ml_io_map() 映射物理内存 0xFE0B0100 直接读写寄存器。
//  寄存器地图（Field 精确追踪，基址 0xFE0B0100）：
//    TMP  0x07  8bit  CPU 温度（℃）
//    DUT1 0xCE  8bit  风扇1 占空比
//    DUT2 0xCF  8bit  风扇2 占空比
//    RPM1 0xD0  16bit 风扇1 转速（大端，RPM = 2156220 / raw）
//    RPM2 0xD2  16bit 风扇2 转速（大端，同上）
//    RPM3 0xE0  16bit 风扇3 转速
//  转速换算：RPM 字段直接读出的就是转速值（16bit 小端），无需系数换算。
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>

#include "ECFanSMC.hpp"
#include "KeyImplementations.hpp"

// ============ EC 内存映射基址与偏移 ============
#define EC_RAM_PHYS     0xFE0B0100ULL  // EC RAM 物理基址（DSDT RAM OperationRegion）
#define EC_RAM_SIZE     0x100          // 256 字节

#define EC_OFF_TMP      0x07
#define EC_OFF_DUT1     0xCE
#define EC_OFF_DUT2     0xCF
#define EC_OFF_RPM1     0xD0
#define EC_OFF_RPM2     0xD2
#define EC_OFF_RPM3     0xE0

static ECFanSMC *gFanSMC = nullptr;

// F0ID 用的风扇描述符（SmcKeyTypeFds 的 {fds 类型，固定 16 字节）
// 布局与 VirtualSMC 官方 SMCDellSensors 完全一致：4 字节头 + 12 字节名字。
static constexpr int32_t DiagFunctionStrLen = 12;
struct FanTypeDesc {
	uint8_t type      {0};    // 0 = FAN_PWM_TACH
	uint8_t zone      {1};
	uint8_t location  {12};
	uint8_t rsvd      {0};
	char    strFunction[DiagFunctionStrLen];
};

OSDefineMetaClassAndStructors(ECFanSMC, IOService)

// ============ EC 内存映射访问（读转速/温度用） ============
// EC RAM 是 SystemMemory 内存映射（DSDT RAM OperationRegion @ 0xFE0B0100），
// 用内核私有 KPI ml_io_map() 把物理内存映射进内核虚拟地址空间直接读写。
// ml_io_map / ml_io_unmap 由 com.apple.kpi.unsupported 提供（内核已导出 _ml_io_map）。
extern "C" {
	vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
	// 注意：内核只导出 _ml_io_map，没有 _ml_io_unmap。
	// 黑苹果 kext 开机加载后从不卸载，EC 映射永久有效，无需 unmap。
}

// ============ EC 端口 I/O（调速命令用） ============
// 蓝天 EC 调速命令 0x99 走 ACPI 标准 EC 端口协议 0x62/0x66，不是内存映射。
// macOS 内核不导出 x86 的 inb/outb（ml_io_read8/ml_io_write8 的第一参数是
// 「映射后的虚拟地址」而非端口号，用它们访问端口会解引用无效地址导致内核 panic）。
// 正确做法：直接用 x86 IN/OUT 内联汇编访问 IO 端口（黑苹果 kext 访问 EC 的标准方式）。
static inline uint8_t ecInb(uint16_t port) {
	uint8_t v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "dN"(port));
	return v;
}
static inline void ecOutb(uint16_t port, uint8_t value) {
	__asm__ volatile ("outb %0, %1" : : "a"(value), "dN"(port));
}

#define EC_SC_PORT   0x66   // 状态/命令端口
#define EC_DATA_PORT 0x62   // 数据端口
#define EC_IBF       1      // 输入缓冲满标志位（写前须为 0）
#define EC_OBF       0      // 输出缓冲满标志位（读前须为 1）

// 等待 EC 状态位翻转（fanctrl.c ec_io_wait 的 macOS 版）
// 对齐官方 fanctrl.c：上限 100 次（不是 1000），每次 IODelay(1000)=1ms。
// 最长忙等 ~100ms/次，3 步 ~300ms，远低于内核 watchdog 阈值，EC 不响应也能安全返回。
static inline bool ecWait(uint16_t port, uint32_t flag, bool value) {
	for (int i = 0; i < 100; i++) {
		uint8_t data = ecInb(port);
		if (((data >> flag) & 1) == (value ? 1 : 0))
			return true;
		IODelay(1000);   // 1ms
	}
	return false;
}

// 写 EC 命令通道三段式：outb(cmd,0x66) → outb(port,0x62) → outb(value,0x62)
static inline bool ecIoDo(uint8_t cmd, uint8_t port, uint8_t value) {
	if (!ecWait(EC_SC_PORT, EC_IBF, false)) return false;
	ecOutb(EC_SC_PORT, cmd);

	if (!ecWait(EC_SC_PORT, EC_IBF, false)) return false;
	ecOutb(EC_DATA_PORT, port);

	if (!ecWait(EC_SC_PORT, EC_IBF, false)) return false;
	ecOutb(EC_DATA_PORT, value);

	return ecWait(EC_SC_PORT, EC_IBF, false);
}

// 映射 EC RAM 到 ecBase
bool ECFanSMC::mapEc() {
	if (ecBase)
		return true;

	vm_offset_t v = ml_io_map(static_cast<vm_offset_t>(EC_RAM_PHYS), EC_RAM_SIZE);
	if (!v) {
		SYSLOG("ecfan", "ml_io_map(0xFE0B0100) failed");
		return false;
	}

	ecBase = reinterpret_cast<volatile uint8_t *>(v);
	SYSLOG("ecfan", "EC RAM mapped 0xFE0B0100 -> %p", ecBase);
	return true;
}

void ECFanSMC::unmapEc() {
	// 内核无 ml_io_unmap，映射永久有效；这里只清指针，不真正解除映射。
	ecBase = nullptr;
}

// 读一个 EC RAM 字节（内存映射直接读）
static inline uint8_t ecReadByte(volatile uint8_t *base, uint8_t off) {
	return base[off];
}

// 写一个 EC RAM 字节（内存映射直接写）
static inline void ecWriteByte(volatile uint8_t *base, uint8_t off, uint8_t val) {
	base[off] = val;
}

// 探测 EC：映射 + 温度寄存器读数落在合理区间即认为通
bool ECFanSMC::probeEcm() {
	if (ecProbed)
		return ecReady;
	ecProbed = true;

	if (!mapEc()) {
		ecReady = false;
		return false;
	}

	uint8_t tmp = ecReadByte(ecBase, EC_OFF_TMP);

	// 0 或 0xFF 通常说明地址错/未初始化
	if (tmp == 0x00 || tmp == 0xFF) {
		SYSLOG("ecfan", "EC TMP=0x%02X looks invalid (offset 0x07)", tmp);
		ecReady = false;
		return false;
	}

	SYSLOG("ecfan", "EC online, TMP=%u C", tmp);
	ecReady = true;
	return true;
}

// ============ 风扇协议 ============
uint16_t ECFanSMC::readFanRaw(size_t index) {
	uint8_t off;
	switch (index) {
		case 0: off = EC_OFF_RPM1; break;
		case 1: off = EC_OFF_RPM2; break;
		case 2: off = EC_OFF_RPM3; break;
		default: return 0;
	}

	// 16bit 大端：高字节在前（fanctrl.c / ec-trace.txt 权威实现）
	//   raw = (read_ec(off) << 8) | read_ec(off+1)
	uint16_t raw = static_cast<uint16_t>(ecReadByte(ecBase, off)) << 8
	             | static_cast<uint16_t>(ecReadByte(ecBase, off + 1));

	// 转速换算公式（蓝天 EC）：RPM = 2156220 / raw
	if (raw == 0)
		return 0;
	return static_cast<uint16_t>(2156220U / raw);
}

bool ECFanSMC::forceFanDuty(size_t index, uint8_t duty) {
	// 调速走 EC 命令 0x99（clevo-indicator.c ec_write_fan_duty 权威实现）：
	//   outb(0x99, 0x66)  命令码
	//   outb(index, 0x62) index: 1=风扇1, 2=风扇2（注意是 1-based，不是 0-based）
	//   outb(duty, 0x62)  duty: 0~255（0=停, 255=全速）
	uint8_t fanIdx = static_cast<uint8_t>(index + 1);
	bool ok = ecIoDo(0x99, fanIdx, duty);

	SYSLOG("ecfan", "fan%u duty cmd 0x99 idx=%u duty=0x%02X -> %s",
		static_cast<uint32_t>(index + 1), fanIdx, duty, ok ? "ok" : "FAIL");
	return ok;
}

bool ECFanSMC::releaseFanDuty(size_t index) {
	// 恢复自动：outb(0x99,0x66) → outb(0xFF,0x62) → outb(index,0x62)
	// （《EC调速协议》release_fan_duty：0xFF=恢复自动）
	uint8_t fanIdx = static_cast<uint8_t>(index + 1);
	bool ok = ecIoDo(0x99, 0xFF, fanIdx);

	SYSLOG("ecfan", "fan%u release auto (0x99,0xFF,%u) -> %s",
		static_cast<uint32_t>(index + 1), fanIdx, ok ? "ok" : "FAIL");
	return ok;
}

// ============ 生命周期 ============
bool ECFanSMC::init(OSDictionary *properties) {
	if (!IOService::init(properties))
		return false;

	// 不做任何硬件访问：init 阶段失败会让整个 personality 失配，
	// 这是之前"注入成功但系统里什么都没有"的主因。硬件探测推迟到第一次读。
	SYSLOG("ecfan", "init ok, EC access is lazy (mmio 0xFE0B0100)");
	return true;
}

IOService *ECFanSMC::probe(IOService *provider, SInt32 *score) {
	// probe 只做探测判断，不注册 key（key 注册统一放到 start）。
	// 手动实例化时 provider 可能为 nullptr，不能依赖 IOService::probe 的父类实现。
	if (provider && !IOService::probe(provider, score)) {
		SYSLOG("ecfan", "failed to probe parent");
		return nullptr;
	}
	return this;
}

// 防重复注册：手动 new + start()（kern_start.cpp）会先跑一次，
// 随后 registerService() 又触发 IOKit 正常匹配、再次 probe/start，
// 导致 addKey 执行两遍、vsmcPlugin.data 出现重复 key，
// 进而使 getByName 二分查找/写权限判断错乱（写 F0Tg/F0Md 返回 kIOReturnNotWritable）。
static bool gStarted = false;

bool ECFanSMC::start(IOService *provider) {
	if (provider && !IOService::start(provider)) {
		SYSLOG("ecfan", "failed to start parent");
		return false;
	}

	// 防重入：只允许注册一次 key
	if (gStarted) {
		SYSLOG("ecfan", "start() re-entered, skipping key registration");
		return true;
	}
	gStarted = true;

	setProperty("VersionInfo", kextVersion);

	// Key 实现类通过单例取实例，必须在这里赋值（init 阶段不做硬件访问）
	gFanSMC = this;

	// 注册 SMC key（对齐官方 SMCProcessor：key 注册在 start 里做）
	VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data,
		VirtualSMCAPI::valueWithUint8(2, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

	for (size_t i = 0; i < 2; i++) {
		// F0ID/F1ID：风扇描述符（名字/位置）。部分第三方软件靠它枚举可调速风扇，
		// 缺了这个 key 就只有「风扇存在」而没有「可控制的风扇」，UI 会直接置灰。
		FanTypeDesc desc {};
		desc.type = 0;                                   // FAN_PWM_TACH
		desc.zone = 1;
		desc.location = (i == 0) ? 12 : 14;              // LEFT_MID_REAR / RIGHT_MID_REAR
		strncpy(desc.strFunction, (i == 0) ? "CPU Fan" : "GPU Fan", DiagFunctionStrLen);
		VirtualSMCAPI::addKey(KeyF0ID(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA *>(&desc), sizeof(desc),
				SmcKeyTypeFds, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

		// F0Ac/F1Ac：实际转速，只读
		VirtualSMCAPI::addKey(KeyF0Ac(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new F0Ac(i), SMC_KEY_ATTRIBUTE_READ));
		// F0Mn/F1Mn：最小转速，可读写（工具会先写区间）
		VirtualSMCAPI::addKey(KeyF0Mn(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new MinKey(i),
				static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));
		// F0Mx/F1Mx：最大转速，可读写
		VirtualSMCAPI::addKey(KeyF0Mx(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new MaxKey(i),
				static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));
		// F0Md/F1Md：手动/自动模式，可读写
		VirtualSMCAPI::addKey(KeyF0Md(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithUint8(0, new FanModeKey(i),
				static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));
		// F0Tg/F1Tg：目标转速，可读写（写路径：转占空比写 EC DUT）
		VirtualSMCAPI::addKey(KeyF0Tg(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new F0Tg(i),
				static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));
	}

	// "FS! " 全局强制位掩码：smcFanControl / TG Pro 等软件的总开关，
	// 没有这个 key 它们会在第一步就判定「不支持风扇控制」。
	VirtualSMCAPI::addKey(KeyFS__, vsmcPlugin.data,
		VirtualSMCAPI::valueWithUint16(0, new ForceKey(),
			static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));

	// EC00..EC07：EC RAM 全表只读镜像（8 × 32 字节），用于定位 GPU 温度寄存器
	uint8_t zeros[32] = {0};
	for (size_t i = 0; i < 8; i++) {
		VirtualSMCAPI::addKey(KeyECDump(i), vsmcPlugin.data,
			VirtualSMCAPI::valueWithData(zeros, 32, SmcKeyTypeHex, new ECDumpKey(i),
				SMC_KEY_ATTRIBUTE_READ));
	}

	VirtualSMCAPI::addKey(KeyTC0D, vsmcPlugin.data,
		VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TC0D(), SMC_KEY_ATTRIBUTE_READ));
	VirtualSMCAPI::addKey(KeyTC0P, vsmcPlugin.data,
		VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TC0P(), SMC_KEY_ATTRIBUTE_READ));

	// 关键：VirtualSMC 的 loadPlugin 会对 plugin->data 做二分查找（getByName），
	// 所以 addKey 后必须显式 qsort 排序（官方 SMCProcessor::setupKeys 末尾就是这么做的，
	// 否则未排序会导致部分 key 读不到 —— 之前 F1Tg/F0Md 读不到就是这原因）。
	// qsort 由 Lilu 导出（kern_util.hpp 里 EXPORT void qsort(...)），不是内核的 _kx_qsort。
	qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(),
		sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);

	// 提前探一次，日志里能直接看到 EC 通不通
	probeEcm();

	vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
	if (!vsmcNotifier) {
		SYSLOG("ecfan", "failed to register vsmc handler");
		return false;
	}

	// 可选：-ecfc=<0..100> 强制占空比，用于验证写路径；-ecfcfan=<1..3> 选风扇，默认 1
	int percent = -1;
	if (PE_parse_boot_argn("ecfc", &percent, sizeof(percent)) && percent >= 0) {
		if (percent < 30) percent = 30;   // 下限保护，避免风扇停转
		if (percent > 100) percent = 100;

		int fanIdx = 0;
		if (PE_parse_boot_argn("ecfcfan", &fanIdx, sizeof(fanIdx))) {
			fanIdx = fanIdx - 1;
			if (fanIdx < 0) fanIdx = 0;
			if (fanIdx > 2) fanIdx = 2;
		}

		uint8_t duty = static_cast<uint8_t>((percent * 255) / 100);
		if (forceFanDuty(static_cast<size_t>(fanIdx), duty))
			fanForced = true;
		else
			SYSLOG("ecfan", "force fan duty failed");
	}

	SYSLOG("ecfan", "started, FNum=2, ec=%d, forced=%d", ecReady ? 1 : 0, fanForced ? 1 : 0);
	return true;
}

void ECFanSMC::stop(IOService *provider) {
	if (fanForced) {
		for (size_t i = 0; i < 2; i++)
			releaseFanDuty(i);
		fanForced = false;
	}

	if (vsmcNotifier) {
		vsmcNotifier->remove();
		vsmcNotifier = nullptr;
	}

	unmapEc();
	gFanSMC = nullptr;
	IOService::stop(provider);
}

// ============ vsmc 通知 ============
bool ECFanSMC::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
	if (sensors && vsmc) {
		auto &plugin = static_cast<ECFanSMC *>(sensors)->vsmcPlugin;
		auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
		if (ret == kIOReturnSuccess) {
			DBGLOG("ecfan", "submitted plugin to vsmc");
			return true;
		} else if (ret != kIOReturnUnsupported) {
			SYSLOG("ecfan", "plugin submission failure %X", ret);
		} else {
			DBGLOG("ecfan", "plugin submission to non-vsmc");
		}
	} else {
		SYSLOG("ecfan", "null vsmc notification");
	}
	return false;
}

// ============ 对外读数 ============
uint16_t ECFanSMC::readFanRpm(size_t index) {
	if (!ecReady && !probeEcm())
		return 0;

	// RPM 字段直接就是转速值（16bit 小端），无需系数换算
	return readFanRaw(index);
}

uint8_t ECFanSMC::readTemp() {
	if (!ecReady && !probeEcm())
		return 0;

	return ecReadByte(ecBase, EC_OFF_TMP);
}

// 按目标转速写占空比（路径 1：直接写 DUT 占空比寄存器）
// 换算：duty = clamp(rpm / F0Mx, 30..100)。
void ECFanSMC::setFanRpm(size_t index, uint16_t rpm) {
	if (index >= 2)
		return;

	if (!ecReady && !probeEcm())
		return;

	// rpm 为 0 表示交还自动
	if (rpm == 0) {
		releaseFanDuty(index);
		targetRpm[index] = 0;
		manualMode[index] = 0;
		fansForced &= static_cast<uint16_t>(~(1u << index));
		return;
	}

	// 先缓存写入值：F0Tg 回读必须原样等于写入值，否则做回读校验的软件会判定失败。
	targetRpm[index] = rpm;

	uint16_t hi = maxRpm[index];
	if (hi == 0)
		hi = 6500;

	// 占空比 = rpm / F0Mx，下限 0%（允许停转），上限 100%（全速）。
	// 之前 30% 的下限保护导致「拉不到最低」——风扇在低档被钳在 30% 占空比。
	uint32_t pct = (static_cast<uint32_t>(rpm) * 100) / hi;
	if (pct > 100) pct = 100;

	uint8_t duty = static_cast<uint8_t>((pct * 255) / 100);
	if (forceFanDuty(index, duty)) {
		manualMode[index] = 1;
		fansForced |= static_cast<uint16_t>(1u << index);
	}
}

void ECFanSMC::setManualMode(size_t index, uint8_t manual) {
	if (index >= 2)
		return;

	if (!ecReady && !probeEcm())
		return;

	manualMode[index] = manual ? 1 : 0;
	if (!manual) {
		// 切回自动：交还 EC 固件控制
		releaseFanDuty(index);
		targetRpm[index] = 0;
		fansForced &= static_cast<uint16_t>(~(1u << index));
	} else {
		fansForced |= static_cast<uint16_t>(1u << index);
	}
}

uint8_t ECFanSMC::readManualMode(size_t index) {
	if (index >= 2)
		return 0;
	return manualMode[index];
}

// ---- F0Tg 目标转速缓存 ----
uint16_t ECFanSMC::readTargetRpm(size_t index) {
	if (index >= 2)
		return 0;
	return targetRpm[index];
}

void ECFanSMC::setTargetRpm(size_t index, uint16_t rpm) {
	if (index >= 2)
		return;
	targetRpm[index] = rpm;
}

// ---- F0Mn/F0Mx 调速区间 ----
uint16_t ECFanSMC::readMinRpm(size_t index) {
	if (index >= 2)
		return 1000;
	return minRpm[index];
}

uint16_t ECFanSMC::readMaxRpm(size_t index) {
	if (index >= 2)
		return 6500;
	return maxRpm[index];
}

void ECFanSMC::setMinRpm(size_t index, uint16_t rpm) {
	if (index >= 2)
		return;
	minRpm[index] = rpm;
}

void ECFanSMC::setMaxRpm(size_t index, uint16_t rpm) {
	if (index >= 2)
		return;
	maxRpm[index] = rpm;
}

// ---- "FS! " 强制位掩码 ----
uint16_t ECFanSMC::readForceMask() {
	return fansForced;
}

void ECFanSMC::setForceMask(uint16_t mask) {
	for (size_t i = 0; i < 2; i++) {
		if (mask & (1u << i)) {
			// 置位：进入手动，速度由随后的 F0Tg 写入决定（此处不下发硬件）
			fansForced |= static_cast<uint16_t>(1u << i);
			manualMode[i] = 1;
			SYSLOG("ecfan", "FS! force fan%u on", static_cast<uint32_t>(i + 1));
		} else {
			// 清位：交还 EC 自动
			SYSLOG("ecfan", "FS! release fan%u", static_cast<uint32_t>(i + 1));
			setManualMode(i, 0);
		}
	}
}

ECFanSMC *ecfanGetInstance() { return gFanSMC; }

// ============ EC RAM 诊断直读 ============
void ECFanSMC::readEcBlock(size_t offset, uint8_t *out, size_t len) {
	if (!out || len == 0)
		return;

	if (!ecReady && !probeEcm()) {
		memset(out, 0, len);
		return;
	}

	for (size_t i = 0; i < len; i++) {
		size_t off = offset + i;
		out[i] = off < EC_RAM_SIZE ? ecBase[off] : 0;
	}
}

//
//  ECFanSMC.hpp
//  ECFanSMC
//
//  蓝天(Clevo)/同方模具笔记本 EC 风扇传感器桥接
//  通过 ACPI 标准 EC 端口 0x66/0x62 读写 EC RAM，不走物理内存映射。
//
//  Copyright © 2026. All rights reserved.
//

#ifndef ECFanSMC_hpp
#define ECFanSMC_hpp

#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <Headers/kern_util.hpp>
#include <VirtualSMCSDK/kern_vsmcapi.hpp>

class ECFanSMC : public IOService {
	OSDeclareDefaultStructors(ECFanSMC)

	/** Key name index mapping ("0123456789ABCDEF") */
	static constexpr size_t MaxIndexCount = sizeof("0123456789ABCDEF") - 1;
	static constexpr const char *KeyIndexes = "0123456789ABCDEF";

	/** Key name declarations */
	static constexpr SMC_KEY KeyFNum = SMC_MAKE_IDENTIFIER('F','N','u','m');
	static constexpr SMC_KEY KeyF0Ac(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'A','c'); }
	static constexpr SMC_KEY KeyF0Tg(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'T','g'); }
	static constexpr SMC_KEY KeyF0Md(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'M','d'); }
	static constexpr SMC_KEY KeyF0Mn(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'M','n'); }
	static constexpr SMC_KEY KeyF0Mx(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'M','x'); }
	/** 风扇描述符（{fds 16 字节），部分第三方软件靠它才能列出手动调速风扇 */
	static constexpr SMC_KEY KeyF0ID(size_t i) { return SMC_MAKE_IDENTIFIER('F',KeyIndexes[i],'I','D'); }
	/** 全局强制位掩码（bit i = 风扇 i 被强制手动），smcFanControl/TG Pro 类软件的总开关 */
	static constexpr SMC_KEY KeyFS__ = SMC_MAKE_IDENTIFIER('F','S','!',' ');
	/**
	 * EC RAM 诊断直读：EC00..EC07 共 8 个 32 字节块，覆盖 0x00..0xFF。
	 * 用于在没有 GPU 温度传感器的前提下扫描 EC 全表、定位第二个温度寄存器。
	 * 只读、无副作用；定位完成后可保留（体积可忽略）。
	 */
	static constexpr SMC_KEY KeyECDump(size_t i) { return SMC_MAKE_IDENTIFIER('E','C','0', char('0' + i)); }
	static constexpr SMC_KEY KeyTC0D = SMC_MAKE_IDENTIFIER('T','C','0','D');
	static constexpr SMC_KEY KeyTC0P = SMC_MAKE_IDENTIFIER('T','C','0','P');

	/** VirtualSMC service registration notifier */
	IONotifier *vsmcNotifier {nullptr};

	/** Registered plugin instance */
	VirtualSMCAPI::Plugin vsmcPlugin {
		xStringify(PRODUCT_NAME),
		parseModuleVersion(xStringify(MODULE_VERSION)),
		VirtualSMCAPI::Version,
	};

	/** EC 是否可用（首次访问时探测） */
	bool ecReady {false};
	bool ecProbed {false};

	/** 是否已接管风扇（-ecfc= 触发），stop 时需交还 */
	bool fanForced {false};

	/** ---- EC 底层（内存映射 0xFE0B0100）---- */
	/** EC RAM 虚拟地址（ml_io_map 后），nullptr 表示未映射 */
	volatile uint8_t *ecBase {nullptr};
	/** 映射 EC RAM，失败返回 false */
	bool  mapEc();
	void  unmapEc();
	/** 首次使用时探测 EC 是否响应 */
	bool  probeEcm();

	/** ---- 风扇协议 ---- */
	uint16_t readFanRaw(size_t index);
	bool  forceFanDuty(size_t index, uint8_t duty);
	bool  releaseFanDuty(size_t index);

	/** 手动模式标志（0=自动，1=手动），供 F0Md 键读写 */
	uint8_t manualMode[size_t(2)] {0, 0};

	/**
	 * 目标转速缓存（F0Tg 的回读值）。
	 * 关键：Apple SMC 的 F0Tg 是「目标寄存器」，写完回读必须等于写入值。
	 * 之前直接回读实际转速，凡是「写后回读校验」的软件（TG Pro / Stats / smcFanControl）
	 * 都会认为写入失败并立刻回滚到自动，表现就是「怎么调都没反应」。
	 * 0 表示未手动（此时回读实际转速）。
	 */
	uint16_t targetRpm[size_t(2)] {0, 0};

	/** 调速区间缓存（F0Mn/F0Mx 可写：不少软件先写区间再写目标转速） */
	uint16_t minRpm[size_t(2)] {1000, 1000};
	uint16_t maxRpm[size_t(2)] {6500, 6500};

	/** FS! 强制位掩码（bit i 对应风扇 i） */
	uint16_t fansForced {0};

public:
	bool init(OSDictionary *properties) override;
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

	static bool vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier);

	// 供 Key 类读取的接口
	uint16_t readFanRpm(size_t index);
	uint8_t  readTemp();
	uint8_t  readManualMode(size_t index);
	// 供 Key 类写入的接口：按目标转速写占空比；切手动/自动模式
	void     setFanRpm(size_t index, uint16_t rpm);
	void     setManualMode(size_t index, uint8_t manual);

	// 目标转速 / 区间 / 强制位掩码（供 F0Tg、F0Mn、F0Mx、FS! 键实现）
	uint16_t readTargetRpm(size_t index);
	void     setTargetRpm(size_t index, uint16_t rpm);
	uint16_t readMinRpm(size_t index);
	uint16_t readMaxRpm(size_t index);
	void     setMinRpm(size_t index, uint16_t rpm);
	void     setMaxRpm(size_t index, uint16_t rpm);
	uint16_t readForceMask();
	void     setForceMask(uint16_t mask);

	/** 读一段 EC RAM（供 EC00..EC07 诊断键使用） */
	void     readEcBlock(size_t offset, uint8_t *out, size_t len);
};

#endif /* ECFanSMC_hpp */

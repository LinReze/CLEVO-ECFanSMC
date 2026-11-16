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

	/**
	 * ---- RAPL 功耗墙（真正决定发热的源头，不是风扇）----
	 * 走 MSR：0x606 = PKG_POWER_SKU_UNIT（bits[3:0] 为 2 的幂次），
	 *         0x610 = PKG_POWER_LIMIT（PL1 @14:0，PL2 @46:32，各 15 bit 同一单位）。
	 * RP01/RP02 只读，返回换算后的整数瓦；RW01/RW02 可写，内部钳制 5..150W。
	 * 与 DSDT 的 SPL1() 写 CPWR MMIO 是同一个硬件寄存器的两种访问方式，
	 * 用 MSR 更稳（DSDT 那条依赖 GUAM/PEPD 触发，且要自己算 MMIO 基址）。
	 */
	static constexpr SMC_KEY KeyRP01 = SMC_MAKE_IDENTIFIER('R','P','0','1');
	static constexpr SMC_KEY KeyRP02 = SMC_MAKE_IDENTIFIER('R','P','0','2');
	static constexpr SMC_KEY KeyRW01 = SMC_MAKE_IDENTIFIER('R','W','0','1');
	static constexpr SMC_KEY KeyRW02 = SMC_MAKE_IDENTIFIER('R','W','0','2');
	/** RAPL power unit（诊断用：确认单位换算是否正确） */
	static constexpr SMC_KEY KeyRPUN = SMC_MAKE_IDENTIFIER('R','P','U','N');

	/**
	 * ---- EC RAM 任意字节读写（通用底座）----
	 * ECWA = 地址（ui8，可读写），ECWD = 该地址上的字节（ui8，可读写）。
	 * 用法：先写 ECWA 选偏移，再读写 ECWD。
	 *
	 * 用途：
	 *   1. 电池「在位」位（0x10 bit2）被 EC 卡成 0 时手动补回，恢复系统电池显示；
	 *   2. 后续定位键盘背光档位、电池充电上限阈值等寄存器。
	 * 只碰一个字节，不会像整块 dump 那样误伤相邻字段。
	 */
	static constexpr SMC_KEY KeyECWA = SMC_MAKE_IDENTIFIER('E','C','W','A');
	static constexpr SMC_KEY KeyECWD = SMC_MAKE_IDENTIFIER('E','C','W','D');

	/**
	 * ---- 第二块 EC 内存映射 0xFE0B0300（DSDT: OperationRegion RAM3）----
	 * 这块才是键盘背光/灯效所在（0xFE0B0100 那块只有风扇与电池）：
	 *   +0x80..0x82  KLCR/KLCG/KLCB  左区 RGB
	 *   +0x83..0x85  KMCR/KMCG/KMCB  中区 RGB
	 *   +0x86..0x88  KRCR/KRCG/KRCB  右区 RGB
	 *   +0x89        KBBH            背光亮度
	 *   +0x8A        FANQ
	 *   +0x8B        KBTP            键盘类型
	 *   +0x8C..      F1T1..F3R3      风扇曲线（4 温度点 / 4 占空比 / 3 转速）
	 * EC2A = 地址、EC2D = 该地址字节，用法与 ECWA/ECWD 相同但指向 RAM3。
	 */
	static constexpr SMC_KEY KeyEC2A = SMC_MAKE_IDENTIFIER('E','C','2','A');
	static constexpr SMC_KEY KeyEC2D = SMC_MAKE_IDENTIFIER('E','C','2','D');
	/** 键盘背光亮度（KBBH，0 = 关） */
	static constexpr SMC_KEY KeyKBBH = SMC_MAKE_IDENTIFIER('K','B','B','H');

	// EC 端口协议（0x62/0x66）原语：ECPA=偏移，ECPD=数据（写=端口写，读=端口读）
	static constexpr SMC_KEY KeyECPA = SMC_MAKE_IDENTIFIER('E','C','P','A');
	static constexpr SMC_KEY KeyECPD = SMC_MAKE_IDENTIFIER('E','C','P','D');
	/** RAM3 全表只读镜像（EC10..EC17，8 × 32 字节） */
	static constexpr SMC_KEY KeyECDump2(size_t i) { return SMC_MAKE_IDENTIFIER('E','C','1', char('0' + i)); }

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
	/** 第二块 EC RAM（0xFE0B0300，键盘背光/灯效）虚拟地址 */
	volatile uint8_t *ecBase2 {nullptr};
	/** 映射 EC RAM，失败返回 false */
	bool  mapEc();
	/** 映射第二块 EC RAM（0xFE0B0300） */
	bool  mapEc2();
	void  unmapEc();
	/** 首次使用时探测 EC 是否响应 */
	bool  probeEcm();
	/**
	 * 电池「在位」位修复。
	 * EC[0x10] bit2 是 ACPI 层 BAT0._STA 唯一依据；它被卡成 0 时
	 * macOS 直接判定 BatteryInstalled=No（菜单栏电池消失），
	 * 而同一块内存里的容量/电压/电流其实都是活的。
	 * 这里只在「位为 0 且容量数据有效」时才补，避免真的没电池时乱写。
	 */
	void  restoreBatteryPresence();

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

	/** 单字节读 / 写 EC RAM（供 ECWA/ECWD 键使用） */
	uint8_t  readEcByte(uint8_t offset);
	bool     writeEcByte(uint8_t offset, uint8_t value);

	/** 第二块 EC RAM（0xFE0B0300）的单字节读写与整块读取 */
	uint8_t  readEc2Byte(uint8_t offset);
	bool     writeEc2Byte(uint8_t offset, uint8_t value);
	void     readEc2Block(size_t offset, uint8_t *out, size_t len);

	/** 键盘背光亮度（KBBH @ RAM3 0x89），0 到 0xFF */
	uint8_t  readKeyboardBacklight();
	bool     writeKeyboardBacklight(uint8_t value);

	/**
	 * EC 端口协议（ACPI EmbeddedControl，0x62/0x66）单字节读写。
	 * 内存窗口（0xFE0B0100）只适合数据交换：EC 固件只监视**端口**上的
	 * FCMD(0xF8) 写入事件来执行命令，经窗口写 FCMD 不会触发执行。
	 * 供 ECPA/ECPD 键与 ECMD 命令通道使用。
	 */
	uint8_t  readEcPortByte(uint8_t offset);
	bool     writeEcPortByte(uint8_t offset, uint8_t value);

	// ---- RAPL 功耗墙（供 RP01/RP02/RW01/RW02/RPUN 键实现）----
	uint32_t readRaplUnit();
	uint16_t readPl1Watts();
	uint16_t readPl2Watts();
	bool     writePl1Watts(uint16_t watts);
	bool     writePl2Watts(uint16_t watts);
};

#endif /* ECFanSMC_hpp */

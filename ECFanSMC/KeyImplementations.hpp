//
//  KeyImplementations.hpp
//  ECFanSMC
//

#ifndef KeyImplementations_hpp
#define KeyImplementations_hpp

#include <libkern/libkern.h>
#include <VirtualSMCSDK/kern_vsmcapi.hpp>

class ECFanKey : public VirtualSMCValue { };

class FanIndexKey : public VirtualSMCValue {
protected:
	size_t index;
public:
	FanIndexKey(size_t index) : index(index) {}
};

class F0Ac : public FanIndexKey { using FanIndexKey::FanIndexKey; protected: SMC_RESULT readAccess() override; };
class F0Tg : public FanIndexKey { using FanIndexKey::FanIndexKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };

// 最小/最大转速键（F0Mn/F0Mx）：可读写，供工具划定调速区间
// 不少软件会先写区间再写目标转速，只读会返回 NotWritable 让整个调速流程中止
class MinKey : public FanIndexKey { using FanIndexKey::FanIndexKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };
class MaxKey : public FanIndexKey { using FanIndexKey::FanIndexKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };

// 手动模式标志键（F0Md/F1Md）：工具写 1=手动 0=自动
class FanModeKey : public FanIndexKey { using FanIndexKey::FanIndexKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };

// 全局强制位掩码（"FS! "）：bit i=1 表示风扇 i 被强制到手动转速
class ForceKey : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };

// EC RAM 诊断块（EC00..EC07，每块 32 字节）
class ECDumpKey : public FanIndexKey {
	using FanIndexKey::FanIndexKey;
protected:
	SMC_RESULT readAccess() override;
};

class TC0D : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; };
class TC0P : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; };

// ---- RAPL 功耗墙 ----
// RP01/RP02：只读，当前 PL1/PL2 功耗墙（ui16，整数瓦）
class Pl1Key : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; };
class Pl2Key : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; };

// RW01/RW02：可写，把 PL1/PL2 设成指定瓦数（内部钳制 5..150W，且保留 enable/时间窗位）
class Pl1WriteKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};
class Pl2WriteKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};

// ECPA / ECPD：EC 端口协议（0x62/0x66）任意字节读写。
// 与 ECWA/ECWD（内存窗口）的区别：端口写 FCMD 会真正触发 EC 命令执行。
class EcPortAddrKey : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };
class EcPortDataKey : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; SMC_RESULT update(const SMC_DATA *src) override; };

// RPUN：RAPL power unit（诊断用，验证换算是否正确）
class RaplUnitKey : public ECFanKey { using ECFanKey::ECFanKey; protected: SMC_RESULT readAccess() override; };

// ---- EC RAM 任意字节读写 ----
// ECWA：当前选中的 EC 偏移；ECWD：该偏移上的字节。两者都可读写。
// 偏移存在文件作用域静态变量里，让两个 key 共享同一个「指针」。
class EcAddrKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};
class EcDataKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};

// ---- 第二块 EC RAM（0xFE0B0300，键盘背光/灯效）----
class Ec2AddrKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};
class Ec2DataKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};

// 键盘背光亮度（KBBH @ RAM3 0x89）
class KeyboardBacklightKey : public ECFanKey {
	using ECFanKey::ECFanKey;
protected:
	SMC_RESULT readAccess() override;
	SMC_RESULT update(const SMC_DATA *src) override;
};

// RAM3 全表镜像（EC10..EC17）
class ECDump2Key : public FanIndexKey {
	using FanIndexKey::FanIndexKey;
protected:
	SMC_RESULT readAccess() override;
};

#endif /* KeyImplementations_hpp */

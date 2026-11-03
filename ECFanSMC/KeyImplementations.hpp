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

#endif /* KeyImplementations_hpp */

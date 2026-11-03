//
//  KeyImplementations.cpp
//  ECFanSMC
//

#include "KeyImplementations.hpp"
#include "ECFanSMC.hpp"

ECFanSMC *ecfanGetInstance();

// 风扇转速（只读）—— encodeIntFp：fpe2 定点，RPM 直接填
SMC_RESULT F0Ac::readAccess() {
	auto inst = ecfanGetInstance();
	uint16_t rpm = inst ? inst->readFanRpm(index) : 0;
	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, rpm);
	return SmcSuccess;
}

SMC_RESULT F0Tg::readAccess() {
	auto inst = ecfanGetInstance();
	if (!inst) {
		*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, 0);
		return SmcSuccess;
	}

	// Apple SMC 的 F0Tg 是「目标转速寄存器」，写完回读必须原样返回写入值。
	// 只有从未手动设置过（target==0）时才回读实际转速，避免工具显示 0 RPM。
	uint16_t rpm = inst->readTargetRpm(index);
	if (rpm == 0)
		rpm = inst->readFanRpm(index);

	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, rpm);
	return SmcSuccess;
}

// 写目标转速：工具设转速时，VirtualSMC 先调 writeAccess()（权限检查），
// 再调 update(src) 传入新数据。真正写 EC 的动作必须在 update 里做。
SMC_RESULT F0Tg::update(const SMC_DATA *src) {
	VirtualSMCValue::update(src);

	uint16_t rpm = VirtualSMCAPI::decodeIntFp(SmcKeyTypeFpe2, *reinterpret_cast<const uint16_t *>(src));

	auto inst = ecfanGetInstance();
	if (inst)
		inst->setFanRpm(index, rpm);

	return SmcSuccess;
}

// 最小/最大转速（可写，写入只影响缓存不下发硬件）
SMC_RESULT MinKey::readAccess() {
	auto inst = ecfanGetInstance();
	uint16_t rpm = inst ? inst->readMinRpm(index) : 1000;
	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, rpm);
	return SmcSuccess;
}

SMC_RESULT MinKey::update(const SMC_DATA *src) {
	VirtualSMCValue::update(src);

	uint16_t rpm = VirtualSMCAPI::decodeIntFp(SmcKeyTypeFpe2, *reinterpret_cast<const uint16_t *>(src));
	auto inst = ecfanGetInstance();
	if (inst)
		inst->setMinRpm(index, rpm);

	return SmcSuccess;
}

SMC_RESULT MaxKey::readAccess() {
	// 蓝天模具风扇常见最大转速约 6000~6800，默认取 6500 留余量；可由 F0Mx 写入覆盖
	auto inst = ecfanGetInstance();
	uint16_t rpm = inst ? inst->readMaxRpm(index) : 6500;
	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, rpm);
	return SmcSuccess;
}

SMC_RESULT MaxKey::update(const SMC_DATA *src) {
	VirtualSMCValue::update(src);

	uint16_t rpm = VirtualSMCAPI::decodeIntFp(SmcKeyTypeFpe2, *reinterpret_cast<const uint16_t *>(src));
	auto inst = ecfanGetInstance();
	if (inst)
		inst->setMaxRpm(index, rpm);

	return SmcSuccess;
}

// 手动模式键读写
SMC_RESULT FanModeKey::readAccess() {
	auto inst = ecfanGetInstance();
	uint8_t m = inst ? inst->readManualMode(index) : 0;
	data[0] = m;
	return SmcSuccess;
}

SMC_RESULT FanModeKey::update(const SMC_DATA *src) {
	VirtualSMCValue::update(src);

	uint8_t m = src[0];
	auto inst = ecfanGetInstance();
	if (inst)
		inst->setManualMode(index, m);

	return SmcSuccess;
}

// ---- "FS! " 全局强制位掩码（ui16，大端）----
// 语义与真机 Apple SMC 一致：bit i = 1 表示风扇 i 处于强制手动。
// smcFanControl / TG Pro 这类软件的流程是「写 F0Tg 设速度 → 写 FS! 置位保持」，
// 只要 FS! 这个 key 不存在，它们第一步读到 key not found 就直接放弃调速。
// 反过来 bit 清 0 时交还 EC 固件自动控制。
SMC_RESULT ForceKey::readAccess() {
	auto inst = ecfanGetInstance();
	uint16_t mask = inst ? inst->readForceMask() : 0;
	data[0] = static_cast<uint8_t>(mask >> 8);
	data[1] = static_cast<uint8_t>(mask & 0xFF);
	return SmcSuccess;
}

SMC_RESULT ForceKey::update(const SMC_DATA *src) {
	VirtualSMCValue::update(src);

	uint16_t mask = static_cast<uint16_t>((src[0] << 8) | src[1]);

	auto inst = ecfanGetInstance();
	if (inst)
		inst->setForceMask(mask);

	return SmcSuccess;
}

// CPU 温度（只读）—— encodeIntSp：sp78 定点，摄氏度
SMC_RESULT TC0D::readAccess() {
	auto inst = ecfanGetInstance();
	uint8_t tmp = inst ? inst->readTemp() : 0;
	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntSp(SmcKeyTypeSp78, tmp);
	return SmcSuccess;
}

SMC_RESULT TC0P::readAccess() {
	auto inst = ecfanGetInstance();
	uint8_t tmp = inst ? inst->readTemp() : 0;
	*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntSp(SmcKeyTypeSp78, tmp);
	return SmcSuccess;
}

// ---- EC RAM 诊断直读（EC00..EC07）----
// 每块 32 字节，i*32 起始。用于扫描 EC 全表定位 GPU 温度寄存器。
SMC_RESULT ECDumpKey::readAccess() {
	auto inst = ecfanGetInstance();
	if (!inst) {
		memset(data, 0, 32);
		return SmcSuccess;
	}
	inst->readEcBlock(index * 32, data, 32);
	return SmcSuccess;
}

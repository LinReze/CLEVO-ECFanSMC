//
//  smc_core.h
//  FanControl — SMC 访问核心（app 与特权 helper 共用）
//

#ifndef SMC_CORE_H
#define SMC_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMC_MAX_FANS 2

typedef struct {
	uint8_t  cpuTemp;              // ℃
	uint16_t fanRpm[SMC_MAX_FANS]; // 实际转速
	uint16_t fanMin[SMC_MAX_FANS];
	uint16_t fanMax[SMC_MAX_FANS];
	uint16_t fanTarget[SMC_MAX_FANS];
	uint8_t  fanMode[SMC_MAX_FANS]; // 0=auto 1=manual
	uint16_t forceMask;             // "FS! " 位掩码
	uint8_t  fanCount;
	bool     valid;
} SMCState_t;

/** 打开 AppleSMC 连接，返回 0 成功 */
int  smc_open(void);
void smc_close(void);

/** 读取整机风扇/温度状态，返回 0 成功 */
int  smc_state(SMCState_t *out);

/**
 * 读取 GPU 温度。会依次尝试 TG0D / TG0P / TGDD 等键。
 * 返回 0 表示读到有效值（写入 *out），非 0 表示本机没有可用的 GPU 温度键。
 */
int  smc_gpu_temp(uint8_t *out);

/** 读取任意 key 的原始字节（诊断用），返回 0 成功 */
int  smc_read_bytes(const char *key, uint8_t *buf, uint32_t *size);

/** 设置风扇转速（需 root），返回 0 成功 */
int  smc_set_fan(int index, int rpm);
/** 交还自动（需 root） */
int  smc_set_auto(int index);
/** 探测是否有写权限（试写同值），返回 0 表示可写 */
int  smc_can_write(void);

#ifdef __cplusplus
}
#endif

#endif /* SMC_CORE_H */

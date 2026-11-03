//
//  smc_core.c
//  FanControl — SMC 读写核心
//
//  AppleSMC 用户态协议（与 VirtualSMC 官方 smc 工具 / Macs Fan Control 一致）：
//    IOServiceOpen("AppleSMC", type 0)
//    IOConnectCallStructMethod(conn, KERNEL_INDEX_SMC=2, &in, sizeof(SMCKeyData_t)=80, &out, &sz)
//    in.data8 决定动作：9=READ_KEYINFO(取 size/type)，5=READ_BYTES，6=WRITE_BYTES
//
//  结构体必须是完整的 80 字节版本（含 vers + pLimitData + keyInfo）。
//  精简成 56 字节会被 AppleSMCClient 判为 kIOReturnBadArgument(0xe00002c2)。
//
//  读操作普通用户即可；写操作返回 kIOReturnNotPrivileged(0xe00002c1)，必须 root。
//

#include "smc_core.h"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
	char major, minor, build, reserved[1];
	uint16_t release;
} SMCVers_t;

typedef struct {
	uint16_t version, length;
	uint32_t cpuPLimit, gpuPLimit, memPLimit;
} SMCPLimit_t;

typedef struct {
	uint32_t dataSize;
	uint32_t dataType;
	char     dataAttributes;
} SMCKeyInfo_t;

typedef char SMCBytes_t[32];

typedef struct {
	uint32_t      key;
	SMCVers_t     vers;
	SMCPLimit_t   pLimitData;
	SMCKeyInfo_t  keyInfo;
	char          result;
	char          status;
	char          data8;
	uint32_t      data32;
	SMCBytes_t    bytes;
} SMCKeyData_t;

#define KERNEL_INDEX_SMC     2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_KEYINFO 9

static io_connect_t gConn = 0;

static uint32_t smc_fourcc(const char *s) {
	return ((uint32_t)(unsigned char)s[0] << 24) | ((uint32_t)(unsigned char)s[1] << 16) |
	       ((uint32_t)(unsigned char)s[2] << 8)  |  (uint32_t)(unsigned char)s[3];
}

static int smc_call(SMCKeyData_t *in, SMCKeyData_t *out) {
	size_t outSize = sizeof(SMCKeyData_t);
	kern_return_t r = IOConnectCallStructMethod(gConn, KERNEL_INDEX_SMC,
	                                            in, sizeof(SMCKeyData_t), out, &outSize);
	if (r != kIOReturnSuccess)
		return (int)r;
	if (out->result != 0)
		return -100 - (int)(uint8_t)out->result;
	return 0;
}

int smc_open(void) {
	if (gConn)
		return 0;

	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
	                                               IOServiceMatching("AppleSMC"));
	if (!svc)
		return -1;

	kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &gConn);
	IOObjectRelease(svc);
	if (r != kIOReturnSuccess) {
		gConn = 0;
		return (int)r;
	}
	return 0;
}

void smc_close(void) {
	if (gConn) {
		IOServiceClose(gConn);
		gConn = 0;
	}
}

/** 读原始字节，size/type 可为 NULL */
static int smc_read_raw(const char *key, uint8_t *buf32, uint32_t *size, uint32_t *type) {
	if (!gConn && smc_open() != 0)
		return -1;

	SMCKeyData_t in, out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.key = smc_fourcc(key);
	in.data8 = SMC_CMD_READ_KEYINFO;
	int rc = smc_call(&in, &out);
	if (rc != 0)
		return rc;

	uint32_t dataSize = out.keyInfo.dataSize;
	if (dataSize == 0 || dataSize > 32)
		return -2;

	SMCKeyData_t in2, out2;
	memset(&in2, 0, sizeof(in2));
	memset(&out2, 0, sizeof(out2));
	in2.key = smc_fourcc(key);
	in2.keyInfo.dataSize = dataSize;
	in2.data8 = SMC_CMD_READ_BYTES;
	rc = smc_call(&in2, &out2);
	if (rc != 0)
		return rc;

	memcpy(buf32, out2.bytes, dataSize);
	if (size) *size = dataSize;
	if (type) *type = out.keyInfo.dataType;
	return 0;
}

static int smc_write_raw(const char *key, const uint8_t *buf, uint32_t size) {
	if (!gConn && smc_open() != 0)
		return -1;

	SMCKeyData_t in, out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.key = smc_fourcc(key);
	in.data8 = SMC_CMD_READ_KEYINFO;
	int rc = smc_call(&in, &out);
	if (rc != 0)
		return rc;

	SMCKeyData_t in2, out2;
	memset(&in2, 0, sizeof(in2));
	memset(&out2, 0, sizeof(out2));
	in2.key = smc_fourcc(key);
	in2.keyInfo.dataSize = size;
	in2.data8 = SMC_CMD_WRITE_BYTES;
	memcpy(in2.bytes, buf, size > 32 ? 32 : size);
	return smc_call(&in2, &out2);
}

static void smc_key_fan(char *dst, int index, char a, char b) {
	dst[0] = 'F';
	dst[1] = (char)('0' + index);
	dst[2] = a;
	dst[3] = b;
	dst[4] = '\0';
}

/* ---- 定点数编解码 ---- */

static uint16_t fpe2_decode(const uint8_t *b) {
	return (uint16_t)(((uint16_t)b[0] << 8) | b[1]) / 4;
}

static void fpe2_encode(uint16_t value, uint8_t *b) {
	uint16_t raw = (uint16_t)(value * 4);
	b[0] = (uint8_t)(raw >> 8);
	b[1] = (uint8_t)(raw & 0xFF);
}

static uint8_t sp78_decode(const uint8_t *b) {
	int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
	int v = raw / 256;
	if (v < 0) v = 0;
	if (v > 255) v = 255;
	return (uint8_t)v;
}

int smc_state(SMCState_t *out) {
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (!gConn && smc_open() != 0)
		return -1;

	uint8_t buf[32];
	uint32_t size = 0, type = 0;

	// 温度
	if (smc_read_raw("TC0D", buf, &size, &type) == 0 && size >= 2)
		out->cpuTemp = sp78_decode(buf);

	// 风扇数量（优先 FNum，失败则固定按 2 个探测）
	uint8_t count = 0;
	if (smc_read_raw("FNum", buf, &size, &type) == 0 && size >= 1)
		count = buf[0];
	if (count == 0 || count > SMC_MAX_FANS) {
		count = 0;
		for (int i = 0; i < SMC_MAX_FANS; i++) {
			char k[5];
			smc_key_fan(k, i, 'A', 'c');
			if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 2)
				count = (uint8_t)(i + 1);
		}
	}
	out->fanCount = count;

	for (int i = 0; i < count && i < SMC_MAX_FANS; i++) {
		char k[5];

		smc_key_fan(k, i, 'A', 'c');
		if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 2)
			out->fanRpm[i] = fpe2_decode(buf);

		smc_key_fan(k, i, 'M', 'n');
		if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 2)
			out->fanMin[i] = fpe2_decode(buf);

		smc_key_fan(k, i, 'M', 'x');
		if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 2)
			out->fanMax[i] = fpe2_decode(buf);

		smc_key_fan(k, i, 'T', 'g');
		if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 2)
			out->fanTarget[i] = fpe2_decode(buf);

		smc_key_fan(k, i, 'M', 'd');
		if (smc_read_raw(k, buf, &size, &type) == 0 && size >= 1)
			out->fanMode[i] = buf[0] ? 1 : 0;
	}

	if (smc_read_raw("FS! ", buf, &size, &type) == 0 && size >= 2)
		out->forceMask = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);

	out->valid = true;
	return 0;
}

int smc_set_fan(int index, int rpm) {
	if (index < 0 || index >= SMC_MAX_FANS)
		return -2;

	char k[5];
	uint8_t buf[32];

	// 与 Macs Fan Control 一致：先切手动模式，再写目标转速
	smc_key_fan(k, index, 'M', 'd');
	buf[0] = 1;
	int rc = smc_write_raw(k, buf, 1);
	if (rc != 0)
		return rc;

	smc_key_fan(k, index, 'T', 'g');
	fpe2_encode((uint16_t)(rpm < 0 ? 0 : rpm), buf);
	return smc_write_raw(k, buf, 2);
}

int smc_set_auto(int index) {
	if (index < 0 || index >= SMC_MAX_FANS)
		return -2;

	char k[5];
	uint8_t buf[32];

	// 目标转速清零，再交还自动
	smc_key_fan(k, index, 'T', 'g');
	fpe2_encode(0, buf);
	smc_write_raw(k, buf, 2);

	smc_key_fan(k, index, 'M', 'd');
	buf[0] = 0;
	return smc_write_raw(k, buf, 1);
}

int smc_can_write(void) {
	if (!gConn && smc_open() != 0)
		return -1;

	// 用一个无害的探测：把 F0Tg 写成它当前的值（不改状态），看是否被拒绝
	uint8_t buf[32];
	uint32_t size = 0, type = 0;
	if (smc_read_raw("F0Tg", buf, &size, &type) != 0 || size < 2)
		return -1;
	return smc_write_raw("F0Tg", buf, 2);
}

int smc_gpu_temp(uint8_t *out) {
	if (!out)
		return -1;

	// GPU 温度不在 SMC 里，而是 Nvidia/AMD 的 Metal 加速器节点 NVAccel 的
	// PerformanceStatistics 字典里的 "Temperature(C)"。
	// MFC 也是从这里读的，与 EC/我们的 kext 无关。
	io_iterator_t iter = 0;
	if (IOServiceGetMatchingServices(kIOMainPortDefault,
	                                 IOServiceMatching("IOAccelerator"), &iter) != kIOReturnSuccess)
		return -2;

	io_service_t s = 0;
	int result = -2;
	while ((s = IOIteratorNext(iter))) {
		CFTypeRef ps = IORegistryEntryCreateCFProperty(s, CFSTR("PerformanceStatistics"),
		                                               kCFAllocatorDefault, 0);
		if (ps && CFGetTypeID(ps) == CFDictionaryGetTypeID()) {
			CFTypeRef temp = CFDictionaryGetValue((CFDictionaryRef)ps, CFSTR("Temperature(C)"));
			if (temp && CFGetTypeID(temp) == CFNumberGetTypeID()) {
				double t = 0;
				CFNumberGetValue((CFNumberRef)temp, kCFNumberDoubleType, &t);
				if (t > 0 && t < 200) {
					*out = (uint8_t)(t + 0.5);
					result = 0;
					CFRelease(ps);
					IOObjectRelease(s);
					break;
				}
			}
			CFRelease(ps);
		}
		IOObjectRelease(s);
	}
	IOObjectRelease(iter);
	return result;
}

int smc_read_bytes(const char *key, uint8_t *buf, uint32_t *size) {
	return smc_read_raw(key, buf, size, NULL);
}

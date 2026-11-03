//
//  ecfdump — 打印 ECFanSMC 暴露的 EC RAM 全表（EC00..EC07）
//
//  用途：定位 EC 里的第二个温度寄存器（GPU 温度）。
//  用法：
//      ./ecfdump            打印一次
//      ./ecfdump -w 2       每 2 秒刷新一次，观察哪个字节随负载变化
//
//  编译: clang -O2 -I. -o ecfdump ecfdump.c smc_core.c -framework IOKit -framework CoreFoundation
//

#include "smc_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int dump_once(void) {
	uint8_t ec[256];
	memset(ec, 0, sizeof(ec));
	int ok = 1;

	for (int i = 0; i < 8; i++) {
		char key[5] = { 'E', 'C', '0', (char)('0' + i), 0 };
		uint8_t buf[32];
		uint32_t size = 0;
		if (smc_read_bytes(key, buf, &size) != 0 || size < 32) {
			fprintf(stderr, "read %s failed\n", key);
			ok = 0;
			continue;
		}
		memcpy(ec + i * 32, buf, 32);
	}

	if (!ok)
		return 1;

	SMCState_t s;
	int haveState = (smc_state(&s) == 0);

	printf("        ");
	for (int c = 0; c < 16; c++)
		printf("%02X ", c);
	printf("\n");

	for (int row = 0; row < 16; row++) {
		printf("0x%02X  | ", row * 16);
		for (int c = 0; c < 16; c++) {
			uint8_t v = ec[row * 16 + c];
			// 20..99 之间的值很可能是温度，高亮显示
			if (v >= 20 && v <= 99)
				printf("\033[33m%02X\033[0m ", v);
			else if (v == 0)
				printf("\033[90m00\033[0m ");
			else
				printf("%02X ", v);
		}
		printf("\n");
	}

	if (haveState) {
		printf("\n参考：CPU=%u°C  风扇0=%u RPM  风扇1=%u RPM\n",
		       s.cpuTemp, s.fanRpm[0], s.fanRpm[1]);
	}
	return 0;
}

int main(int argc, char **argv) {
	if (smc_open() != 0) {
		fprintf(stderr, "smc_open failed (AppleSMC 不可用)\n");
		return 1;
	}

	int interval = 0;
	if (argc >= 3 && strcmp(argv[1], "-w") == 0)
		interval = atoi(argv[2]);

	if (interval <= 0) {
		int rc = dump_once();
		smc_close();
		return rc;
	}

	while (1) {
		printf("\033[2J\033[H");
		dump_once();
		fflush(stdout);
		sleep(interval);
	}
}

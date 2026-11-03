// 读取 AppleSMC 的全部键，重点看风扇相关键是否存在（FNum / F0Ac / F0Tg / F0Md ...）
// 编译: clang -O2 -o smc_probe smc_probe.c -framework IOKit -framework CoreFoundation
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

typedef struct { char major, minor, build, reserved; UInt16 release; } SMCKeyData_vers_t;
typedef struct { UInt16 version, length; UInt32 cpuPLimit, gpuPLimit, memPLimit; } SMCKeyData_pLimitData_t;
typedef struct { UInt32 dataSize, dataType; char dataAttributes; } SMCKeyData_keyInfo_t;
typedef char SMCBytes_t[32];

typedef struct {
    UInt32 key;
    SMCKeyData_vers_t vers;
    SMCKeyData_pLimitData_t pLimitData;
    SMCKeyData_keyInfo_t keyInfo;
    char result, status, data8;
    UInt32 data32;
    SMCBytes_t bytes;
} SMCKeyData_t;

enum { kSMCUserClientOpen = 0, kSMCUserClientClose = 1, kSMCHandleYPCEvent = 2,
       kSMCReadKey = 5, kSMCGetKeyCount = 7, kSMCGetKeyFromIndex = 8, kSMCGetKeyInfo = 9 };

static io_connect_t conn = 0;

static kern_return_t call(int idx, SMCKeyData_t *in, SMCKeyData_t *out) {
    size_t sz = sizeof(SMCKeyData_t);
    return IOConnectCallStructMethod(conn, idx, in, sizeof(SMCKeyData_t), out, &sz);
}

static UInt32 fourcc(const char *s) {
    return ((UInt32)(unsigned char)s[0] << 24) | ((UInt32)(unsigned char)s[1] << 16) |
           ((UInt32)(unsigned char)s[2] << 8) | (UInt32)(unsigned char)s[3];
}

static void keystr(UInt32 k, char *b) {
    b[0] = (k >> 24) & 0xff; b[1] = (k >> 16) & 0xff; b[2] = (k >> 8) & 0xff; b[3] = k & 0xff; b[4] = 0;
}

static kern_return_t keyInfo(const char *name, SMCKeyData_t *out) {
    SMCKeyData_t in; memset(&in, 0, sizeof in);
    in.key = fourcc(name); in.data8 = kSMCGetKeyInfo;
    return call(kSMCHandleYPCEvent, &in, out);
}

static kern_return_t readKey(UInt32 key, SMCKeyData_t *info, SMCKeyData_t *out) {
    SMCKeyData_t in; memset(&in, 0, sizeof in);
    in.key = key; in.keyInfo.dataSize = info->keyInfo.dataSize; in.data8 = kSMCReadKey;
    return call(kSMCHandleYPCEvent, &in, out);
}

int main(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { printf("找不到 AppleSMC\n"); return 1; }
    if (IOServiceOpen(svc, mach_task_self(), 0, &conn) != kIOReturnSuccess) { printf("打不开 AppleSMC\n"); return 1; }
    int dummy = 0; (void)dummy;
    IOConnectCallScalarMethod(conn, kSMCUserClientOpen, 0, 0, 0, 0);

    // 键总数
    SMCKeyData_t in, out;

    // FNum
    SMCKeyData_t fi;
    if (keyInfo("FNum", &fi) == kIOReturnSuccess) {
        SMCKeyData_t ro;
        if (readKey(fourcc("FNum"), &fi, &ro) == kIOReturnSuccess)
            printf("FNum（风扇数量）= %d\n", (int)(unsigned char)ro.bytes[0]);
    } else printf("FNum: 键不存在\n");
    printf("\n");

    printf("=== 所有 F 开头的键（风扇/传感器） ===\n");
    int foundFan = 0;
    for (UInt32 i = 0; i < 4000; i++) {
        memset(&in, 0, sizeof in);
        in.data8 = kSMCGetKeyFromIndex; in.data32 = i;
        if (call(kSMCHandleYPCEvent, &in, &out) != kIOReturnSuccess) break;
        char n[5]; keystr(out.key, n);
        if (n[0] != 'F') continue;
        SMCKeyData_t ki, ro;
        if (keyInfo(n, &ki) != kIOReturnSuccess) continue;
        char t[5]; keystr(ki.keyInfo.dataType, t);
        double v = 0; char display[64] = "";
        if (readKey(fourcc(n), &ki, &ro) == kIOReturnSuccess) {
            if (!strcmp(t, "flt ")) { memcpy(&v, ro.bytes, 4); snprintf(display, sizeof display, "%.2f", v); }
            else if (!strcmp(t, "ui8 ")) { snprintf(display, sizeof display, "%u", (unsigned char)ro.bytes[0]); }
            else if (!strcmp(t, "ui16")) { snprintf(display, sizeof display, "%u", *(UInt16 *)ro.bytes); }
            else if (!strcmp(t, "ui32")) { snprintf(display, sizeof display, "%u", *(UInt32 *)ro.bytes); }
            else snprintf(display, sizeof display, "-");
        }
        printf("  %-4s  type=%-4s  size=%2u  value=%s\n", n, t, (unsigned)ki.keyInfo.dataSize, display);
        if (n[0] == 'F' && n[1] >= '0' && n[1] <= '9') foundFan = 1;
    }
    printf("\n结论: %s\n", foundFan ? "存在 F0xx 风扇键" : "没有任何 F0xx 风扇键 —— 风扇工具必然失效");

    IOConnectCallScalarMethod(conn, kSMCUserClientClose, 0, 0, 0, 0);
    IOServiceClose(conn);
    IOObjectRelease(svc);
    return 0;
}

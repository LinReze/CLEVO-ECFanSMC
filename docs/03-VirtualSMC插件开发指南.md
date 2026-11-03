# 03 · VirtualSMC 插件开发指南

> 写（或改）风扇类 VirtualSMC 插件必读。本文的"坑"全部来自实测翻车——
> 每一条都对应一次真实的故障现象，照着做可以少走几天弯路。

## 一、SMC 风扇键位语义（Apple 规范）

| 键 | 类型 | 属性 | 含义 | 实现要点 |
|---|---|---|---|---|
| `FNum` | `ui8 ` | R | 风扇数量 | 返回 2 |
| `F0Ac` `F1Ac` | `fpe2` | R | 实际转速 | 读 EC，随负载变化 |
| `F0Tg` `F1Tg` | `fpe2` | RW | **目标转速** | **回读必须返回写入值**（见坑 5） |
| `F0Mn` `F1Mn` | `fpe2` | RW | 转速下限 | 需可写（见坑 6） |
| `F0Mx` `F1Mx` | `fpe2` | RW | 转速上限 | 同上 |
| `F0Md` `F1Md` | `ui8 ` | RW | 模式：0=自动 1=手动 | |
| `F0ID` `F1ID` | `fds ` | R | 16 字节风扇描述符 | 缺了工具置灰（见坑 6） |
| `"FS! "` | `ui16` | RW | 强制位掩码 bit i=风扇 i 手动 | 缺了判定"不支持"（见坑 6） |
| `TC0D` `TC0P` | `sp78` | R | CPU 温度 | 有符号 8.8 定点 |

类型编码说明：

- **`fpe2`**（风扇转速）：无符号 14.2 定点，`encodeIntFp(SmcKeyTypeFpe2, rpm)` 内部
  已做字节序交换，直接 `*reinterpret_cast<uint16_t*>(data) = encodeIntFp(...)` 即可。
- **`sp78`**（温度）：有符号 8.8 定点，`value= temp * 256`。
- **`fds `**（风扇描述符）：16 字节 = 4 字节头（类型/子类型/位置/flag）+ 12 字节名字，
  照抄 VirtualSMC 官方 `SMCDellSensors` 的 `FanTypeDescStruct` 布局。

## 二、keystore 的写路径（核心机制）

VirtualSMC 处理用户写入分**两步**，钩子别放错：

```
用户写 F0Tg
   → writeAccess()    ← 只做权限检查（属性无 WRITE 标志会在这里拒绝）
   → update(src)      ← 真正的写入逻辑必须在这里！
```

```cpp
class F0Tg : public VirtualSMCValue {
protected:
    // ❌ 不要把写 EC 的动作放 writeAccess()——它只是权限检查
    SMC_RESULT update(const SMC_DATA *src) override {
        VirtualSMCValue::update(src);          // 先让基类存下字节
        uint16_t rpm = VirtualSMCAPI::decodeIntFp(
            SmcKeyTypeFpe2, *reinterpret_cast<const uint16_t *>(src));
        targetRpm[index] = rpm;                // 缓存目标值（坑 5）
        inst->setFanRpm(index, rpm);           // 写 EC
        return SmcSuccess;
    }
};
```

官方参照：VirtualSMC 仓库 `Sensors/SMCDellSensors`（Dell 笔记本风扇插件），
它的 `TargetKey`/`ManualKey` 全部 override `update`。开发前先读它的源码。

注册键时注意属性组合：

```cpp
VirtualSMCAPI::addKey(KeyF0Tg(i), vsmcPlugin.data,
    VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new F0Tg(i),
        static_cast<SMC_KEY_ATTRIBUTES>(SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE)));
// READ=0x80, WRITE=0x40
```

## 三、八个必踩的坑（按翻车顺序）

### 坑 1：转速字节序——读出 8194/9986 这种离谱值

EC 的 16bit 转速是**大端**（高字节在前）：

```c
raw = base[off] | (base[off+1] << 8);   // ❌ 小端
raw = (base[off] << 8) | base[off+1];   // ✅ 大端
```

且必须换算：`RPM = 2156220 / raw`，不是直接把 raw 当转速。

### 坑 2：键重复注册——写什么都 NotWritable

`start()` 里手动 `new` + `start()` 之后又调 `registerService()`，会触发 IOKit
二次匹配，`addKey` 跑两遍 → keystore 出现重复键 → `getByName` 二分查找错乱，
表现为明明是 READ|WRITE 的键却返回 `0xe00002c1`（NotWritable）。

```cpp
static bool gStarted = false;   // ✅ 防重入
if (gStarted) return true;
gStarted = true;
```

### 坑 3：写逻辑放错钩子

见上一节 `writeAccess()` vs `update()`。现象：权限检查通过，EC 毫无动作。

### 坑 4：`ml_io_write8` 不是端口 IO——一开工具就 panic

内核导出的 `ml_io_read8/ml_io_write8` 第一参数是 **`uintptr_t` 虚拟地址**
（给映射后的内存用的），不是端口号：

```c
ml_io_write8(0x66, cmd);   // ❌ 0x66 被当虚拟地址解引用 → panic（实测一开 MFC 就死机）
ml_io_write8((uintptr_t)mapped_ptr, val);   // ✅ 它的正确用法
```

端口 IO 必须用内联汇编 `inb/outb`（见 02 篇第五节），编译后用
`otool -t` 反汇编确认生成了 `ee`/`ec` 指令再上机器。

### 坑 5：`F0Tg` 回读必须等于写入值——"点 Manual 跳回 Auto"的元凶

Apple SMC 的 `F0Tg` 是**目标转速寄存器**，语义上写什么读回什么。
如果回读的是实际转速（EC 响应滞后，值对不上），所有做"写后回读校验"的工具
都会判定写入失败、回滚自动——表现为只有不做校验的工具能用。

```cpp
SMC_RESULT F0Tg::readAccess() {
    uint16_t v = (targetRpm[i] > 0) ? targetRpm[i]    // ✅ 优先返回缓存
                                    : inst->readFanRpm(i);  // 从未设过才兜底实际值
    ...
}
```

### 坑 6：缺 `FS! ` / `F0ID` / 可写的 `F0Mn`·`F0Mx`——"不支持风扇控制"

smcFanControl / TG Pro 类工具的探测逻辑：

1. 读 `"FS! "`（强制位掩码）——键不存在直接判定"本机不支持风扇控制"；
2. 枚举 `F0ID` 风扇描述符——缺了 UI 里风扇不显示/置灰；
3. 调速前先写 `F0Mn`/`F0Mx` 区间——键只读会返回 NotWritable，流程中止。

三个都补上（写入进缓存即可，不必动硬件）。

### 坑 7：占空比下限把最低转速钳死

"拉到最低只是变慢一点"——多半是自己加的保护：

```c
if (pct < 30) pct = 30;   // ❌ 好心办坏事，最低档被抬到 30%
```

要能拉到真最低就放开到 0%（或按需设 5~10%），让用户自己选。

### 坑 8：用户态 SMC 结构体必须 80 字节

`SMCKeyData_t` 完整布局是 `key(4) + vers(6) + pLimitData(16) + keyInfo(12) +
result/status/data8(3) + data32(4) + bytes(32) = 80 字节`。
自己精简成 56 字节会被判 `0xe00002c2`（BadArgument，很容易误以为是 Unsupported）。
参考 VirtualSMC `Tools/smc-fuzzer/smc.h`。另外：**读不需要特权，写必须 root**。

## 四、编译环境

三个源码树缺一不可（`./build.sh` 自动处理）：

| 依赖 | 提供 |
|---|---|
| MacKernelSDK (acidanthera) | 内核头文件 + `libkmod.a` |
| Lilu 源码 | `Headers/` + `Library/plugin_start.cpp` |
| VirtualSMC 源码 | `VirtualSMCSDK/kern_vsmcapi.hpp` 等 |

关键编译标志（`build.sh` 里都有注释）：

```
-mkernel -mcmodel=kernel     # kext 规范参数
-fapple-kext                 # C++ new/异常模型
-ffreestanding               # 修 stdatomic 宏
-nostdlib -r -Wl,-kext       # 链接 libkmod.a + libkmodc++.a
```

> `RIP-relative overflow target=0x0` 报错 = **符号没解析出来**（内核不导出），
> 不是寻址问题。用 `check-symbols.sh` 审计二进制的未定义符号，把不导出的
> 函数换成内联实现。`qsort`、`withPhysicalAddress`、`ml_io_write8`(误用) 都是这么踩的。

## 五、调试技巧

- `SYSLOG("ecfan", ...)` 输出到系统日志：`log show --last boot --predicate 'eventMessage CONTAINS "ecfan"'`
  （注意默认级别可能看不到 info 级，加 `--info`；查不到不等于没跑）
- 用户态枚举全表验证注册结果（`tools/smc_probe.c`）——每个键应该只出现一次
- `kextstat | grep -i <name>` 确认加载；加载失败先怀疑没走 OpenCore，再怀疑符号

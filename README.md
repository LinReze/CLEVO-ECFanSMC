# ECFanSMC

以下信息由AI生成，如不明白可以将此项目扔给AI处理



**蓝天 / 同方（Clevo / Tongfang）模具笔记本的 macOS 风扇调速内核扩展** —— VirtualSMC 插件。

把嵌入式控制器（EC）里的风扇转速 / 温度 / 占空比桥接成标准 Apple SMC 键（`FNum` `F0Ac` `F0Tg` `F0Mn` `F0Mx` `F0ID` `FS! ` `TC0D` …），让 Macs Fan Control、TG Pro、smcFanControl 等现成工具**无需改动**即可读取和控制风扇；同时通过 EC 私有命令 `0x99` 实现真正的手动调速（设置占空比 0–255）与交还自动。

```
实测参考机型：七彩虹隐星 P15 23（海外版 COLORFUL EVOL P15 23，型号代码 P15 23-HE55D16512A-B 系）
蓝天/同方体系公模 · ITE IT5570E-128 EC · 双风扇
12/13 代 H 标压 + RTX 40 系 Laptop 各配置通用（12450H/12500H/12650H/12700H/13620H + 4050/4060）
读转速误差 < 3% · 最低 1207 RPM / 最高 6675 RPM · 手动/自动切换稳定
```

## 特性

- **读**：CPU 温度（`TC0D`/`TC0P`，sp78）、双风扇实时转速（`F0Ac/F1Ac`，fpe2，大端 + `2156220/raw` 换算）、占空比回读、EC RAM 全表诊断键（`EC00`~`EC07`）
- **写**：目标转速（`F0Tg`）、最小/最大区间（`F0Mn`/`F0Mx`）、手动/自动模式（`F0Md`）、强制位掩码（`FS! `）——写路径走 EC 命令 `0x99`
- **兼容性**：完整实现 Apple SMC 风扇键语义（含 `F0Tg` 回读缓存），通过 Macs Fan Control / TG Pro / Stats / smcFanControl 等主流工具的写后校验
- **安全**：等待握手带超时（EC 无响应不会挂死内核）；风扇 index 1-based；支持 `release` 命令一键交还 EC 固件自动控制

## 兼容范围

| 项       | 说明                                                                                                                      |
| ------- | ----------------------------------------------------------------------------------------------------------------------- |
| 模具      | 蓝天/同方（Clevo/Tongfang）体系公模——EC 为 ITE IT55xx 系、数据区内存映射 `0xFE0B0100` 一带的机型。已实测：七彩虹隐星 P15 23（EVOL P15 23）、神舟 TX8（n960kp）同协议 |
| macOS   | 10.15+（x86_64），实测 15.x                                                                                                  |
| 引导      | OpenCore（需 Lilu + VirtualSMC）                                                                                           |
| CPU/GPU | 无关（kext 只访问 EC，不感知处理器型号；同模具不同配置直接通用）                                                                                    |

> **不同模具的 EC 固件可能不同**：寄存器偏移与命令码不保证一致。请先按  
> [docs/05-移植到其他模具.md](docs/05-移植到其他模具.md) 的流程做只读验证，再开写路径。

## 快速开始

**不想编译？直接用成品**：[`dist/ECFanSMC-0.2.1.zip`](dist/ECFanSMC-0.2.1.zip)  
（或 `dist/ECFanSMC.kext` 目录），解开后跳到第 2 步。  
二进制与源码同一版本（0.2.1，x86_64）：

```
ECFanSMC（二进制）  sha256 92300d7dd42ab92c99dea73b0f9457783f87cc1fa477b96baed177e1163f3fa1
ECFanSMC-0.2.1.zip  sha256 a63677a2aa357492...
```

1. 编译（macOS + Xcode CLT，首次自动拉取依赖到 `./deps/`）：
   ```bash
   cd ECFanSMC
   ./build.sh
   ```
2. 部署：把 `ECFanSMC.kext`（编译产物在 `ECFanSMC/build/`）拷到 `EFI/OC/Kexts/`，  
   config.plist 里加一条 `Kernel → Add`（参照 [docs/01-快速上手.md](docs/01-快速上手.md) 的清单），重启。
3. 验证：`smc_probe`（`tools/`）应看到 `FNum=2`、`F0Ac` 有合理转速；再用  
   Macs Fan Control 试手动调速。

## 仓库结构

```
dist/              预编译成品（不想编译直接用）
  ECFanSMC.kext    可直接部署的 kext（x86_64，0.2.1）
  ECFanSMC-0.2.1.zip
ECFanSMC/          kext 源码（build.sh 一键编译，check-symbols.sh 符号审计）
tools/             用户态工具
  smc_probe.c      枚举 AppleSMC 全部键（判断系统里有没有风扇键、验证注册结果）
  ecfdump.c        扫描 EC RAM 全表（EC00..EC07 诊断键，定位未知寄存器）
  smc_core.c/.h    用户态 SMC 读写核心（读不需要特权；写要 root）
docs/              使用手册（按顺序读即可上手，见下）
```

## 文档（使用手册）

| 文档                                               | 内容                                |
| ------------------------------------------------ | --------------------------------- |
| [01-快速上手](docs/01-快速上手.md)                       | 安装、config.plist 配置、验证、日常使用        |
| [02-EC协议参考](docs/02-EC协议参考.md)                   | EC 寄存器地图、0x99 调速协议、RPM 换算、ECMD 通道 |
| [03-VirtualSMC插件开发](docs/03-VirtualSMC插件开发指南.md) | SMC 键位语义、keystore 写路径、必须避开的 8 个坑  |
| [04-调试与排错](docs/04-调试与排错.md)                     | 测试方法论、典型故障排查表、内核调试                |
| [05-移植到其他模具](docs/05-移植到其他模具.md)                 | 换模具/换机型时如何定位自己的 EC 布局             |

## 工作原理（一图流）

```
风扇硬件 ── ITE EC ──┬── EC RAM 0xFE0B0100（内存映射，只读镜像：RPM/DUT/TMP）
                     └── 端口 0x62/0x66（ACPI EC 命令通道）
                              │  命令 0x99: set duty / release
                              ▼
                    ECFanSMC.kext（本项目）
                              │  注册标准 SMC 键
                              ▼
                    VirtualSMC → AppleSMC
                              │
                              ▼
              Macs Fan Control / TG Pro / 任意 SMC 工具
```

## 安全提示

- 写 EC 有风险：**先只读验证、再小步写入**（流程见 docs/05）。异常时立即执行  
  `release`（交还自动）或断电重启，EC 固件会恢复自带的温控曲线。
- **不要同时运行两个风扇控制程序**（如 Macs Fan Control 与本插件配套工具），  
  它们会互相覆写目标转速。
- 本项目与 EC 固件交互仅限风扇/温度寄存器与 0x99 命令，未触碰键盘/电池等其他 EC 功能。

## 致谢

- [Acidanthera](https://github.com/acidanthera) 的 Lilu / VirtualSMC / MacKernelSDK
- VirtualSMC 官方 `SMCDellSensors`（Dell 笔记本风扇插件）—— SMC 键位语义的参照实现
- `fanctrl.c`（KB3700 协议参考实现）与 Tuxedo Drivers 的 `ec_access.cc` —— RPM 换算公式来源
- Linux `cfc`（libclevoec）—— 0x99 调速命令的逆向来源

## License

BSD 3-Clause（见 [LICENSE](LICENSE)）。依赖组件遵循各自许可证。

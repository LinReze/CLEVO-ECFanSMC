#!/bin/bash
# 符号审计：找出"内核没导出、依赖也没提供"的未定义符号
#
# 为什么需要它：
#   OC 注入失败时的报错是 "RIP-relative overflow ... target=0x0"。
#   target=0x0 的真实含义是**符号解析到了地址 0**，不是寻址范围不够。
#   历史上踩过两个：
#     - IOMemoryDescriptor::withPhysicalAddress → 编出 32 位变体 withPhysicalAddressEjjj，
#       内核只导出 64 位 withPhysicalAddressEyyj
#     - qsort → 内核只有 _kx_qsort，没有标准 C++ 那个
#   这个脚本把同类问题在开机前就抓出来。
#
# 用法：./check-symbols.sh [kext二进制路径]

set -u

BIN="${1:-build/ECFanSMC.kext/Contents/MacOS/ECFanSMC}"
[ -f "$BIN" ] || { echo "找不到 $BIN"; exit 1; }

KERNEL="/System/Library/Kernels/kernel"
if [ ! -f "$KERNEL" ]; then
    echo "跳过：本机没有 $KERNEL（新版 macOS 内核在缓存里），改用手工比对"
    echo "  可参考：nm -u $BIN"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

nm -u "$BIN" | awk '{print $NF}' | sort -u > "$TMP/undef.txt"
nm "$KERNEL" 2>/dev/null | awk '$2 ~ /^[TtDdBbSsRr]$/ {print $3}' | sort -u > "$TMP/kernel.txt"
comm -23 "$TMP/undef.txt" "$TMP/kernel.txt" > "$TMP/missing.txt"

# 白名单：这些由 Lilu / VirtualSMC 依赖 kext 提供，或属于 C++ 运行时/虚表槽位
grep -Ev 'VirtualSMCAPI|VirtualSMCValue|LiluAPI|^_lilu$|^_lilu_os_log$|__Z5qsort|__ZTV|__ZTI|__ZTS|_RESERVED|__cxa|__gxx|_Znwm|_ZdlPv|_ZdlPvm|__Znwm|__ZdlPv' \
    "$TMP/missing.txt" > "$TMP/bad.txt" || true

COUNT=$(wc -l < "$TMP/undef.txt" | tr -d ' ')
echo "未定义符号总数：$COUNT"

if [ -s "$TMP/bad.txt" ]; then
    echo ""
    echo "!! 危险：以下符号内核未导出，注入时会解析成 0 → RIP-relative overflow"
    echo "------------------------------------------------------------"
    sed 's/^/   /' "$TMP/bad.txt"
    echo "------------------------------------------------------------"
    echo "处理：换用内核导出的等价 API，或去掉该调用。"
    exit 1
fi

echo "符号审计通过 ✅"

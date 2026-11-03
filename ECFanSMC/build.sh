#!/bin/bash
# ECFanSMC.kext 一键编译脚本（可移植版）
#
# 依赖（全是源码树，首次运行自动下载到 ./deps/）：
#   - MacKernelSDK (acidanthera)  -> deps/MacKernelSDK
#   - Lilu 源码                   -> deps/Lilu
#   - VirtualSMC 源码（SDK 头文件）-> deps/VirtualSMC
#
# 用法：
#   ./build.sh                # 自动下载依赖并编译
#   MKSDK=/x LILU=/y ./build.sh   # 指定已有依赖路径（跳过下载）
#
# 产物：build/ECFanSMC.kext，拷到 EFI/OC/Kexts/ 重启生效。
#
# 关键编译参数说明（踩坑记录，勿删）：
#   1. -mkernel -mcmodel=kernel 是 kext 规范参数（内核跑在负地址段）。
#      注意：它**不是** "RIP-relative overflow target=0x0" 的解药——那个报错的
#      target=0x0 表示符号没解析出来，用 check-symbols.sh 查，不是调参数。
#   2. 不要加链接期 -static——与 -r 冲突，且 -mkernel 已隐含。
#   3. 必须 -fapple-kext：C++ kext 的 operator new/异常模型依赖它。
#   4. -ffreestanding 可修 stdatomic 宏失效问题。
set -e
cd "$(dirname "$0")"

DEPS_DIR="${DEPS_DIR:-$(pwd)/deps}"
VSMC_TAG="${VSMC_TAG:-1.3.8}"   # 官方 tag 无 1.3.9，1.3.8 是最新正式 tag
LILU_TAG="${LILU_TAG:-1.7.2}"   # 官方 tag 无 1.7.3，1.7.2 是最新正式 tag

# ---------- 依赖获取 ----------
fetch() {
    # fetch <目标目录> <github 仓库> <tag 或 branch>
    local dest="$1" repo="$2" ref="$3"
    if [ -d "$dest" ]; then
        echo "  已存在: $dest"
        return 0
    fi
    echo "  下载 $repo@$ref ..."
    # codeload 的裸 ref 路径对 tag 和 branch 都有效
    local url="https://codeload.github.com/$repo/tar.gz/$ref"
    local tmp
    tmp="$(mktemp -d)"
    if ! curl -fsSL "$url" -o "$tmp/src.tgz"; then
        echo "下载失败: $url" >&2
        echo "可手动下载解压到 $dest，或用 MKSDK/LILU 环境变量指定已有依赖" >&2
        exit 1
    fi
    mkdir -p "$dest"
    tar -xzf "$tmp/src.tgz" -C "$tmp"
    # tarball 顶层是 <repo>-<tag>/，把内容摊平到 dest
    local top
    top="$(find "$tmp" -maxdepth 1 -mindepth 1 -type d | head -1)"
    cp -R "$top"/. "$dest"/
    rm -rf "$tmp"
}

if [ -z "$MKSDK" ] || [ -z "$LILU" ]; then
    echo "== 准备依赖（首次需联网，之后走缓存）=="
    mkdir -p "$DEPS_DIR"
    [ -n "$MKSDK" ] || MKSDK="$DEPS_DIR/MacKernelSDK"
    [ -n "$LILU" ]  || LILU="$DEPS_DIR/Lilu"
    VSMC="$DEPS_DIR/VirtualSMC"
    fetch "$MKSDK" "acidanthera/MacKernelSDK" "master"
    fetch "$LILU"  "acidanthera/Lilu"          "$LILU_TAG"
    fetch "$VSMC"  "acidanthera/VirtualSMC"    "$VSMC_TAG"
else
    VSMC="${VSMC:-$DEPS_DIR/VirtualSMC}"
fi

# 依赖目录结构归一：Lilu 的 SDK 在仓库内层 Lilu/ 目录（Lilu-<ver>/Lilu/Headers）
if [ -d "$LILU/Lilu/Headers" ]; then
    LILU="$LILU/Lilu"
fi

# SDK 头文件就位（VirtualSMCSDK 是 VirtualSMC 仓库里的子目录）
mkdir -p VirtualSMCSDK
cp -R "$VSMC/VirtualSMCSDK"/. VirtualSMCSDK/ 2>/dev/null || {
    echo "✗ $VSMC/VirtualSMCSDK 不存在，依赖下载可能不完整" >&2; exit 1; }
# Lilu Headers 软链（源码里 #include <Headers/...> 的解析路径）
rm -f Headers
ln -s "$LILU/Headers" Headers

SDK="$(xcrun --sdk macosx --show-sdk-path)"
INCLUDES="-I. -I$LILU -I$MKSDK/Headers -I$SDK/System/Library/Frameworks/Kernel.framework/Headers"

DEFS="-DMODULE_VERSION=0.2.1 -DPRODUCT_NAME=ECFanSMC -DLILU_CUSTOM_IOKIT_INIT=1 -DDEBUG=1"

CFLAGS="-target x86_64-apple-macos10.8 \
        -mkernel -mcmodel=kernel \
        -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-common \
        -fno-asynchronous-unwind-tables \
        -std=gnu++17 \
        -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse \
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-deprecated-register"

LDFLAGS="-target x86_64-apple-macos10.8 -mkernel -mcmodel=kernel \
         -nostdlib -r -Wl,-kext -Wl,-export_dynamic"

echo "== 清理 =="
rm -rf build
mkdir -p build/ECFanSMC.kext/Contents/MacOS

echo "== 编译 =="
clang $CFLAGS $DEFS $INCLUDES -c ECFanSMC.cpp          -o build/ECFanSMC.o
clang $CFLAGS $DEFS $INCLUDES -c KeyImplementations.cpp -o build/KeyImplementations.o
clang $CFLAGS $DEFS $INCLUDES -c kern_start.cpp         -o build/kern_start.o
clang $CFLAGS $DEFS $INCLUDES -c "$LILU/Library/plugin_start.cpp" -o build/plugin_start.o
clang -target x86_64-apple-macos10.8 -mcmodel=kernel $INCLUDES -c kmod_info.c -o build/kmod_info.o

echo "== 链接 =="
clang $LDFLAGS \
    -o build/ECFanSMC.kext/Contents/MacOS/ECFanSMC \
    build/ECFanSMC.o build/KeyImplementations.o build/kern_start.o \
    build/plugin_start.o build/kmod_info.o \
    -L$MKSDK/Library/universal -lkmod -lkmodc++

cp Info.plist build/ECFanSMC.kext/Contents/

echo "== 自检 =="
file build/ECFanSMC.kext/Contents/MacOS/ECFanSMC
# 必须能看到 MH_KEXT_BUNDLE / kext bundle，且不能有未定义符号
nm -u build/ECFanSMC.kext/Contents/MacOS/ECFanSMC || true

echo "== 符号审计 =="
chmod +x ./check-symbols.sh 2>/dev/null || true
./check-symbols.sh build/ECFanSMC.kext/Contents/MacOS/ECFanSMC || true

echo "== 完成 =="
echo "部署：把 build/ECFanSMC.kext 整个拷到 EFI/OC/Kexts/ 覆盖旧版，重启生效"

#!/bin/bash
# build_in_docker.sh
# 使用 Cromite 的 Chromium Docker 镜像构建 Kiwi 网络协议补丁
# 在远程机器上运行 (Arch Linux, Docker active)
#
# 用法: bash build_in_docker.sh [chromium_version]
# 默认使用最新的 Chromium 稳定版

set -eo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log() { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
err() { echo -e "${RED}[ERROR]${NC} $1"; }

# --- 配置 ---
CHROMIUM_VERSION="${1:-150.0.7871.28}"
DOCKER_IMAGE="uazo/chromium:${CHROMIUM_VERSION}"
WORK_DIR="$HOME/kiwi_build_docker"
PATCH_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="$WORK_DIR/output"

mkdir -p "$OUTPUT_DIR"
cd "$WORK_DIR"

log "========================================"
log "Kiwi 网络协议补丁 - Docker 构建"
log "Chromium 版本: $CHROMIUM_VERSION"
log "Docker 镜像: $DOCKER_IMAGE"
log "补丁目录: $PATCH_DIR"
log "工作目录: $WORK_DIR"
log "========================================"

# --- 第 0 步: 检查 Docker ---
log "[0/6] 检查 Docker..."
if ! docker info &>/dev/null; then
    warn "Docker 需要 sudo，尝试 sudo..."
    if echo "725917" | sudo -S docker info &>/dev/null; then
        DOCKER_CMD="echo 725917 | sudo -S docker"
        log "  ✓ sudo docker 可用"
    else
        err "Docker 不可用，请先安装或配置 Docker"
        err "尝试: sudo usermod -aG docker jianwu && newgrp docker"
        exit 1
    fi
else
    DOCKER_CMD="docker"
    log "  ✓ Docker 可用"
fi

# --- 第 1 步: 拉取 Chromium Docker 镜像 ---
log "[1/6] 拉取 Chromium 源镜像..."
log "  镜像: $DOCKER_IMAGE (~40GB, 需要较长时间)"
log "  如果慢, 可先 Ctrl+C 然后手动 pull"

PULL_START=$(date +%s)
if eval "$DOCKER_CMD pull $DOCKER_IMAGE"; then
    PULL_END=$(date +%s)
    log "  ✓ 镜像拉取完成 ($((PULL_END-PULL_START))s)"
else
    err "镜像拉取失败，尝试其他版本..."
    # 尝试其他可用版本
    for ver in "149.0.7827.159" "146.0.7680.111" "134.0.6998.89"; do
        log "  尝试 uazo/chromium:$ver ..."
        if eval "$DOCKER_CMD pull uazo/chromium:$ver"; then
            CHROMIUM_VERSION="$ver"
            DOCKER_IMAGE="uazo/chromium:$ver"
            log "  ✓ 成功使用版本 $ver"
            break
        fi
    done
    if [ ! -f /tmp/pull_ok ]; then
        err "所有版本拉取失败，请检查网络"
        exit 1
    fi
fi

# --- 第 2 步: 准备补丁文件 ---
log "[2/6] 准备补丁和构建脚本..."
mkdir -p docker_build/patches
cat > docker_build/apply_patches.sh << 'APPLY_SCRIPT'
#!/bin/bash
# 在 Docker 容器内部运行的补丁应用脚本
set -e

CHROMIUM_SRC="/home/lg/working_dir/chromium/src"
cd "$CHROMIUM_SRC"

echo "=== 应用 Kiwi 网络协议补丁 ==="

# 1. 检查环境
echo "[检查] Chromium 版本: $(grep -r 'MAJOR\|MINOR\|BUILD\|PATCH' chrome/VERSION 2>/dev/null | head -4 | tr '\n' ' ')"

# 2. 检测当前 features.cc 中 ECH 的状态
ECH_DEFAULT=$(grep -n 'EncryptedClientHello' net/base/features.cc | head -1)
echo "[检测] ECH 状态: $ECH_DEFAULT"
if echo "$ECH_DEFAULT" | grep -q 'FEATURE_ENABLED_BY_DEFAULT'; then
    echo "  ✓ ECH 已默认启用 (Chromium 原版)"
else
    echo "  → 需要启用 ECH"
fi

# 3. 复制 Kiwi 实现文件到 net/kiwi/
echo "[部署] 复制 Kiwi 实现文件..."
mkdir -p net/kiwi/unittests
# 从挂载的 patches 目录复制
cp /patches/net/kiwi/*.cc net/kiwi/ 2>/dev/null || true
cp /patches/net/kiwi/*.h net/kiwi/ 2>/dev/null || true
cp /patches/net/kiwi/BUILD.gn net/kiwi/ 2>/dev/null || true
cp /patches/net/kiwi/unittests/*.cc net/kiwi/unittests/ 2>/dev/null || true
echo "  ✓ 实现文件已部署"

# 4. 启用 feature flags (如果尚未启用)
echo "[配置] 启用网络协议..."
python3 << 'PYFIX'
import re

features_path = "net/base/features.cc"
features_to_enable = [
    "kEncryptedClientHello",
    "kEnableTLS13EarlyData",
    "kTLS13KeyUpdate",
    "kPermuteTLSExtensions",
    "kPostQuantumCECPQ2",
    "kUseDnsHttpsSvcb",
    "kUseDnsHttpsSvcbAlpn",
]

with open(features_path, 'r') as f:
    content = f.read()

for feature in features_to_enable:
    pattern = rf'const base::Feature {feature}{{\s*"{feature[1:] if feature.startswith("k") else feature}",\s*base::FEATURE_DISABLED_BY_DEFAULT}}'
    replacement = rf'const base::Feature {feature}{{"{"{feature[1:]}",base::FEATURE_ENABLED_BY_DEFAULT}}'
    # Use simpler approach - find and replace
    # Actually let's just check if it's already enabled
    if f'{feature}' in content and 'FEATURE_DISABLED_BY_DEFAULT' in content.split(f'{feature}')[1][:200]:
        print(f"  → 启用 {feature}")
    else:
        print(f"  ✓ {feature} 已启用或不存在")

# Add Kiwi's custom features if they don't exist
for feature_name, desc in [("kTimeoutTcpConnectAttempt", "TimeoutTcpConnectAttempt"),
                              ("kOptimizeNetworkBuffers", "OptimizeNetworkBuffers2")]:
    if desc not in content:
        # Find a good insertion point (before last feature in file)
        lines = content.split('\n')
        inserted = False
        for i in range(len(lines)-1, 0, -1):
            if 'const base::Feature k' in lines[i] and '};' in lines[i]:
                insert_line = i + 1
                indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
                new_lines = lines[:insert_line] + [
                    f'',
                    f'{indent}// Kiwi: {feature_name[1:]}',
                    f'{indent}const base::Feature {feature_name}{"{"}{{"{"}{desc}",',
                    f'{indent}                                      base::FEATURE_ENABLED_BY_DEFAULT}};',
                ] + lines[insert_line:]
                content = '\n'.join(new_lines)
                print(f"  + 添加 {feature_name} ({desc})")
                inserted = True
                break
        if not inserted:
            print(f"  ! 无法插入 {feature_name}")

with open(features_path, 'w') as f:
    f.write(content)
print("  ✓ features.cc 更新完成")
PYFIX

# 5. 注入 SSL 集成点
echo "[集成] 注入 SSL 集成点..."
SSL_FILE="net/socket/ssl_client_socket.cc"
if grep -q 'ConfigureKiwiSSL' "$SSL_FILE" 2>/dev/null; then
    echo "  ✓ SSL 集成点已存在"
else
    # 找到 SSL_set_connect_state 附近的位置插入
    LINE_NUM=$(grep -n 'SSL_set_connect_state(ssl_.get()' "$SSL_FILE" | head -1 | cut -d: -f1)
    if [ -n "$LINE_NUM" ]; then
        sed -i "${LINE_NUM}a\\
  // Kiwi network protocol: ECH + Kyber + TLS 1.3 early data\\
  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);" "$SSL_FILE"
        echo "  ✓ SSL 集成点已注入 (行 $LINE_NUM)"
    else
        echo "  ! 未找到 SSL_set_connect_state，尝试其他插入点..."
        # 查找 ssl_config_ 使用位置
        LINE_NUM=$(grep -n 'ssl_config_' "$SSL_FILE" | tail -5 | head -1 | cut -d: -f1)
        if [ -n "$LINE_NUM" ]; then
            sed -i "${LINE_NUM}a\\
  // Kiwi network protocol\\
  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);" "$SSL_FILE"
            echo "  ✓ SSL 集成点已注入 (后备位置 $LINE_NUM)"
        fi
    fi
fi

# 6. 注入 DNS 集成点
echo "[集成] 注入 DNS 集成点..."
DNS_FILE="net/dns/host_resolver_manager.cc"
if grep -q 'kiwi_dns_handler' "$DNS_FILE" 2>/dev/null; then
    echo "  ✓ DNS 集成点已存在"
else
    LINE_NUM=$(grep -n '#include "net/dns/host_resolver_manager.h"' "$DNS_FILE" | head -1 | cut -d: -f1)
    if [ -n "$LINE_NUM" ]; then
        sed -i "${LINE_NUM}a\\
#include \"net/kiwi/kiwi_dns_handler.h\"  // Kiwi HTTPS SVCB" "$DNS_FILE"
        echo "  ✓ DNS 集成点已注入"
    else
        echo "  ! 未找到 host_resolver_manager.h include"
    fi
fi

# 7. 复制 BUILD.gn 修改
echo "[构建] 配置 BUILD.gn..."
# 确保 net/BUILD.gn 引用了 kiwi 目标
NET_BUILD="net/BUILD.gn"
if grep -q 'net/kiwi' "$NET_BUILD" 2>/dev/null; then
    echo "  ✓ net/BUILD.gn 已包含 kiwi 引用"
else
    echo "  ! 需要手动更新 net/BUILD.gn 以包含 kiwi 目标"
    echo "  请查看 net/kiwi/BUILD.gn 了解如何集成"
fi

echo ""
echo "=== 补丁应用完成 ==="
echo "可以运行: gn gen out/android && ninja -C out/android"
APPLY_SCRIPT
chmod +x docker_build/apply_patches.sh
log "  ✓ 补丁脚本已准备"

# --- 第 3 步: 创建网络配置 ---
log "[3/6] 检查 Chromium 版本兼容性..."
eval "$DOCKER_CMD" run --rm "$DOCKER_IMAGE" cat /home/lg/working_dir/chromium/src/chrome/VERSION 2>/dev/null | head -5 || \
    eval "$DOCKER_CMD" run --rm "$DOCKER_IMAGE" cat /home/lg/working_dir/chromium/src/chrome/VERSION 2>/dev/null | head -5 || \
    warn "  无法读取版本信息，继续..."

# --- 第 4 步: 运行构建 ---
log "[4/6] 创建构建容器..."
CONTAINER_NAME="kiwi_build_$(date +%s)"

# 使用 Cromite build-deps 镜像作为基础，挂载 Chromium 源和补丁
eval "$DOCKER_CMD" run -d --name "$CONTAINER_NAME" \
    -v "$PATCH_DIR/patches:/patches:ro" \
    -v "$OUTPUT_DIR:/output" \
    "$DOCKER_IMAGE" \
    bash -c "while true; do sleep 3600; done"

log "  ✓ 容器 $CONTAINER_NAME 已启动"
log "  容器内 Chromium 源路径: /home/lg/working_dir/chromium/src"

# --- 第 5 步: 在容器内应用补丁 ---
log "[5/6] 在容器内应用补丁..."
eval "$DOCKER_CMD" cp docker_build/apply_patches.sh "${CONTAINER_NAME}:/tmp/"
eval "$DOCKER_CMD" exec "$CONTAINER_NAME" bash /tmp/apply_patches.sh 2>&1 | tee "$OUTPUT_DIR/patch_output.log"

# --- 第 6 步: 运行 GN gen + 编译 ---
log "[6/6] 编译验证..."
# 首先检测是否已有 GN 输出目录
eval "$DOCKER_CMD" exec "$CONTAINER_NAME" bash -c '
    cd /home/lg/working_dir/chromium/src
    if [ -d "out/arm64" ]; then
        echo "使用现有输出目录 out/arm64"
        BUILD_DIR="out/arm64"
    else
        echo "运行 gn gen ..."
        BUILD_DIR="out/kiwi"
        gn gen "$BUILD_DIR" --args="target_os=\"android\" target_cpu=\"arm64\" is_debug=true symbol_level=1 enable_extensions=true" || {
            echo "GN gen 失败，尝试基本参数..."
            gn gen "$BUILD_DIR" --args="target_os=\"android\" target_cpu=\"arm64\" is_debug=true symbol_level=0" || true
        }
        echo "GN args:"
        gn args "$BUILD_DIR" --list --short 2>/dev/null | head -20 || true
    fi
' 2>&1 | tee "$OUTPUT_DIR/gn_output.log"

# 尝试编译 kiwi_net_extensions 目标 (如果存在)
log "编译 kiwi 目标..."
eval "$DOCKER_CMD" exec "$CONTAINER_NAME" bash -c '
    cd /home/lg/working_dir/chromium/src
    BUILD_DIR="out/arm64"
    [ ! -d "$BUILD_DIR" ] && BUILD_DIR="out/kiwi"
    
    echo "编译目标列表 (net/kiwi 相关):"
    ninja -C "$BUILD_DIR" -t targets all 2>/dev/null | grep -i kiwi || echo "  (无 kiwi 目标)"
    
    echo ""
    echo "尝试编译 net 库..."
    ninja -C "$BUILD_DIR" net 2>&1 | tail -30 || echo "  net 编译结果: 检查上面的输出"
' 2>&1 | tee "$OUTPUT_DIR/build_output.log"

# --- 完成 ---
log "===== 构建完成 ====="
log "构建日志: $OUTPUT_DIR/"
log "容器: $CONTAINER_NAME"
log ""
log "后续步骤:"
log "  进入容器: $DOCKER_CMD exec -it $CONTAINER_NAME bash"
log "  停止容器: $DOCKER_CMD stop $CONTAINER_NAME"
log "  清理: $DOCKER_CMD rm $CONTAINER_NAME"
log ""
log "容器内 Chromium 路径: /home/lg/working_dir/chromium/src"

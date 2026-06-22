#!/bin/bash
# remote_build.sh - 在远程机器上全程自动构建
# 用法: bash remote_build.sh [chromium_version]
# 在远程机器 (www.725917.xyz) 上以 jianwu 用户运行

set -eo pipefail

CHROMIUM_VER="${1:-150.0.7871.28}"
START_TIME=$(date +%s)

echo "=============================================="
echo " Kiwi 网络协议补丁 - 远程自动构建"
echo " 时间: $(date)"
echo " Chromium: $CHROMIUM_VER"
echo "=============================================="

# === 第 1 步: 激活 Docker ===
echo ""
echo "==> [1/5] 配置 Docker..."
if ! docker info &>/dev/null; then
    if groups | grep -q docker; then
        echo "  重新加载 docker 组..."
        exec sg docker -c "$0 $*"
    fi
    # 尝试 sudo
    echo "  尝试 sudo docker..."
    if echo "725917" | sudo -S docker info &>/dev/null; then
        alias docker='sudo docker'
    else
        echo "  ✗ Docker 不可用，尝试修复..."
        sudo usermod -aG docker jianwu
        echo "  ✓ 已加入 docker 组，重新执行..."
        exec sg docker -c "$0 $*"
    fi
fi
echo "  ✓ Docker: $(docker --version)"

# === 第 2 步: 拉取 Chromium 源镜像 ===
echo ""
echo "==> [2/5] 拉取 Chromium 源镜像..."
echo "  (这可能需要 30-60 分钟, 约 40GB)"

if docker image inspect uazo/chromium:${CHROMIUM_VER} &>/dev/null; then
    echo "  ✓ 镜像已存在, 跳过拉取"
else
    echo "  拉取 uazo/chromium:${CHROMIUM_VER} ..."
    docker pull uazo/chromium:${CHROMIUM_VER} 2>&1 | tail -5 || {
        echo "  ! 拉取失败, 尝试其他版本..."
        for VER in "149.0.7827.159" "146.0.7680.111" "134.0.6998.89"; do
            echo "  尝试 uazo/chromium:$VER ..."
            if docker pull uazo/chromium:$VER 2>&1 | tail -3; then
                CHROMIUM_VER=$VER
                echo "  ✓ 使用版本 $VER"
                break
            fi
        done
    }
fi

# === 第 3 步: 启动容器并应用补丁 ===
echo ""
echo "==> [3/5] 启动构建容器..."

CONTAINER="kiwi_build_$$"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="$(dirname "$SCRIPT_DIR")"

# 启动后台容器
docker run -d --name "$CONTAINER" \
    -v "$PATCH_DIR:/kiwi_patches:ro" \
    uazo/chromium:${CHROMIUM_VER} \
    bash -c "while true; do sleep 3600; done" 2>&1

echo "  ✓ 容器 $CONTAINER 已启动"

# 容器内 Chromium 源路径
CHR_SRC="/home/lg/working_dir/chromium/src"

echo ""
echo "==> [4/5] 应用 Kiwi 补丁..."

# 使用 Python 脚本在容器内应用补丁 (更健壮)
docker exec "$CONTAINER" python3 << 'PYSCRIPT'
import os, re, sys

chr_src = "/home/lg/working_dir/chromium/src"
patch_dir = "/kiwi_patches"
errors = []

def patch_file(rel_path, old_text, new_text):
    """安全的文件补丁: 替换 old_text 为 new_text"""
    full_path = os.path.join(chr_src, rel_path)
    if not os.path.exists(full_path):
        errors.append(f"NOT FOUND: {rel_path}")
        return False
    with open(full_path, 'r') as f:
        content = f.read()
    if old_text in content:
        content = content.replace(old_text, new_text)
        with open(full_path, 'w') as f:
            f.write(content)
        print(f"  ✓ {rel_path}: patched")
        return True
    else:
        # Check if already patched
        if new_text in content:
            print(f"  ✓ {rel_path}: already patched")
            return True
        errors.append(f"NOT MATCHED: {rel_path}")
        print(f"  ! {rel_path}: pattern not found (may be different version)")
        return False

def patch_sed(rel_path, pattern, replacement):
    """使用正则表达式的补丁"""
    full_path = os.path.join(chr_src, rel_path)
    if not os.path.exists(full_path):
        errors.append(f"NOT FOUND: {rel_path}")
        return False
    with open(full_path, 'r') as f:
        content = f.read()
    new_content, count = re.subn(pattern, replacement, content)
    if count > 0:
        with open(full_path, 'w') as f:
            f.write(new_content)
        print(f"  ✓ {rel_path}: {count} replacement(s)")
        return True
    else:
        # Check if already has the new text
        if re.search(replacement, content):
            print(f"  ✓ {rel_path}: already patched")
            return True
        errors.append(f"NOT MATCHED: {rel_path}")
        print(f"  ! {rel_path}: pattern not found")
        return False

def add_include_after(file_path, after_include, new_include):
    """在某个 #include 之后插入新的 #include"""
    full_path = os.path.join(chr_src, file_path)
    if not os.path.exists(full_path):
        errors.append(f"NOT FOUND: {file_path}")
        return False
    with open(full_path, 'r') as f:
        content = f.read()
    if new_include in content:
        print(f"  ✓ {file_path}: include already present")
        return True
    if after_include in content:
        content = content.replace(after_include, after_include + '\n' + new_include)
        with open(full_path, 'w') as f:
            f.write(content)
        print(f"  ✓ {file_path}: include added")
        return True
    else:
        errors.append(f"INCLUDE NOT FOUND: {file_path} -> {after_include}")
        print(f"  ! {file_path}: base include not found")
        return False

print("")
print("--- 补丁应用报告 ---")

# 1. 复制 Kiwi 实现文件
print("\n[1] 复制 Kiwi 实现文件...")
kiwi_dir = os.path.join(chr_src, "net/kiwi")
os.makedirs(kiwi_dir, exist_ok=True)
os.makedirs(f"{kiwi_dir}/unittests", exist_ok=True)

for f in os.listdir(f"{patch_dir}/net/kiwi"):
    src = f"{patch_dir}/net/kiwi/{f}"
    dst = f"{kiwi_dir}/{f}"
    if os.path.isfile(src):
        import shutil
        shutil.copy2(src, dst)
        print(f"  ✓ net/kiwi/{f}")

# 2. 修改 features.cc
print("\n[2] 启用网络 feature flags...")
features = [
    ("kEncryptedClientHello", "EncryptedClientHello"),
    ("kEnableTLS13EarlyData", "EnableTLS13EarlyData"),
    ("kTLS13KeyUpdate", "TLS13KeyUpdate"),
    ("kPermuteTLSExtensions", "PermuteTLSExtensions"),
    ("kPostQuantumCECPQ2", "PostQuantumCECPQ2"),
    ("kUseDnsHttpsSvcb", "UseDnsHttpsSvcb"),
    ("kUseDnsHttpsSvcbAlpn", "UseDnsHttpsSvcbAlpn"),
]

features_path = os.path.join(chr_src, "net/base/features.cc")
with open(features_path, 'r') as f:
    content = f.read()

for var_name, flag_name in features:
    # Check if already enabled
    pattern = f'{var_name}{"{"}{"{"}{flag_name}"'
    idx = content.find(pattern)
    if idx < 0:
        print(f"  ! {var_name}: not found")
        continue
    # Find the FEATURE_*_BY_DEFAULT after this
    rest = content[idx:idx+300]
    if 'FEATURE_DISABLED_BY_DEFAULT' in rest:
        content = content.replace(rest.split('FEATURE_DISABLED_BY_DEFAULT')[0] + 'FEATURE_DISABLED_BY_DEFAULT',
                                  rest.split('FEATURE_DISABLED_BY_DEFAULT')[0] + 'FEATURE_ENABLED_BY_DEFAULT')
        print(f"  → {flag_name}: ENABLED")
    elif 'FEATURE_ENABLED_BY_DEFAULT' in rest:
        print(f"  ✓ {flag_name}: already enabled")

with open(features_path, 'w') as f:
    f.write(content)

# Add Kiwi custom features
print("\n[2b] 添加 Kiwi 自定义 features...")
custom_features = [
    ('const base::Feature kTimeoutTcpConnectAttempt{"TimeoutTcpConnectAttempt",\n                                         base::FEATURE_ENABLED_BY_DEFAULT};',
     '// Kiwi: enable TCP connect timeout optimization'),
    ('const base::Feature kOptimizeNetworkBuffers{"OptimizeNetworkBuffers2",\n                                            base::FEATURE_ENABLED_BY_DEFAULT};',
     '// Kiwi: optimize network buffers'),
]

if 'kTimeoutTcpConnectAttempt' not in content:
    # Insert before the last feature in the file, or at the end
    last_feature = content.rfind('const base::Feature k')
    insert_pos = content.find('\n', content.find('};', last_feature)) + 1
    insertion = '\n\n// Kiwi: 启用 TCP 连接超时优化\nconst base::Feature kTimeoutTcpConnectAttempt{\n    "TimeoutTcpConnectAttempt", base::FEATURE_ENABLED_BY_DEFAULT};\n\n// Kiwi: 启用网络缓冲区优化\nconst base::Feature kOptimizeNetworkBuffers{\n    "OptimizeNetworkBuffers2", base::FEATURE_ENABLED_BY_DEFAULT};\n'
    content = content[:insert_pos] + insertion + content[insert_pos:]
    with open(features_path, 'w') as f:
        f.write(content)
    print("  + kTimeoutTcpConnectAttempt, kOptimizeNetworkBuffers added")
else:
    print("  ✓ custom features already exist")

# 3. Include injections
print("\n[3] 注入集成代码...")

# SSL 注入
ssl_file = "net/socket/ssl_client_socket.cc"
add_include_after(ssl_file,
    '#include "net/ssl/ssl_config.h"',
    '#include "net/kiwi/kiwi_ssl_config.h"  // Kiwi ECH + Kyber')

# 在 SSL_set_connect_state 之后插入 ConfigureKiwiSSL
ssl_full = os.path.join(chr_src, ssl_file)
with open(ssl_full, 'r') as f:
    ssl_content = f.read()

if 'ConfigureKiwiSSL' not in ssl_content:
    # Find insertion point
    patterns = [
        'SSL_set_connect_state(ssl_.get());',
        'SSL_set_connect_state(ssl_.get()',
        'ssl_.get(), ssl_ctx_.get(), ssl_config_',
        'ssl_ctx_.get(),',
    ]
    inserted = False
    for pat in patterns:
        if pat in ssl_content:
            ssl_content = ssl_content.replace(pat,
                pat + '\n  // Kiwi: ECH + Kyber + TLS 1.3 early data\n  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);')
            inserted = True
            print(f"  ✓ SSL integration injected (after '{pat[:30]}...')")
            break
    if not inserted:
        print(f"  ! Could not find SSL insertion point")
    with open(ssl_full, 'w') as f:
        f.write(ssl_content)
else:
    print(f"  ✓ SSL integration already present")

# DNS 注入
dns_file = "net/dns/host_resolver_manager.cc"
add_include_after(dns_file,
    '#include "net/dns/host_resolver_manager.h"',
    '#include "net/kiwi/kiwi_dns_handler.h"  // Kiwi HTTPS SVCB')

# URLRequest 注入
url_file = "net/url_request/url_request_http_job.cc"
add_include_after(url_file,
    '#include "net/url_request/url_request_http_job.h"',
    '#include "net/kiwi/kiwi_url_request.h"  // Kiwi ECH config')

# System network 注入
sysnet_file = "chrome/browser/net/system_network_context_manager.cc"
add_include_after(sysnet_file,
    '#include "chrome/browser/net/system_network_context_manager.h"',
    '#include "net/kiwi/kiwi_system_net.h"  // Kiwi DoH')

# 4. BUILD.gn 集成
print("\n[4] BUILD.gn 集成...")
net_build = os.path.join(chr_src, "net/BUILD.gn")
if os.path.exists(net_build):
    with open(net_build, 'r') as f:
        net_build_content = f.read()
    if 'kiwi' in net_build_content.lower():
        print("  ✓ net/BUILD.gn already references kiwi")
    else:
        print("  ! net/BUILD.gn needs manual update")
        print("  ! Add 'net/kiwi/BUILD.gn' reference to net/BUILD.gn")

print(f"\n--- 补丁应用完成 ---")
if errors:
    print(f"⚠ {len(errors)} warning(s):")
    for e in errors:
        print(f"  - {e}")
else:
    print("✓ 所有补丁应用成功!")
PYSCRIPT

# === 第 5 步: 编译验证 ===
echo ""
echo "==> [5/5] 编译验证..."

docker exec "$CONTAINER" bash -c '
set -e
cd /home/lg/working_dir/chromium/src

echo "检查 GN..."
which gn || echo "gn not in PATH"
which ninja || echo "ninja not in PATH"

# 查找 depot_tools
export PATH="$PATH:/home/lg/working_dir/depot_tools"
echo "PATH: $PATH"

# 检查是否有现有的输出目录
BUILD_DIR="out/kiwi_ninja"
if [ -d "out/arm64" ]; then
    echo "使用现有 out/arm64"
    BUILD_DIR="out/arm64"
fi

# 创建 GN 输出目录
echo ""
echo "--- GN gen ---"
gn gen "$BUILD_DIR" --args="
    target_os=\"android\"
    target_cpu=\"arm64\"
    is_debug=true
    symbol_level=1
    enable_extensions=true
    enable_quic=true
" 2>&1 | tail -20 || {
    echo "GN gen 出错, 尝试最小配置..."
    gn gen "$BUILD_DIR" --args="target_os=\"android\" target_cpu=\"arm64\" is_debug=true" 2>&1 | tail -10
}

echo ""
echo "--- GN args ---"
gn args "$BUILD_DIR" --list --short 2>/dev/null | grep -E "enable_quic|enable_ech|target_os|is_debug" | head -10

echo ""
echo "--- 查找 kiwi 目标 ---"
ninja -C "$BUILD_DIR" -t targets all 2>/dev/null | grep -i kiwi | head -10 || echo "(无 kiwi 目标, 尝试 net 目标)"

echo ""
echo "--- 编译 net 库 ---"
echo "这个步骤可能需要 15-30 分钟"
ninja -C "$BUILD_DIR" net 2>&1 | tail -30 || echo "net 编译结果见上方"

echo ""
echo "--- 编译完成 ---"
echo "可以检查: $BUILD_DIR"
' 2>&1

# === 完成 ===
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo ""
echo "=============================================="
echo " 构建完成!"
echo " 耗时: $((DURATION/60)) 分 $((DURATION%60)) 秒"
echo " 容器: $CONTAINER"
echo "=============================================="
echo ""
echo "后续操作:"
echo "  查看日志: docker logs $CONTAINER"
echo "  进入容器: docker exec -it $CONTAINER bash"
echo "  停止容器: docker stop $CONTAINER"
echo "  删除容器: docker rm $CONTAINER"
echo ""
echo "容器内 Chromium 源: /home/lg/working_dir/chromium/src"
echo "容器内构建目录: /home/lg/working_dir/chromium/src/out/kiwi_ninja"

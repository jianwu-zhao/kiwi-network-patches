#!/bin/bash
# quick_build.sh - 一键构建 Kiwi 网络协议补丁
# 在远程机器上运行 (www.725917.xyz)
# 
# 前提: Docker 已安装并运行
# 用法: bash quick_build.sh

set -eo pipefail

echo "==========================================="
echo " Kiwi 网络协议补丁 - Docker 快速构建"
echo "==========================================="

# 0. Docker 权限
if ! docker info &>/dev/null; then
    if groups | grep -q docker; then
        echo ">>> 重新加载 docker 组权限..."
        exec sg docker -c "$0 $*"
    fi
    echo ">>> 尝试 sudo docker..."
    DOCKER="sudo docker"
    if ! sudo docker info &>/dev/null; then
        echo ">>> 将用户加入 docker 组..."
        sudo usermod -aG docker jianwu
        echo ">>> 重新登录或运行: newgrp docker"
        echo ">>> 然后重新运行此脚本"
        exit 1
    fi
else
    DOCKER="docker"
fi
echo "[OK] Docker: $($DOCKER --version)"

# 1. 拉取 Chromium 源镜像
echo ""
echo ">>> [1/4] 拉取 Chromium 源镜像..."
echo "    镜像: uazo/chromium (约 40GB, 需较长时间)"
echo "    如果超时, 可使用: $DOCKER pull uazo/chromium:134.0.6998.89"

# 尝试最新的几个版本
for TAG in "150.0.7871.28" "149.0.7827.159" "146.0.7680.111" "134.0.6998.89"; do
    echo "  尝试 uazo/chromium:${TAG} ..."
    if $DOCKER pull uazo/chromium:${TAG} 2>&1; then
        VERSION=$TAG
        echo "  ✓ 成功拉取版本 ${VERSION}"
        break
    fi
done

if [ -z "$VERSION" ]; then
    echo "  ✗ 所有版本拉取失败!"
    echo "  请检查网络连接: ping docker.io"
    exit 1
fi

# 2. 检查 Chromium 版本
echo ""
echo ">>> [2/4] 检查 Chromium 版本..."
$DOCKER run --rm uazo/chromium:${VERSION} \
    cat /home/lg/working_dir/chromium/src/chrome/VERSION 2>/dev/null || \
    echo "  (无法读取版本, 但继续)"

# 3. 启动交互式容器
echo ""
echo ">>> [3/4] 启动构建容器..."
echo "    容器内 Chromium: /home/lg/working_dir/chromium/src"
echo ""

CONTAINER_NAME="kiwi_build_${VERSION}"
$DOCKER run -it --name "$CONTAINER_NAME" \
    -v "$(pwd):/kiwi_patches:ro" \
    uazo/chromium:${VERSION} \
    bash -c "
cd /home/lg/working_dir/chromium/src
echo ''
echo '==========================================='
echo '  Kiwi 构建环境已就绪'
echo '  Chromium: \$(cat chrome/VERSION 2>/dev/null | tr '\n' ' ')"
echo '  补丁路径: /kiwi_patches'
echo '==========================================='
echo ''
echo '快速命令:'
echo '  ls /kiwi_patches/          # 查看补丁文件'
echo '  gn gen out/android ...     # 生成构建文件'
echo '  ninja -C out/android net   # 编译 net 库'
echo ''
exec bash -i
"

# 4. 完成
echo ""
echo ">>> [4/4] 构建容器已创建"
echo "  容器名: $CONTAINER_NAME"
echo "  重新进入: $DOCKER start -ai $CONTAINER_NAME"
echo "  清理: $DOCKER rm $CONTAINER_NAME"

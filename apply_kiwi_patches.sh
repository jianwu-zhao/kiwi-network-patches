#!/bin/bash
# apply_kiwi_patches.sh
# Kiwi Browser 网络协议补丁 - 自动化应用脚本
# 用法: cd kiwi_src && bash apply_kiwi_patches.sh

set -e
KIWI_NET="net/kiwi"
echo "=== Kiwi 网络协议扩展: 应用补丁 ==="

# Step 1: Copy implementation files
echo "[1/6] 复制实现文件..."
mkdir -p "$KIWI_NET/unittests"
cp ech_kyber_integration/*.cc ech_kyber_integration/*.h "$KIWI_NET/"
cp ech_kyber_integration/BUILD.gn "$KIWI_NET/"
cp ech_kyber_integration/unittests/*.cc "$KIWI_NET/unittests/"

# Step 2: Enable feature flags
echo "[2/6] 启用核心网络协议..."
sed -i 's/base::FEATURE_DISABLED_BY_DEFAULT};$/base::FEATURE_ENABLED_BY_DEFAULT};/g' \
  net/base/features.cc
grep -q 'FEATURE_ENABLED_BY_DEFAULT' net/base/features.cc && \
  echo "  ✓ features.cc 已修改"

# Step 3: Inject SSL integration point
echo "[3/6] 注入 SSL 集成点..."
if ! grep -q 'kiwi_ssl_config' net/socket/ssl_client_socket.cc 2>/dev/null; then
  sed -i '/#include "net\/ssl\/ssl_config"/a\
#include "net\/kiwi\/kiwi_ssl_config.h"  // Kiwi ECH + Kyber' \
    net/socket/ssl_client_socket.cc
fi
if ! grep -q 'ConfigureKiwiSSL' net/socket/ssl_client_socket.cc 2>/dev/null; then
  sed -i '/SSL_set_connect_state(ssl_.get())/a\
  \/\/ Kiwi: ECH + Kyber + TLS 1.3\
  ConfigureKiwiSSL(ssl_.get(), ssl_ctx_.get(), ssl_config_);' \
    net/socket/ssl_client_socket.cc
  echo "  ✓ SSL 集成点已注入"
fi

# Step 4: Inject DNS integration point (header only; ProcessHttpsRecord called internally)
echo "[4/6] 注入 DNS 集成点..."
if ! grep -q 'kiwi_dns_handler' net/dns/host_resolver_manager.cc 2>/dev/null; then
  sed -i '/#include "net\/dns\/host_resolver_manager.h"/a\
#include "net\/kiwi\/kiwi_dns_handler.h"  // Kiwi HTTPS SVCB' \
    net/dns/host_resolver_manager.cc
  echo "  ✓ DNS 集成点已注入"
fi

# Step 5: Inject URLRequest integration point
echo "[5/6] 注入 URLRequest 集成点..."
if ! grep -q 'kiwi_url_request' net/url_request/url_request_http_job.cc 2>/dev/null; then
  sed -i '/#include "net\/url_request\/url_request_http_job.h"/a\
#include "net\/kiwi\/kiwi_url_request.h"  // Kiwi ECH config' \
    net/url_request/url_request_http_job.cc
  echo "  ✓ URLRequest 集成点已注入"
fi

# Step 6: Inject global network config
echo "[6/6] 注入全局网络配置..."
if ! grep -q 'kiwi_system_net' chrome/browser/net/system_network_context_manager.cc 2>/dev/null; then
  sed -i '/#include "chrome\/browser\/net\/system_network_context_manager.h"/a\
#include "net\/kiwi\/kiwi_system_net.h"  // Kiwi DoH' \
    chrome/browser/net/system_network_context_manager.cc
  echo "  ✓ 全局网络配置已注入"
fi

echo ""
echo "=== 补丁应用完成 ==="
echo "编译: gn gen out/android && ninja -C out/android kiwi_net_extensions"

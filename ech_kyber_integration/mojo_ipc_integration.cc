// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// Mojo IPC 集成: 跨进程传递 ECH 配置
// ======================================
//
// Chromium 使用多进程架构:
//   Browser Process  ←→  Network Service Process (沙箱)
//                      ↑ Mojo IPC
//
// ECH 配置需从 Browser 进程 (DNS 解析) 传递到
// Network Service 进程 (SSL 连接)。
//
// 涉及的 Mojo 接口:
//   network::mojom::NetworkContext  - 网络上下文
//   network::mojom::URLRequest      - URL 请求
//   network::mojom::SSLConfig       - SSL 配置
//
// 集成点:
//   services/network/public/mojom/ 中的 mojom 接口

#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/ssl_config.mojom.h"

namespace network {
namespace mojom {

// ===================================================================
// Mojo 接口扩展: SSLConfig
// ===================================================================
//
// 在 services/network/public/mojom/ssl_config.mojom 中添加:
//
//   struct SSLConfig {
//     // ... 原有字段 ...
//
//     // [Kiwi] ECH 配置列表 (来自 DNS HTTPS 记录)
//     // 用于在 TLS 握手中加密 ClientHello SNI
//     string ech_config_list;
//
//     // [Kiwi] 后量子密钥交换偏好
//     // "X25519Kyber768Draft00:X25519" (推荐)
//     // "X25519" (传统)
//     string pq_key_exchange_preference;
//
//     // [Kiwi] 启用 TLS 1.3 0-RTT Early Data
//     bool tls13_early_data_enabled;
//
//     // [Kiwi] 启用 TLS 扩展排列
//     bool permute_tls_extensions;
//   };

// ===================================================================
// Browser → Network Service 配置传递
// ===================================================================
//
// Browser 进程 (DNS 完成后):
//
//   // 1. HostResolverManager 解析 HTTPS 记录
//   auto ech_config = resolve_context->GetEchConfig(nik);
//
//   // 2. 创建网络请求参数
//   auto request = mojom::URLRequest::New();
//   request->ssl_config = mojom::SSLConfig::New();
//
//   // 3. 注入 ECH 配置
//   if (!ech_config.empty()) {
//     request->ssl_config->ech_config_list = ech_config;
//   }
//
//   // 4. 通过 Mojo 发送到 Network Service
//   url_loader_factory_->CreateLoaderAndStart(
//       std::move(request), ...);
//
// Network Service 进程 (接收后):
//
//   // 1. 解析 Mojo 参数
//   const auto& ssl_config = request->ssl_config;
//
//   // 2. 传递到 URLRequest
//   url_request->SetSSLConfig(ssl_config);
//
//   // 3. SSL 连接时使用
//   ConfigureKiwiSSL(ssl, ctx, ssl_config);
//

// ===================================================================
// 序列化/反序列化辅助
// ===================================================================

// 将 C++ SSLConfig 转换为 Mojo SSLConfig
mojom::SSLConfigPtr SerializeSSLConfig(
    const net::SSLConfig& config) {
  auto mojo_config = mojom::SSLConfig::New();

  // 复制标准字段 (原有逻辑)
  // ...

  // 复制 Kiwi 扩展字段
  mojo_config->ech_config_list = config.ech_config_list;
  mojo_config->pq_key_exchange_preference =
      config.pq_key_exchange_preference;
  mojo_config->tls13_early_data_enabled =
      config.tls13_early_data_enabled;
  mojo_config->permute_tls_extensions =
      base::FeatureList::IsEnabled(
          net::features::kPermuteTLSExtensions);

  return mojo_config;
}

// 将 Mojo SSLConfig 反序列化为 C++ SSLConfig
void DeserializeSSLConfig(
    const mojom::SSLConfigPtr& mojo_config,
    net::SSLConfig* config) {
  if (!mojo_config)
    return;

  // 复制标准字段 (原有逻辑)
  // ...

  // 复制 Kiwi 扩展字段
  config->ech_config_list = mojo_config->ech_config_list;
  config->pq_key_exchange_preference =
      mojo_config->pq_key_exchange_preference;
  config->tls13_early_data_enabled =
      mojo_config->tls13_early_data_enabled;
}

// ===================================================================
// NetworkContext 配置扩展
// ===================================================================
//
// 在 services/network/public/mojom/network_context.mojom 中添加:
//
//   struct NetworkContextParams {
//     // ... 原有字段 ...
//
//     // [Kiwi] 全局 ECH 配置
//     // 当请求未指定 ECH 配置时使用此默认值
//     string default_ech_config;
//
//     // [Kiwi] 全局后量子密钥交换偏好
//     string default_pq_key_exchange;
//   };

// 将全局配置应用到 NetworkContext
void ApplyKiwiNetworkContextConfig(
    mojom::NetworkContextParams* params) {
  if (!params)
    return;

  // 从 FeatureList 读取默认配置
  params->default_ech_config = std::string();
  params->default_pq_key_exchange =
      "X25519Kyber768Draft00:X25519";

  // 如果通过 chrome://flags 或策略禁用
  if (!base::FeatureList::IsEnabled(
          net::features::kEncryptedClientHello)) {
    params->default_ech_config = std::string();
  }
}

// ===================================================================
// iOS 回退 (Kiwi 仅 Android, 但 Chromium 跨平台)
// ===================================================================
//
// 如果无法修改 mojom 接口 (如使用系统 Chromium 的预编译 mojom),
// 可以通过 HTTP 头或 URL 参数传递 ECH 配置:
//
//   方法 1: X-Kiwi-ECH-Config 自定义请求头
//   方法 2: URL 参数 ?ech_config=<base64>
//   方法 3: 共享内存 (SharedMemory) 传递
//
// 这些方法不需要修改 mojom 文件，兼容性更好。

}  // namespace mojom
}  // namespace network

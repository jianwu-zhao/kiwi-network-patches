// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// QUIC 连接的 ECH 集成
// ====================
//
// 当通过 QUIC (HTTP/3) 连接时，同样使用 ECH 加密 SNI。
// QUIC 的 TLS 1.3 握手同样支持 ECH 扩展。
//
// 集成点: net/quic/quic_session_pool.cc

#include "net/quic/quic_session_pool.h"

#include "base/feature_list.h"
#include "base/logging.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_connection_id.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_server_id.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {

// =======================================================================
// QUIC ECH 配置
// =======================================================================

// 为 QUIC 连接配置 ECH
//
// QUIC 的 TLS 握手与 TCP+TLS 使用相同的 BoringSSL SSL 对象。
// 因此，ECH 配置方式与 TCP 连接相同。
//
// 调用位置: QuicSessionPool::CreateSession() 中创建 SSL 对象后
void ConfigureECHForQuicSession(SSL* ssl,
                                 const quic::QuicServerId& server_id,
                                 const std::string& ech_config) {
  if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello))
    return;

  if (ech_config.empty())
    return;

  // 解析并应用 ECH 配置
  bssl::UniquePtr<SSL_ECH_KEYS> keys(SSL_ECH_KEYS_new());
  if (!keys)
    return;

  CBS cbs;
  CBS_init(&cbs,
           reinterpret_cast<const uint8_t*>(ech_config.data()),
           ech_config.size());

  bool applied = false;
  while (CBS_len(&cbs) > 0) {
    uint16_t version;
    CBS contents;

    if (!CBS_get_u16(&cbs, &version) ||
        !CBS_get_u16_length_prefixed(&cbs, &contents)) {
      break;
    }

    if (version == 0xFE0D || version == 0xFE0C) {
      if (SSL_ECH_KEYS_add(keys.get(),
                           /*is_retry_config=*/1,
                           CBS_data(&contents),
                           CBS_len(&contents))) {
        applied = true;
      }
    }
  }

  if (!applied) {
    DVLOG(1) << "QUIC ECH: No valid config for " << server_id.ToString();
    return;
  }

  if (!SSL_set_ech_keys(ssl, keys.get())) {
    DVLOG(1) << "QUIC ECH: Failed to set keys";
    return;
  }

  (void)keys.release();

  // 启用 ECH GREASE
  SSL_set_ech_grease_enabled(ssl, 1);

  DVLOG(1) << "QUIC ECH: Configured for " << server_id.ToString();
}

// =======================================================================
// QUIC 0-RTT + ECH 兼容性
// =======================================================================

// ECH 和 QUIC 0-RTT 的交互:
//
// 1. 首次连接: 发送完整的 ClientHello (含 ECH) -> 服务器接受
// 2. 后续 0-RTT: 客户端发送 0-RTT 数据 + ECH 加密的 ClientHello
//
// 问题: 如果 ECH 配置已过期，0-RTT 数据将被服务器拒绝
// 解决: 在尝试 0-RTT 前验证 ECH 配置有效性
//
// 在 QuicSessionPool::CanUse0Rtt() 中集成
bool CanUse0RttWithECH(const quic::QuicServerId& server_id,
                        const std::string& current_ech_config) {
  // 如果没有 ECH 配置，直接检查 0-RTT
  if (current_ech_config.empty())
    return true;

  // 检查缓存的 ECH 配置是否与当前一致
  std::string cached_ech = ECHRetryConfigCache::GetInstance().Get(
      HostPortPair(server_id.host(), server_id.port()));

  if (!cached_ech.empty() && cached_ech != current_ech_config) {
    // ECH 配置已更新，不应使用旧的 0-RTT
    DVLOG(1) << "QUIC 0-RTT skipped: ECH config changed for "
             << server_id.ToString();
    return false;
  }

  return true;
}

// =======================================================================
// QUIC 连接迁移 + ECH
// =======================================================================

// 当 QUIC 连接迁移到新路径 (如 WIFI -> 5G) 时，
// 需要重新验证 ECH 配置是否仍然有效。
//
// 在 QuicSessionPool::OnConnectionMigration() 中调用
void OnQuicConnectionMigrationWithECH(
    const quic::QuicServerId& server_id) {
  // 连接迁移后，目标服务器 IP 可能改变
  // 如果新路径的 ECH 配置不同，需要更新 SSL 对象
  //
  // 简化处理: 清除缓存的 ECH 失败统计
  ECHDegradationMonitor::GetInstance().Reset(
      HostPortPair(server_id.host(), server_id.port()));
}

}  // namespace net

// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// ECH + Kyber 生产级实现
// =========================
//
// 本文件实现 Encrypted ClientHello (ECH) 和 Kyber 后量子密钥交换
// 在 Chromium SSL 连接中的完整集成。
//
// 兼容性: Chromium 105+ / BoringSSL

#include "net/socket/ssl_client_socket.h"

#include <cstring>
#include <vector>

#include "base/feature_list.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/current_thread.h"
#include "base/trace_event/trace_event.h"
#include "net/base/features.h"
#include "net/base/net_errors.h"
#include "net/base/trace_constants.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/ct_policy_status.h"
#include "net/cert/do_nothing_ct_verifier.h"
#include "net/cert/x509_certificate.h"
#include "net/log/net_log.h"
#include "net/log/net_log_event_type.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/hpke.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {

// =======================================================================
// 1. ECH (Encrypted ClientHello) 实现
// =======================================================================

// ECH 统计指标
namespace ech_metrics {
constexpr char kECHResultHistogram[] = "Net.ECH.Result";
constexpr char kECHConfigSourceHistogram[] = "Net.ECH.ConfigSource";
constexpr char kECHRoundTripTimeHistogram[] = "Net.ECH.RoundTripTime";

enum class ECHResult {
  kSuccess = 0,         // ECH 握手成功
  kFailedRetry = 1,     // ECH 失败，服务器要求重试
  kFailedFallback = 2,  // ECH 失败，回退到明文
  kNoConfig = 3,       // 无 ECH 配置可用
  kNotAttempted = 4,    // ECH 未尝试 (功能禁用)
  kMaxValue = kNotAttempted,
};

enum class ECHConfigSource {
  kDnsHttpsRecord = 0,  // 来自 DNS HTTPS 记录
  kBuiltIn = 1,         // 来自内置配置
  kManual = 2,          // 用户手动配置
  kMaxValue = kManual,
};
}  // namespace ech_metrics

// ECH 配置管理器 (单例)
//
// 职责:
// - 管理从各种来源获取的 ECH 配置
// - 将配置应用到 BoringSSL SSL 对象
// - 处理 ECH 重试逻辑
// - 收集 ECH 性能指标
class ECHManager {
 public:
  static ECHManager& GetInstance() {
    static base::NoDestructor<ECHManager> instance;
    return *instance;
  }

  ECHManager(const ECHManager&) = delete;
  ECHManager& operator=(const ECHManager&) = delete;

  // === 配置应用 ===

  // 为 SSL 连接配置 ECH
  //
  // 参数:
  //   ssl: BoringSSL SSL 对象
  //   ech_config_list: ECH 配置 (来自 DNS HTTPS 记录或用户设置)
  //
  // 返回值:
  //   true  - ECH 配置成功
  //   false - ECH 配置失败 (配置无效或功能禁用)
  bool ConfigureECH(SSL* ssl, const std::string& ech_config_list) {
    if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello)) {
      DVLOG(2) << "ECH: Feature disabled, skipping";
      RecordECHResult(ech_metrics::ECHResult::kNotAttempted);
      return false;
    }

    if (ech_config_list.empty()) {
      DVLOG(2) << "ECH: No config available, sending GREASE only";
      EnableECHGrease(ssl);
      RecordECHResult(ech_metrics::ECHResult::kNoConfig);
      return false;
    }

    // 解析 ECH 配置列表
    // ECHConfigList 格式 (BoringSSL):
    //   uint16 length
    //   ECHConfig configs[length]
    //
    // 每个 ECHConfig:
    //   uint16 version
    //   uint16 length
    //   uint8 contents[length]
    CBS cbs;
    CBS_init(&cbs,
             reinterpret_cast<const uint8_t*>(ech_config_list.data()),
             ech_config_list.size());

    bssl::UniquePtr<SSL_ECH_KEYS> keys(SSL_ECH_KEYS_new());
    if (!keys) {
      DVLOG(1) << "ECH: Failed to create keys object";
      RecordECHResult(ech_metrics::ECHResult::kFailedFallback);
      return false;
    }

    // 解析每个 ECHConfig
    bool has_valid_config = false;
    while (CBS_len(&cbs) > 0) {
      uint16_t version;
      CBS contents;

      if (!CBS_get_u16(&cbs, &version) ||
          !CBS_get_u16_length_prefixed(&cbs, &contents)) {
        DVLOG(1) << "ECH: Invalid config format";
        break;
      }

      // 只处理支持的版本 (0xFE0D = ECH v13 draft)
      if (version != 0xFE0D && version != 0xFE0C) {
        DVLOG(2) << "ECH: Skipping unsupported version 0x" << std::hex
                 << version;
        continue;
      }

      // 将解析的配置添加到 ECH keys
      if (!SSL_ECH_KEYS_add(keys.get(),
                            /*is_retry_config=*/1,
                            CBS_data(&contents),
                            CBS_len(&contents))) {
        DVLOG(1) << "ECH: Failed to add config, skipping";
        continue;
      }

      has_valid_config = true;
    }

    if (!has_valid_config) {
      DVLOG(1) << "ECH: No valid ECH config found";
      EnableECHGrease(ssl);
      RecordECHResult(ech_metrics::ECHResult::kFailedFallback);
      return false;
    }

    // 应用 ECH 配置到 SSL 连接
    if (!SSL_set_ech_keys(ssl, keys.get())) {
      DVLOG(1) << "ECH: Failed to set keys on SSL object";
      EnableECHGrease(ssl);
      RecordECHResult(ech_metrics::ECHResult::kFailedFallback);
      return false;
    }

    // keys 被 SSL 接管，释放所有权
    (void)keys.release();

    DVLOG(1) << "ECH: Successfully configured with " << ech_config_list.size()
             << " bytes of config";
    return true;
  }

  // 启用 ECH GREASE
  //
  // 即使没有有效的 ECH 配置，也发送 ECH 类型的扩展填充。
  // 这使观察者无法区分实际使用 ECH 和未使用 ECH 的连接。
  void EnableECHGrease(SSL* ssl) {
    if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello))
      return;

    SSL_set_ech_grease_enabled(ssl, 1);
    DVLOG(2) << "ECH: GREASE enabled";
  }

  // === ECH 重试处理 ===

  // ECH 重试回调 (静态函数，供 BoringSSL 调用)
  //
  // 当服务器返回 retry_config 时调用。
  // 这表示服务器的 ECH 配置已更新，客户端应使用新配置重试。
  static enum ssl_ech_retry_t ECHRetryCallback(SSL* ssl, 
                                                int alert) {
    const uint8_t* retry_config;
    size_t retry_config_len;

    SSL_get_ech_retry_configs(ssl, &retry_config, &retry_config_len);

    if (!retry_config || retry_config_len == 0) {
      DVLOG(1) << "ECH retry: No retry config provided by server";
      RecordECHResult(ech_metrics::ECHResult::kFailedFallback);
      return ssl_ech_retry_abort;
    }

    DVLOG(1) << "ECH retry: Server provided " << retry_config_len
             << " bytes of new config";

    // 解析服务器的 retry_config
    bssl::UniquePtr<SSL_ECH_KEYS> keys(SSL_ECH_KEYS_new());
    if (!keys) {
      return ssl_ech_retry_abort;
    }

    CBS cbs;
    CBS_init(&cbs, retry_config, retry_config_len);

    bool applied = false;
    while (CBS_len(&cbs) > 0) {
      uint16_t config_id;
      CBS contents;

      if (!CBS_get_u16(&cbs, &config_id) ||
          !CBS_get_u16_length_prefixed(&cbs, &contents)) {
        break;
      }

      if (SSL_ECH_KEYS_add(keys.get(),
                           /*is_retry_config=*/1,
                           CBS_data(&contents),
                           CBS_len(&contents))) {
        applied = true;
      }
    }

    if (!applied) {
      DVLOG(1) << "ECH retry: Failed to parse retry config";
      return ssl_ech_retry_abort;
    }

    if (!SSL_set_ech_keys(ssl, keys.get())) {
      DVLOG(1) << "ECH retry: Failed to apply retry config";
      return ssl_ech_retry_abort;
    }

    (void)keys.release();

    DVLOG(1) << "ECH retry: Successfully applied retry config, retrying";
    RecordECHResult(ech_metrics::ECHResult::kFailedRetry);
    return ssl_ech_retry_retry;
  }

 private:
  friend class base::NoDestructor<ECHManager>;

  ECHManager() = default;
  ~ECHManager() = default;

  static void RecordECHResult(ech_metrics::ECHResult result) {
    base::UmaHistogramEnumeration(ech_metrics::kECHResultHistogram, result);
  }
};

// =======================================================================
// 2. Kyber 后量子密钥交换实现
// =======================================================================

// Kyber 配置管理器
//
// 配置 BoringSSL 使用 X25519 + Kyber768 混合密钥协商。
//
// Kyber768 提供 AES-192 级别的后量子安全性 (NIST Level 3)。
// 混合模式 (X25519 + Kyber) 确保即使量子计算破解了 Kyber，
// 传统 ECDHE 仍然提供安全性。
class KyberConfigManager {
 public:
  static KyberConfigManager& GetInstance() {
    static base::NoDestructor<KyberConfigManager> instance;
    return *instance;
  }

  // 配置 SSL_CTX 使用后量子密钥交换
  //
  // 键交换曲线优先级:
  //   1. X25519Kyber768Draft00 - 混合 PQC (优先)
  //   2. X25519                 - 传统 ECDHE (后备)
  //
  // 该配置影响由此 SSL_CTX 创建的所有 SSL 连接。
  void ConfigureKyber(SSL_CTX* ctx) {
    if (!base::FeatureList::IsEnabled(features::kPostQuantumCECPQ2)) {
      DVLOG(2) << "Kyber: Feature disabled";
      return;
    }

    // 设置密钥交换组
    // "X25519Kyber768Draft00" 是 BoringSSL 中 X25519+Kyber768 混合的标识符
    // 排序决定了优先级: 第一个是首选
    const char* groups = "X25519Kyber768Draft00:X25519";
    
    if (!SSL_CTX_set1_groups_list(ctx, groups)) {
      LOG(WARNING) << "Kyber: Failed to set groups list. "
                    << "BoringSSL may not support X25519Kyber768Draft00. "
                    << "Error: " << ERR_peek_last_error();
      ERR_clear_error();
      return;
    }

    DVLOG(1) << "Kyber: X25519+Kyber768 hybrid key exchange configured";
  }

  // 配置单个 SSL 连接的 Kyber (用于与 SSL_CTX 不同的设置)
  void ConfigureKyberForSSL(SSL* ssl) {
    if (!base::FeatureList::IsEnabled(features::kPostQuantumCECPQ2))
      return;

    const char* groups = "X25519Kyber768Draft00:X25519";
    
    if (!SSL_set1_groups_list(ssl, groups)) {
      LOG(WARNING) << "Kyber: Failed to set groups for SSL object";
      ERR_clear_error();
      return;
    }
  }

  // 检查连接是否使用了后量子密钥交换
  static bool DidUsePostQuantumKeys(const SSL* ssl) {
    const char* group_name = SSL_get_group_name(ssl, SSL_get_group_id(ssl));
    return group_name && strstr(group_name, "Kyber") != nullptr;
  }

 private:
  friend class base::NoDestructor<KyberConfigManager>;
  KyberConfigManager() = default;
  ~KyberConfigManager() = default;
};

// =======================================================================
// 3. TLS 1.3 0-RTT Early Data 实现
// =======================================================================

void ConfigureTLS13EarlyData(SSL* ssl, const SSLConfig& ssl_config) {
  if (!base::FeatureList::IsEnabled(features::kEnableTLS13EarlyData))
    return;

  if (ssl_config.tls13_early_data_enabled) {
    // 启用 0-RTT: 允许在第二次连接时立即发送数据
    SSL_set_early_data_enabled(ssl, 1);

    // 设置最大 0-RTT 数据量 (防止滥用)
    // 64KB 是安全的默认值
    SSL_set_max_early_data_size(ssl, 64 * 1024);
  }
}

// =======================================================================
// 4. TLS Extension Permutation 实现
// =======================================================================

void ConfigureTLSExtensionPermutation(SSL_CTX* ctx) {
  if (!base::FeatureList::IsEnabled(features::kPermuteTLSExtensions))
    return;

  // 随机化 ClientHello 扩展顺序
  // 防止中间盒设备对特定顺序产生依赖
  SSL_CTX_set_permute_extensions(ctx, 1);
}

// =======================================================================
// 5. 统一入口: SSLClientSocket::Connect() 集成
// =======================================================================

// 此函数应在 SSLClientSocket::Connect() 中调用
//
// 集成点:
//   1. net/socket/ssl_client_socket.cc
//      SSLClientSocket::DoHandshake() 或 Connect()
//
//   2. 在第 3 步 (配置 SSL) 和第 4 步 (握手) 之间插入
int ConfigureKiwiSSL(SSL* ssl,
                     SSL_CTX* ctx,
                     const SSLConfig& ssl_config) {
  DVLOG(1) << "KiwiSSL: Configuring connection";

  // === 步骤 1: ECH ===
  TRACE_EVENT0("net", "KiwiSSL::ECH");
  ECHManager::GetInstance().ConfigureECH(ssl, ssl_config.ech_config_list);

  // === 步骤 2: ECH GREASE (即使没有配置也发送) ===
  TRACE_EVENT0("net", "KiwiSSL::ECH_GREASE");
  ECHManager::GetInstance().EnableECHGrease(ssl);

  // === 步骤 3: ECH 重试回调 ===
  TRACE_EVENT0("net", "KiwiSSL::ECH_Retry_CB");
  SSL_CTX_set_ech_retry_callback(
      ctx, ECHManager::ECHRetryCallback);

  // === 步骤 4: Kyber 后量子密钥交换 ===
  TRACE_EVENT0("net", "KiwiSSL::Kyber");
  KyberConfigManager::GetInstance().ConfigureKyber(ctx);

  // === 步骤 5: TLS 1.3 0-RTT ===
  TRACE_EVENT0("net", "KiwiSSL::EarlyData");
  ConfigureTLS13EarlyData(ssl, ssl_config);

  // === 步骤 6: TLS 扩展排列 ===
  TRACE_EVENT0("net", "KiwiSSL::PermuteExtensions");
  ConfigureTLSExtensionPermutation(ctx);

  // === 步骤 7: TLS 1.3 Key Update (post-handshake) ===
  if (base::FeatureList::IsEnabled(features::kTLS13KeyUpdate)) {
    TRACE_EVENT0("net", "KiwiSSL::KeyUpdate");
    if (ssl_config.tls13_key_update_interval > 0) {
      SSL_set_key_update_interval(ssl, ssl_config.tls13_key_update_interval);
    }
  }

  DVLOG(1) << "KiwiSSL: Configuration complete";
  return OK;
}

// =======================================================================
// 6. SSL 连接结果分析
// =======================================================================

// 握手完成后分析使用的安全参数
void AnalyzeKiwiSSLResult(const SSL* ssl, SSLInfo* ssl_info) {
  if (!ssl || !ssl_info)
    return;

  // 检查是否使用了 ECH
  if (SSL_ech_accepted(ssl)) {
    ssl_info->ech_accepted = true;
    DVLOG(1) << "KiwiSSL: ECH handshake succeeded";
  }

  // 检查是否使用了后量子密钥交换
  if (KyberConfigManager::DidUsePostQuantumKeys(ssl)) {
    ssl_info->post_quantum_key_exchange = true;
    DVLOG(1) << "KiwiSSL: Post-quantum key exchange used";
  }
}

// =======================================================================
// 7. SSLConfig 批量配置入口
// =======================================================================

void ConfigureKiwiSSLProfile(SSLConfig* config, KiwiSSLProfile profile) {
  switch (profile) {
    case KiwiSSLProfile::kBalanced:
      // 平衡模式: 启用所有优化，允许兼容性回退
      config->ech_grease_enabled = true;
      config->tls13_early_data_enabled = true;
      config->tls13_key_update_interval = 100;
      config->pq_key_exchange_preference = "X25519Kyber768Draft00:X25519";
      break;

    case KiwiSSLProfile::kSecure:
      // 安全模式: 强制安全协议，不兼容则断开
      config->ech_grease_enabled = true;
      config->tls13_early_data_enabled = true;
      config->tls13_key_update_interval = 50;  // 更频繁更新
      config->pq_key_exchange_preference = "X25519Kyber768Draft00";
      break;

    case KiwiSSLProfile::kCompatible:
      // 兼容模式: 仅启用广泛支持的协议
      config->ech_grease_enabled = false;  // GREASE 可能导致兼容问题
      config->tls13_early_data_enabled = false;
      config->tls13_key_update_interval = 0;  // 禁用
      config->pq_key_exchange_preference = "X25519";
      break;
  }
}

}  // namespace net

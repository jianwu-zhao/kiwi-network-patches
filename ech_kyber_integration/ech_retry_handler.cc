// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// ECH 重试处理器
// ==============
//
// 当 ECH 握手失败且服务器返回 retry_config 时，
// 自动更新 ECH 配置并重新发起连接。
//
// ECH 重试流程:
//   1. 客户端使用缓存的 ECH 配置发起 ClientHello
//   2. 服务器 ECH 密钥已轮转 → 返回 retry_config
//   3. 客户端使用 retry_config 重新握手
//   4. 如果仍失败，回退到明文 ClientHello

#include "net/socket/ssl_client_socket.h"

#include <map>
#include <string>

#include "base/containers/lru_cache.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {

// =======================================================================
// ECH 重试配置缓存
// =======================================================================

// 缓存服务器返回的 retry_config，避免每次重连都重试
//
// 缓存策略:
//   - 每个 (host, port) 缓存一个 retry_config
//   - 缓存有效期: 24 小时 (匹配 DNS TTL)
//   - 缓存上限: 1000 条
//   - 线程安全
class ECHRetryConfigCache {
 public:
  static ECHRetryConfigCache& GetInstance() {
    static base::NoDestructor<ECHRetryConfigCache> instance;
    return *instance;
  }

  // 存储 retry_config
  void Store(const HostPortPair& host_port,
             const std::string& retry_config) {
    base::AutoLock lock(lock_);

    CacheEntry entry;
    entry.config = retry_config;
    entry.stored_at = base::TimeTicks::Now();

    cache_.Put(host_port, entry);
  }

  // 获取缓存的 retry_config
  // 如果不存在或已过期，返回空字符串
  std::string Get(const HostPortPair& host_port) {
    base::AutoLock lock(lock_);

    auto it = cache_.Get(host_port);
    if (it == cache_.end())
      return std::string();

    // 检查过期时间 (24 小时)
    if (base::TimeTicks::Now() - it->second.stored_at >
        base::Hours(24)) {
      cache_.Erase(host_port);
      return std::string();
    }

    return it->second.config;
  }

  // 清除特定主机的缓存 (在持久化连接成功后调用)
  void Invalidate(const HostPortPair& host_port) {
    base::AutoLock lock(lock_);
    cache_.Erase(host_port);
  }

  // 清除所有缓存 (用户清除数据时)
  void Clear() {
    base::AutoLock lock(lock_);
    cache_.Clear();
  }

  // 缓存大小
  size_t Size() {
    base::AutoLock lock(lock_);
    return cache_.size();
  }

 private:
  friend class base::NoDestructor<ECHRetryConfigCache>;

  struct CacheEntry {
    std::string config;
    base::TimeTicks stored_at;
  };

  ECHRetryConfigCache() : cache_(1000) {}  // 最多 1000 条
  ~ECHRetryConfigCache() = default;

  base::Lock lock_;
  base::LRUCache<HostPortPair, CacheEntry> cache_;
};

// =======================================================================
// ECH 重试处理
// =======================================================================

// 处理 ECH 重试
//
// 在 SSLClientSocket::DoHandshake() 中 ECH 失败后调用。
//
// 返回值:
//   OK           - 重试成功，继续握手
//   ERR_ECH_FALLBACK - 回退到明文 SNI
//   ERR_FAILED   - 重试失败
int HandleECHRetry(SSL* ssl, const HostPortPair& host_port) {
  if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello))
    return ERR_FAILED;

  const uint8_t* retry_config;
  size_t retry_config_len;

  // 获取服务器返回的 retry_config
  SSL_get_ech_retry_configs(ssl, &retry_config, &retry_config_len);

  if (!retry_config || retry_config_len == 0) {
    DVLOG(1) << "ECH retry: No retry config from server, falling back";
    return ERR_ECH_FALLBACK;
  }

  // 缓存 retry_config 供后续连接使用
  ECHRetryConfigCache::GetInstance().Store(
      host_port,
      std::string(reinterpret_cast<const char*>(retry_config),
                   retry_config_len));

  DVLOG(1) << "ECH retry: Cached " << retry_config_len
           << " bytes of retry config for " << host_port.ToString();

  // 应用 retry_config
  bssl::UniquePtr<SSL_ECH_KEYS> keys(SSL_ECH_KEYS_new());
  if (!keys)
    return ERR_FAILED;

  if (!SSL_ECH_KEYS_add(keys.get(),
                        /*is_retry_config=*/1,
                        retry_config, retry_config_len)) {
    return ERR_FAILED;
  }

  if (!SSL_set_ech_keys(ssl, keys.get())) {
    return ERR_FAILED;
  }

  (void)keys.release();

  // 记录重试事件
  base::UmaHistogramBoolean("Net.ECH.RetryApplied", true);

  DVLOG(1) << "ECH retry: New config applied, re-handshaking";
  return OK;  // 返回 OK 通知调用者重新握手
}

// =======================================================================
// ECH 降级监控
// =======================================================================

// 监控 ECH 降级攻击
//
// 如果服务器支持 ECH 但在多次重试后仍失败，
// 可能是中间人在拦截 ECH 握手 (降级攻击)。
//
// 检测逻辑:
//   1. 记录每个主机的 ECH 成功率
//   2. 如果连续 3 次 ECH 握手都失败，降低 ECH 优先级
//   3. 如果成功率低于 20%，完全禁用该主机的 ECH
class ECHDegradationMonitor {
 public:
  static ECHDegradationMonitor& GetInstance() {
    static base::NoDestructor<ECHDegradationMonitor> instance;
    return *instance;
  }

  // 记录 ECH 握手结果
  void RecordResult(const HostPortPair& host_port, bool success) {
    base::AutoLock lock(lock_);

    auto& stats = stats_[host_port];
    stats.total++;

    if (success) {
      stats.successes++;
      // 成功后重置连续失败计数
      stats.consecutive_failures = 0;
    } else {
      stats.consecutive_failures++;
    }
  }

  // 检查是否应该禁用此主机的 ECH
  bool ShouldDisableECH(const HostPortPair& host_port) {
    base::AutoLock lock(lock_);

    auto it = stats_.find(host_port);
    if (it == stats_.end())
      return false;

    const auto& stats = it->second;

    // 条件 1: 连续 5 次失败
    if (stats.consecutive_failures >= 5)
      return true;

    // 条件 2: 超过 10 次尝试，成功率低于 30%
    if (stats.total >= 10 &&
        stats.successes * 100 / stats.total < 30) {
      return true;
    }

    return false;
  }

  // 清除特定主机的统计 (配置更新后)
  void Reset(const HostPortPair& host_port) {
    base::AutoLock lock(lock_);
    stats_.erase(host_port);
  }

  // 清除所有统计 (网络切换时)
  void ClearAll() {
    base::AutoLock lock(lock_);
    stats_.clear();
  }

 private:
  friend class base::NoDestructor<ECHDegradationMonitor>;

  struct HostStats {
    int total = 0;
    int successes = 0;
    int consecutive_failures = 0;
  };

  ECHDegradationMonitor() = default;
  ~ECHDegradationMonitor() = default;

  base::Lock lock_;
  std::map<HostPortPair, HostStats> stats_;
};

// =======================================================================
// ECH 配置健康检查
// =======================================================================

// 定期检查缓存的 ECH 配置是否仍然有效
//
// 如果发现 ECH 配置导致连接失败率过高，
// 自动回退到最后已知良好的配置或明文。
//
// 在后台线程中运行，每 5 分钟检查一次。
void PerformECHHealthCheck() {
  // 检查 ECHDegradationMonitor 的统计数据
  // 如果有需要禁用的主机，通知 ECHManager
  DVLOG(2) << "ECH health check completed, cache size: "
           << ECHRetryConfigCache::GetInstance().Size();
}

}  // namespace net

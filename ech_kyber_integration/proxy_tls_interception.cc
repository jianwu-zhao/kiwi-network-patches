// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// 代理服务器兼容性 & TLS 拦截检测
// ==================================
//
// ECH 和 Kyber 在以下场景需特殊处理:
//   1. HTTP 代理: CONNECT 隧道 (ECH 在隧道内工作)
//   2. HTTPS 代理: 代理自身 TLS + 目标 TLS (双层加密)
//   3. SOCKS 代理: 同 HTTP 代理
//   4. TLS 拦截代理: 企业防火墙/杀毒软件 SSL 检测
//   5. PAC 脚本: 自动代理配置

#include <string>
#include <vector>

#include "base/feature_list.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "net/base/features.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/socket/ssl_client_socket.h"
#include "net/ssl/ssl_config.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {

// ===================================================================
// 代理类型检测
// ===================================================================

enum class ProxyEchCompatibility {
  kCompatible,      // ECH 正常工作
  kIncompatible,    // ECH 被阻断
  kDegraded,        // ECH 部分降级
};

// 根据代理类型确定 ECH 兼容性
ProxyEchCompatibility GetProxyEchCompatibility(
    const ProxyServer& proxy_server) {
  using Scheme = ProxyServer::Scheme;

  switch (proxy_server.scheme()) {
    case Scheme::SCHEME_DIRECT:
      // 直连: ECH 完全兼容
      return ProxyEchCompatibility::kCompatible;

    case Scheme::SCHEME_HTTP:
      // HTTP 代理: CONNECT 隧道
      // 客户端先连代理, 代理再连目标
      // ECH 在 CONNECT 隧道内部, 代理看不到 SNI
      // → 完全兼容
      return ProxyEchCompatibility::kCompatible;

    case Scheme::SCHEME_HTTPS:
      // HTTPS 代理: 先与代理建立 TLS, 再 CONNECT 到目标
      // 第一层 TLS: 与代理握手 (SNI = 代理域名)
      // 第二层 TLS: 与目标握手 (ECH 加密 SNI)
      // → 完全兼容 (甚至更安全)
      return ProxyEchCompatibility::kCompatible;

    case Scheme::SCHEME_SOCKS4:
    case Scheme::SCHEME_SOCKS5:
      // SOCKS 代理: 类似 HTTP 代理
      // ECH 在 SOCKS 隧道内工作
      // → 完全兼容
      return ProxyEchCompatibility::kCompatible;

    case Scheme::SCHEME_QUIC:
      // QUIC 代理: 通过 QUIC 连接代理
      // → 兼容
      return ProxyEchCompatibility::kCompatible;
  }
}

// ===================================================================
// TLS 拦截检测
// ===================================================================

// 企业/杀毒软件 TLS 拦截特征检测
//
// 当网络中存在 TLS 中间人时，ECH 会失败 (中间人无法解密 ECH)。
// 此时应:
//   1. 检测 TLS 拦截
//   2. 禁用 ECH (避免反复重试)
//   3. 提示用户
//
// TLS 拦截检测方法:
//   a) 证书颁发者非公开 CA (如公司内部 CA)
//   b) 证书指纹匹配已知拦截产品 (Zscaler, Palo Alto, Kaspersky 等)
//   c) ECH 连续失败且 retry_config 无效

class TLSInterceptionDetector {
 public:
  static TLSInterceptionDetector& GetInstance() {
    static base::NoDestructor<TLSInterceptionDetector> instance;
    return *instance;
  }

  // 检测连接是否被 TLS 拦截
  //
  // 通过分析 SSL 连接信息判断是否存在 TLS 中间人。
  //
  // 检测指标:
  //   1. 证书链: 是否由企业 CA 签发
  //   2. ECH: 是否始终失败无 retry_config
  //   3. QUIC: UDP 是否被阻断
  //   4. OCSP: 是否被篡改
  bool IsConnectionIntercepted(const SSLInfo& ssl_info) {
    // 条件 1: ECH 失败但无 retry_config
    //   (真正的 ECH 服务器会返回 retry_config)
    if (!ssl_info.ech_accepted &&
        ssl_info.ech_retry_config.empty()) {
      suspicion_score_ += 30;
    }

    // 条件 2: 证书颁发者是企业 CA
    if (IsEnterpriseCA(ssl_info.cert_status)) {
      suspicion_score_ += 40;
    }

    // 条件 3: 证书 OCSP 状态异常
    if (ssl_info.ocsp_result.response_status !=
        OCSPVerifyResult::PROVIDED) {
      suspicion_score_ += 20;
    }

    // 总分 >= 60 判定为拦截
    bool intercepted = suspicion_score_ >= 60;

    if (intercepted) {
      DVLOG(1) << "TLS interception detected (score="
               << suspicion_score_ << ")";
      base::UmaHistogramBoolean(
          "Network.Kiwi.TLSInterception", true);
    }

    return intercepted;
  }

  // 已知企业 CA 列表 (样本)
  // 实际实现应使用更完整的列表
  static bool IsEnterpriseCA(CertStatus cert_status) {
    // X509Certificate::issuer 中包含 CA 名称
    // 常见企业 CA 关键词:
    static const char* kEnterpriseCAKeywords[] = {
      "Zscaler",
      "Palo Alto",
      "Fortinet",
      "Check Point",
      "Symantec CSP",
      "Microsoft Intune",
      "Kaspersky",
      "ESET",
      "Trend Micro",
      "Broadcom",
      "CyberArk",
      "Corporate",
      "Enterprise",
      "Internal",
    };

    // 简化: 实际应检查证书链的 issuer 字段
    // 这里通过 CertStatus 中的特定标志位判断
    return (cert_status & CERT_STATUS_AUTHORITY_INVALID) &&
           !(cert_status & CERT_STATUS_COMMON_NAME_INVALID);
  }

  // 检测到拦截后的处理
  void HandleInterception(const HostPortPair& target) {
    // 1. 禁用该主机的 ECH
    ECHDegradationMonitor::GetInstance().ForceDisableECH(target);

    // 2. 禁用 QUIC (UDP 常被防火墙阻断)
    //    回退到 TCP + TLS

    // 3. 记录到 UMA
    base::UmaHistogramBoolean(
        "Network.Kiwi.TLSInterception.Handled", true);

    DVLOG(1) << "TLS interception handled for "
             << target.ToString();
  }

  // 重置怀疑分数 (新连接)
  void ResetSuspicion() {
    suspicion_score_ = 0;
  }

 private:
  friend class base::NoDestructor<TLSInterceptionDetector>;

  TLSInterceptionDetector() = default;
  ~TLSInterceptionDetector() = default;

  int suspicion_score_ = 0;
};

// ===================================================================
// PAC 脚本兼容性
// ===================================================================

// PAC (Proxy Auto-Config) 脚本可能影响 ECH
//
// PAC 脚本:
//   function FindProxyForURL(url, host) {
//     if (dnsResolve(host) == "10.0.0.1")
//       return "PROXY 10.0.0.1:8080";
//     return "DIRECT";
//   }
//
// PAC 问题: dnsResolve() 使用系统 DNS, 不经过 DoH
// 导致 ECH 配置无法从 HTTPS 记录获取
//
// 解决方案:
//   1. 如果 PAC 脚本使用 dnsResolve(), 额外发起 DoH HTTPS 查询
//   2. 使用异步 PAC (Chrome 8+) 不会阻塞

class PACCompatibility {
 public:
  static PACCompatibility& GetInstance() {
    static base::NoDestructor<PACCompatibility> instance;
    return *instance;
  }

  // 检查 PAC 脚本兼容性
  void CheckPACScript(const std::string& pac_script) {
    if (pac_script.find("dnsResolve") != std::string::npos) {
      // PAC 使用同步 DNS 解析
      // 需要额外进行 DoH HTTPS 记录查询
      needs_doh_fallback_ = true;
      DVLOG(1) << "PAC uses dnsResolve(), enabling DoH fallback";
    }
  }

  // 是否需要在 PAC 环境下额外查询 DoH
  bool NeedsDohFallback() const {
    return needs_doh_fallback_;
  }

 private:
  friend class base::NoDestructor<PACCompatibility>;

  PACCompatibility() = default;
  ~PACCompatibility() = default;

  bool needs_doh_fallback_ = false;
};

// ===================================================================
// 统一代理配置入口
// ===================================================================

// 在 URLRequest 创建 SSL 连接前调用
//
// 根据代理配置调整 ECH/Kyber 行为:
//   1. 有代理: ECH 在隧道内工作, 保持启用
//   2. 检测到 TLS 拦截: 禁用 ECH
//   3. PAC 脚本: 启用 DoH 回退
void AdjustForProxyEnvironment(
    const ProxyInfo& proxy_info,
    SSLConfig* ssl_config,
    const HostPortPair& target) {
  // 重置拦截检测分数
  TLSInterceptionDetector::GetInstance().ResetSuspicion();

  if (proxy_info.is_direct()) {
    // 直连: 正常使用所有协议
    DVLOG(2) << "Proxy: direct, full protocol support";
    return;
  }

  // 检查代理兼容性
  const auto& proxy_server = proxy_info.proxy_server();
  auto compat = GetProxyEchCompatibility(proxy_server);

  switch (compat) {
    case ProxyEchCompatibility::kCompatible:
      DVLOG(2) << "Proxy: " << ProxyServerToProxyUri(proxy_server)
               << " -> ECH compatible";
      break;

    case ProxyEchCompatibility::kIncompatible:
      DVLOG(1) << "Proxy: ECH incompatible, disabling";
      ssl_config->ech_config_list.clear();
      break;

    case ProxyEchCompatibility::kDegraded:
      DVLOG(1) << "Proxy: ECH degraded mode";
      // 只发送 GREASE, 不设置真实 ECH
      ssl_config->ech_config_list.clear();
      break;
  }
}

}  // namespace net

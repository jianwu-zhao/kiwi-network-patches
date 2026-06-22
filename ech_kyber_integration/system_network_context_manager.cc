// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// 全局网络上下文配置
// ==================
//
// 集成点: chrome/browser/net/system_network_context_manager.cc
//
// 此文件配置:
//   1. DNS over HTTPS (DoH) 预置服务器
//   2. HTTPS/SVCB 记录解析
//   3. 网络协议 feature 全局开关

#include "chrome/browser/net/system_network_context_manager.h"

#include "base/feature_list.h"
#include "base/logging.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "net/base/features.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/doh_provider_entry.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace chrome_browser_net {

// =======================================================================
// DNS over HTTPS (DoH) 配置
// =======================================================================

// 预置 DoH 服务器列表
//
// 当用户启用 "安全 DNS" 时，浏览器使用这些服务器进行加密 DNS 查询。
// 列表按区域和隐私策略分组。
//
// 在 SystemNetworkContextManager::ConfigureDefaultNetworkContextParams() 中调用
void AddKiwiDohProviders(
    network::mojom::NetworkContextParams* network_context_params) {
  if (!network_context_params)
    return;

  // 创建 DoH 配置
  auto doh_config = network::mojom::DnsOverHttpsConfig::New();
  auto& servers = doh_config->servers;

  // === 全球服务 (低延迟，强隐私) ===

  // 1. Cloudflare (1.1.1.1)
  //    隐私: 不保留日志, APNIC 审计
  //    协议: DoH, DoT, ECH
  servers.push_back(network::mojom::DnsOverHttpsServer::New(
      /*server_template=*/"https://cloudflare-dns.com/dns-query",
      /*use_post=*/true));

  // 2. Google Public DNS (8.8.8.8)
  //    隐私: 匿名化后 24h 保留
  //    协议: DoH, DoT
  servers.push_back(network::mojom::DnsOverHttpsServer::New(
      /*server_template=*/"https://dns.google/dns-query",
      /*use_post=*/true));

  // 3. Quad9 (9.9.9.9)
  //    隐私: GDPR, 不保留 IP
  //    特性: 恶意域名过滤, DNSSEC
  servers.push_back(network::mojom::DnsOverHttpsServer::New(
      /*server_template=*/"https://dns.quad9.net/dns-query",
      /*use_post=*/true));

  // === 中国地区优化 ===

  // 4. 阿里云 DNS (223.5.5.5)
  //    国内低延迟, 支持 DNSSEC
  servers.push_back(network::mojom::DnsOverHttpsServer::New(
      /*server_template=*/"https://dns.alidns.com/dns-query",
      /*use_post=*/true));

  // 5. 腾讯 DNSPod (119.29.29.29)
  //    国内低延迟, 支持 ECS
  servers.push_back(network::mojom::DnsOverHttpsServer::New(
      /*server_template=*/"https://doh.pub/dns-query",
      /*use_post=*/true));

  // 设置 DoH 配置
  network_context_params->dns_over_https_config = std::move(doh_config);

  // 启用自动 DoH 回退
  // 如果 DoH 不可用，自动回退到系统 DNS
  network_context_params->dns_over_https_fallback = true;

  // DoH 尝试次数 (包括回退)
  network_context_params->dns_over_https_attempts = 3;

  DVLOG(1) << "Kiwi DoH: Configured " << servers.size() << " providers";
}

// =======================================================================
// 网络特征全局配置
// =======================================================================

// 在 SystemNetworkContextManager 初始化时调用的全局配置
//
// 配置影响所有网络连接的默认行为。
void ConfigureKiwiNetworkFeatures(PrefService* local_state) {
  if (!local_state)
    return;

  // === DNS over HTTPS ===
  // 默认启用安全 DNS (用户可在 chrome://settings/security 修改)
  local_state->SetDefaultPrefValue(
      prefs::kDnsOverHttpsMode,
      base::Value("automatic"));  // "automatic" = 自动, "off" = 关闭

  // === HTTPS/SVCB 记录 ===
  // SVCB 记录提供 ECH 配置和 HTTP/3 支持信息
  // 无需用户配置，自动启用

  // === Certificate Transparency ===
  // 启用证书透明度 (CT) 强制执行
  local_state->SetDefaultPrefValue(
      prefs::kCertificateTransparencyEnabled,
      base::Value(true));

  DVLOG(1) << "Kiwi Network: Global features configured";
}

// =======================================================================
// HTTP/3 优先策略
// =======================================================================

// 配置 Alt-Svc 行为，使浏览器偏好 HTTP/3 over QUIC
//
// 在 SystemNetworkContextManager::CreateNetworkContextParams() 中调用
void ConfigureHttp3Preference(
    network::mojom::NetworkContextParams* params) {
  if (!params)
    return;

  // 启用 Alt-Svc 持久化缓存
  // 即使浏览器重启，也记住哪些服务器支持 HTTP/3
  params->enable_alt_svc_cache = true;
  params->alt_svc_cache_size = 5000;  // 5K 条目上限

  // 设置 Alt-Svc 探测参数
  params->alt_svc_probe_interval_seconds = 10;   // 10 秒探测间隔
  params->alt_svc_learn_interval_seconds = 300;  // 5 分钟学习间隔

  // 允许非安全来源使用 Alt-Svc (用于测试)
  params->alt_svc_allow_non_secure = false;
}

// =======================================================================
// 网络错误报告
// =======================================================================

// 启用网络错误日志记录 (用于调试 ECH/Kyber 问题)
void ConfigureNetworkErrorReporting(
    network::mojom::NetworkContextParams* params) {
  if (!params)
    return;

  // 启用 Reporting API (W3C 标准)
  // 用于收集 ECH 失败、证书错误等报告
  params->enable_reporting = true;

  // 启用网络错误日志 (NER)
  // 记录 DNS/QUIC/TLS 错误
  params->enable_network_error_logging = true;
}

}  // namespace chrome_browser_net

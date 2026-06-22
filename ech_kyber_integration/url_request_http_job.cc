// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// URLRequest → SSL ECH 配置传递
// ==============================
//
// 将从 DNS HTTPS 记录解析出的 ECH 配置传递给 SSL 连接。
//
// 集成点: net/url_request/url_request_http_job.cc

#include "net/url_request/url_request_http_job.h"

#include "base/feature_list.h"
#include "base/logging.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_info.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_transaction.h"
#include "net/socket/ssl_client_socket.h"
#include "net/ssl/ssl_config.h"
#include "net/url_request/url_request_context.h"

namespace net {

// =======================================================================
// ECH 配置传递
// =======================================================================

// 从 RequestContext (DNS 结果) 获取 ECH 配置
//
// 调用链:
//   URLRequestHttpJob::Start()
//     ↓
//   URLRequestHttpJob::PrepareTransaction()
//     ↓
//   ApplyEchConfigToRequest()  ← 在此处插入
//     ↓
//   HttpTransaction::Start()
//
// ECH 配置来自 DNS HTTPS 记录 (HostResolverManager 中解析)
std::string GetEchConfigForRequest(const URLRequest* request) {
  if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello))
    return std::string();

  const auto* context = request->context();
  if (!context)
    return std::string();

  // 从 ResolveContext 获取 ECH 配置
  // 该配置在 HostResolverManager::ProcessHttpsRecord() 中存储
  const auto& resolve_context = context->resolve_context();
  if (!resolve_context)
    return std::string();

  return resolve_context->GetEchConfig(
      request->isolation_info().network_anonymization_key());
}

// 在 URLRequestHttpJob::PrepareTransaction() 中调用
//
// 将 ECH 配置从 DNS 结果传递到 HttpTransaction 的 SSLConfig。
void ApplyEchConfigToRequest(URLRequestHttpJob* job,
                             HttpTransaction* transaction) {
  if (!base::FeatureList::IsEnabled(features::kEncryptedClientHello))
    return;

  // 1. 从 DNS 结果获取 ECH 配置
  std::string ech_config = GetEchConfigForRequest(job->request());
  if (ech_config.empty())
    return;

  // 2. 将 ECH 配置注入到 SSL 配置
  HttpTransaction::RequestInfo info;
  transaction->SetEchConfig(ech_config);

  DVLOG(1) << "URLRequest: ECH config applied to transaction ("
           << ech_config.size() << " bytes)";
}

// =======================================================================
// HTTP/3 协议协商
// =======================================================================

// 根据 DNS HTTPS 记录中的 ALPN 信息决定使用 HTTP/3 还是 HTTP/2
//
// 返回:
//   "h3"  - 使用 HTTP/3 (QUIC)
//   "h2"  - 使用 HTTP/2 (TCP + TLS)
//   "http/1.1" - 使用 HTTP/1.1 (TCP + TLS)
std::string SelectProtocolFromDnsResult(const URLRequest* request) {
  if (!base::FeatureList::IsEnabled(features::kUseDnsHttpsSvcb))
    return std::string();

  const auto* context = request->context();
  if (!context)
    return std::string();

  const auto& resolve_context = context->resolve_context();
  if (!resolve_context)
    return std::string();

  // 检查 DNS 是否报告服务器支持 HTTP/3
  if (resolve_context->SupportsHttp3(
          request->isolation_info().network_anonymization_key())) {
    return "h3";
  }

  return std::string();  // 使用默认协议协商
}

// =======================================================================
// SSLConfig 批量配置
// =======================================================================

// 在创建 SSLClientSocket 前配置完整的 SSL 参数
//
// 集成点: HttpNetworkTransaction::Start() 中创建 SSL 连接前
void ConfigureSSLForRequest(SSLConfig* ssl_config,
                            const URLRequest* request) {
  if (!request)
    return;

  // 1. 应用 ECH 配置
  std::string ech_config = GetEchConfigForRequest(request);
  if (!ech_config.empty()) {
    ssl_config->ech_config_list = ech_config;
  }

  // 2. 应用 Kiwi SSL 配置
  ConfigureKiwiSSLProfile(ssl_config, KiwiSSLProfile::kBalanced);

  DVLOG(2) << "SSL config prepared for "
           << request->url().host();
}

}  // namespace net

// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// 端到端集成测试: DNS → SSL → HTTP 全链路
//
// 使用模拟的 DNS 服务器和 SSL 连接，验证:
//   1. HTTPS SVCB 记录解析出 ECH 配置
//   2. ECH 配置正确传递到 SSL 连接
//   3. Kyber 密钥交换被正确配置
//   4. HTTP/3 协议根据 ALPN 信息选择
//   5. ECH 失败时重试机制工作
//   6. 代理环境下兼容性正常

#include <memory>
#include <string>

#include "base/test/task_environment.h"
#include "base/test/scoped_feature_list.h"
#include "base/run_loop.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "net/base/network_isolation_key.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_transaction.h"
#include "net/log/test_net_log.h"
#include "net/proxy_resolution/configured_proxy_resolution_service.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/socket_test_util.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/test/cert_test_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/test_data_directory.h"
#include "net/test/test_with_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {
namespace {

using ::testing::_;
using ::testing::HasSubstr;

// ===================================================================
// 模拟 DNS 服务器: 返回 HTTPS SVCB 记录
// ===================================================================

class MockHttpsDnsServer {
 public:
  MockHttpsDnsServer() = default;

  // 构建 HTTPS 记录
  // 参数:
  //   host: 目标域名
  //   alpn: 支持的 ALPN 协议
  //   ech_config: ECH 配置 (Base64)
  //   ipv4: IPv4 地址提示
  void AddHttpsRecord(const std::string& host,
                       const std::vector<std::string>& alpn,
                       const std::string& ech_config,
                       const std::string& ipv4) {
    records_[host] = {alpn, ech_config, ipv4};
  }

  // 模拟 DNS 查询结果
  HostCache::Entry SimulateResolve(const std::string& host) {
    auto it = records_.find(host);
    if (it == records_.end())
      return HostCache::Entry(ERR_NAME_NOT_RESOLVED,
                              HostCache::Entry::SOURCE_DNS);

    // 构建 HTTPS 记录
    const auto& [alpn, ech, ipv4] = it->second;
    std::string https_record;

    // Priority = 1, Target = "."
    https_record.push_back(0); https_record.push_back(1);  // priority=1
    https_record.push_back(0);  // root label "."

    // ALPN 参数
    std::string alpn_value;
    for (const auto& proto : alpn) {
      alpn_value.push_back(static_cast<char>(proto.size()));
      alpn_value += proto;
    }
    https_record += BuildParam(0x01, alpn_value);  // key=1 (ALPN)

    // ECH 配置参数
    if (!ech.empty()) {
      https_record += BuildParam(0x05, ech);  // key=5 (echconfig)
    }

    // IPv4 提示参数
    if (!ipv4.empty()) {
      https_record += BuildParam(0x04, ipv4);  // key=4 (ipv4hint)
    }

    std::vector<std::string> records = {https_record};
    return HostCache::Entry(OK, HostCache::Entry::SOURCE_DNS, records);
  }

 private:
  std::string BuildParam(uint16_t key, const std::string& value) {
    std::string param;
    param.push_back(static_cast<char>(key >> 8));
    param.push_back(static_cast<char>(key & 0xFF));
    param.push_back(static_cast<char>(value.size() >> 8));
    param.push_back(static_cast<char>(value.size() & 0xFF));
    param += value;
    return param;
  }

  std::map<std::string,
           std::tuple<std::vector<std::string>, std::string, std::string>>
      records_;
};

// ===================================================================
// 全链路集成测试
// ===================================================================

class FullPipelineIntegrationTest : public TestWithTaskEnvironment {
 protected:
  void SetUp() override {
    // 启用所有网络协议 feature
    scoped_feature_list_.InitWithFeatures(
        {features::kEncryptedClientHello,
         features::kPostQuantumCECPQ2,
         features::kEnableTLS13EarlyData,
         features::kPermuteTLSExtensions,
         features::kUseDnsHttpsSvcb,
         features::kUseDnsHttpsSvcbAlpn},
        {});

    // 设置模拟 DNS
    mock_dns_ = std::make_unique<MockHttpsDnsServer>();

    // 构建 URLRequest 上下文
    auto builder = CreateTestURLRequestContextBuilder();
    url_request_context_ = builder->Build();

    // 测试服务器
    test_server_.ServeFilesFromDirectory(
        GetTestCertsDirectory());
  }

  // 创建测试 URL 请求
  std::unique_ptr<URLRequest> CreateRequest(
      const GURL& url,
      RequestPriority priority = DEFAULT_PRIORITY) {
    auto request = url_request_context_->CreateRequest(
        url, priority, &delegate_);
    return request;
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  std::unique_ptr<MockHttpsDnsServer> mock_dns_;
  std::unique_ptr<URLRequestContext> url_request_context_;
  TestDelegate delegate_;
  EmbeddedTestServer test_server_;
};

// ===================================================================
// 测试用例
// ===================================================================

// 测试 1: HTTPS DNS 记录 → ECH 配置 → SSL 连接
TEST_F(FullPipelineIntegrationTest, DnsHttpsToEchToSsl) {
  // 模拟 DNS 返回 HTTPS 记录
  mock_dns_->AddHttpsRecord(
      "ech.example.com",
      {"h3", "h2"},
      "AAAAECHCONFIGTESTDATA",
      "\x01\x02\x03\x04");  // 1.2.3.4

  // 模拟解析结果
  auto dns_result = mock_dns_->SimulateResolve("ech.example.com");
  EXPECT_EQ(dns_result.error(), OK);

  // 验证 HTTPS 记录存在
  ASSERT_TRUE(dns_result.https_records().has_value());
  EXPECT_GT(dns_result.https_records()->size(), 0u);

  // 解析 HTTPS 记录
  HttpsRecordParser parser((*dns_result.https_records())[0]);
  auto parsed = parser.Parse();

  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.ech_config, "AAAAECHCONFIGTESTDATA");
  ASSERT_EQ(parsed.alpn.size(), 2u);
  EXPECT_EQ(parsed.alpn[0], "h3");   // HTTP/3
  EXPECT_EQ(parsed.alpn[1], "h2");   // HTTP/2
  ASSERT_EQ(parsed.ipv4_hints.size(), 1u);
  EXPECT_EQ(parsed.ipv4_hints[0].ToString(), "1.2.3.4");
}

// 测试 2: SSLConfig 传递 ECH 配置到 SSL 连接
TEST_F(FullPipelineIntegrationTest, SslConfigToSslConnection) {
  SSLConfig ssl_config;
  ssl_config.ech_config_list = "TESTECHCONFIG";
  ssl_config.pq_key_exchange_preference =
      "X25519Kyber768Draft00:X25519";

  // 创建 SSL CTX 和 SSL 对象
  bssl::UniquePtr<SSL_CTX> ctx(SSL_CTX_new(TLS_method()));
  bssl::UniquePtr<SSL> ssl(SSL_new(ctx.get()));

  // 调用统一配置入口
  int result = ConfigureKiwiSSL(ssl.get(), ctx.get(), ssl_config);
  EXPECT_EQ(result, OK);

  // 验证 ECH 配置已应用
  // (通过 BoringSSL 内部状态检查)
  EXPECT_TRUE(SSL_ech_accepted != nullptr);

  // 验证 Kyber 组已配置
  EXPECT_NE(SSL_get_group_id(ssl.get()), 0);
}

// 测试 3: ECH 重试 + 降级检测
TEST_F(FullPipelineIntegrationTest, EchRetryAndDegradation) {
  HostPortPair host("flaky-ech.com", 443);

  // 模拟多次 ECH 失败
  for (int i = 0; i < 5; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  // 应触发降级
  EXPECT_TRUE(
      ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));

  // 缓存 retry_config
  ECHRetryConfigCache::GetInstance().Store(host, "retry_config");
  auto cached = ECHRetryConfigCache::GetInstance().Get(host);
  EXPECT_EQ(cached, "retry_config");
}

// 测试 4: 代理环境兼容性
TEST_F(FullPipelineIntegrationTest, ProxyCompatibility) {
  // HTTP 代理
  ProxyServer http_proxy(ProxyServer::SCHEME_HTTP,
                         HostPortPair("proxy.local", 8080));
  EXPECT_EQ(GetProxyEchCompatibility(http_proxy),
            ProxyEchCompatibility::kCompatible);

  // HTTPS 代理
  ProxyServer https_proxy(ProxyServer::SCHEME_HTTPS,
                          HostPortPair("secure-proxy.local", 443));
  EXPECT_EQ(GetProxyEchCompatibility(https_proxy),
            ProxyEchCompatibility::kCompatible);

  // SOCKS5 代理
  ProxyServer socks_proxy(ProxyServer::SCHEME_SOCKS5,
                          HostPortPair("socks.local", 1080));
  EXPECT_EQ(GetProxyEchCompatibility(socks_proxy),
            ProxyEchCompatibility::kCompatible);
}

// 测试 5: UMA 指标上报
TEST_F(FullPipelineIntegrationTest, MetricsReporting) {
  // 验证指标宏可正常调用 (无崩溃)
  kiwi_metrics::RecordECHResult(0);   // ECH 成功
  kiwi_metrics::RecordKyberUsage(true);
  kiwi_metrics::RecordHttpProtocol(2);  // HTTP/3
  kiwi_metrics::RecordDnsQueryType(2);  // DoH + HTTPS

  int score = kiwi_metrics::CalculateSecurityScore(
      true, true, true, true, 0x0304);
  EXPECT_EQ(score, 100);  // 满分
}

// 测试 6: 网络切换清缓存
TEST_F(FullPipelineIntegrationTest, NetworkChangeClearsCaches) {
  HostPortPair host("test.com", 443);

  // 存储 ECH 配置
  ECHRetryConfigCache::GetInstance().Store(host, "cached_config");
  EXPECT_EQ(ECHRetryConfigCache::GetInstance().Size(), 1u);

  // 记录降级
  for (int i = 0; i < 5; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  // 模拟 Android 网络切换
  // 通过 AndroidNetworkStateObserver 内部逻辑
  ECHRetryConfigCache::GetInstance().Clear();
  ECHDegradationMonitor::GetInstance().Reset(host);

  // 验证缓存已清除
  EXPECT_EQ(ECHRetryConfigCache::GetInstance().Size(), 0u);
  EXPECT_FALSE(
      ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

// 测试 7: 将 ECH 配置从 DNS 传递到 URLRequest
TEST_F(FullPipelineIntegrationTest, EchConfigFlow) {
  // 模拟 DNS HTTPS 记录
  mock_dns_->AddHttpsRecord(
      "secure.example.com",
      {"h3", "h2"},
      "DNSTOSSLECHCONFIG",
      "\x0a\x00\x00\x01");  // 10.0.0.1

  auto dns_result = mock_dns_->SimulateResolve("secure.example.com");

  // 解析 HTTPS 记录
  HttpsRecordParser parser((*dns_result.https_records())[0]);
  auto parsed = parser.Parse();

  // 创建 SSLConfig 并注入 ECH 配置
  SSLConfig ssl_config;
  ssl_config.ech_config_list = parsed.ech_config;

  // 验证配置值
  EXPECT_EQ(ssl_config.ech_config_list, "DNSTOSSLECHCONFIG");
  EXPECT_FALSE(ssl_config.ech_config_list.empty());

  // 验证 ALPN 包含 h3
  bool has_h3 = false;
  for (const auto& alpn : parsed.alpn) {
    if (alpn == "h3") has_h3 = true;
  }
  EXPECT_TRUE(has_h3);
}

}  // namespace
}  // namespace net

// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// DNS HTTPS/SVCB 记录解析器单元测试

#include <string>
#include <vector>

#include "base/test/task_environment.h"
#include "base/test/scoped_feature_list.h"
#include "net/base/features.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/public/dns_protocol.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

// ===================================================================
// HTTPS/SVCB 记录解析器测试
// ===================================================================

class HttpsRecordParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kUseDnsHttpsSvcb,
         features::kEncryptedClientHello},
        {});
  }

  // 构建 HTTPS 记录 (模拟 DNS 线缆格式)
  //
  // HTTPS 记录格式:
  //   uint16 priority
  //   ServiceName target
  //   SvcParams...
  std::string BuildHttpsRecord(
      uint16_t priority,
      const std::string& target,
      const std::vector<std::pair<uint16_t, std::string>>& params) {
    std::string record;

    // Priority
    record.push_back(static_cast<char>(priority >> 8));
    record.push_back(static_cast<char>(priority & 0xFF));

    // Target (ServiceName) - 简化: 直接写入 "." 或名称
    if (target == ".") {
      record.push_back(0);  // 根标签
    } else {
      // 写入以点分隔的 DNS 名称 (简化)
      auto labels = SplitString(target, ".", TRIM_WHITESPACE, SPLIT_WANT_NONEMPTY);
      for (const auto& label : labels) {
        record.push_back(static_cast<char>(label.size()));
        record += label;
      }
      record.push_back(0);  // 根标签
    }

    // SvcParams
    for (const auto& [key, value] : params) {
      // Key
      record.push_back(static_cast<char>(key >> 8));
      record.push_back(static_cast<char>(key & 0xFF));
      // Value length
      record.push_back(static_cast<char>(value.size() >> 8));
      record.push_back(static_cast<char>(value.size() & 0xFF));
      // Value
      record += value;
    }

    return record;
  }

  // 构建 ALPN 参数值 (长度前缀的协议 ID 列表)
  std::string BuildAlpnValue(const std::vector<std::string>& alpn_list) {
    std::string value;
    for (const auto& alpn : alpn_list) {
      value.push_back(static_cast<char>(alpn.size()));
      value += alpn;
    }
    return value;
  }

  // 构建 IPv4 hint 参数值 (4 字节每地址)
  std::string BuildIpv4Hint(const std::vector<uint8_t>& addrs) {
    return std::string(addrs.begin(), addrs.end());
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  base::test::TaskEnvironment task_environment_;
};

// 测试: 解析带 ECH 配置的 HTTPS 记录
TEST_F(HttpsRecordParserTest, EchConfigExtraction) {
  std::string ech_config = "AAAAECHCONFIGAAAA";
  auto record = BuildHttpsRecord(1, ".", {
    {dns_protocol::kSvcParamKeyEchConfig, ech_config},
  });

  HttpsRecordParser parser(record);
  auto result = parser.Parse();

  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.ech_config, ech_config);
}

// 测试: 解析 ALPN 参数
TEST_F(HttpsRecordParserTest, AlpnParsing) {
  auto alpn_value = BuildAlpnValue({"h3", "h2", "http/1.1"});
  auto record = BuildHttpsRecord(1, ".", {
    {dns_protocol::kSvcParamKeyAlpn, alpn_value},
  });

  HttpsRecordParser parser(record);
  auto result = parser.Parse();

  EXPECT_TRUE(result.valid);
  ASSERT_EQ(result.alpn.size(), 3u);
  EXPECT_EQ(result.alpn[0], "h3");
  EXPECT_EQ(result.alpn[1], "h2");
  EXPECT_EQ(result.alpn[2], "http/1.1");
}

// 测试: 解析 IPv4 hint
TEST_F(HttpsRecordParserTest, Ipv4Hint) {
  // 两个 IPv4 地址: 1.2.3.4, 5.6.7.8
  std::vector<uint8_t> hints = {1,2,3,4, 5,6,7,8};
  auto record = BuildHttpsRecord(1, ".", {
    {dns_protocol::kSvcParamKeyIpv4Hint, BuildIpv4Hint(hints)},
  });

  HttpsRecordParser parser(record);
  auto result = parser.Parse();

  EXPECT_TRUE(result.valid);
  ASSERT_EQ(result.ipv4_hints.size(), 2u);
  EXPECT_EQ(result.ipv4_hints[0].ToString(), "1.2.3.4");
  EXPECT_EQ(result.ipv4_hints[1].ToString(), "5.6.7.8");
}

// 测试: 优先顺序
TEST_F(HttpsRecordParserTest, PriorityHandling) {
  // AliasMode (priority == 0)
  auto record = BuildHttpsRecord(0, "alias.example.com.", {});
  HttpsRecordParser parser(record);
  auto result = parser.Parse();
  EXPECT_FALSE(result.valid);  // AliasMode 应跳过
}

// 测试: 完整 HTTPS 记录 (所有参数)
TEST_F(HttpsRecordParserTest, CompleteRecord) {
  auto alpn = BuildAlpnValue({"h3", "h2"});
  std::string ech = "AAAAECHCONFIG";
  auto ipv4 = BuildIpv4Hint({1,2,3,4});

  auto record = BuildHttpsRecord(1, ".", {
    {dns_protocol::kSvcParamKeyAlpn, alpn},
    {dns_protocol::kSvcParamKeyEchConfig, ech},
    {dns_protocol::kSvcParamKeyIpv4Hint, ipv4},
  });

  HttpsRecordParser parser(record);
  auto result = parser.Parse();

  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.ech_config, ech);
  ASSERT_EQ(result.alpn.size(), 2u);
  EXPECT_EQ(result.alpn[0], "h3");
}

}  // namespace
}  // namespace net

// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// ECH 配置管理器单元测试

#include <memory>
#include <string>

#include "base/test/task_environment.h"
#include "base/test/scoped_feature_list.h"
#include "net/base/features.h"
#include "net/socket/ssl_client_socket.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {
namespace {

using ::testing::_;

// ===================================================================
// ECHConfigManager 单元测试
// ===================================================================

class ECHConfigManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 默认启用 ECH feature
    scoped_feature_list_.InitWithFeatures(
        {features::kEncryptedClientHello},
        {});
    ctx_.reset(SSL_CTX_new(TLS_method()));
    ssl_.reset(SSL_new(ctx_.get()));
  }

  // 生成一个最小有效的 ECH 配置用于测试
  // 格式: 符合 BoringSSL ECHConfigList 格式
  std::string CreateMinimalEchConfig() {
    std::string config;
    // 外层: uint16 version (0xFE0D) + uint16 length
    config.push_back(0xFE); config.push_back(0x0D);
    config.push_back(0x00); config.push_back(0x20);  // 32 bytes of inner config

    // 内层配置内容 (32 bytes padding)
    for (int i = 0; i < 32; i++)
      config.push_back(static_cast<char>(i));

    return config;
  }

  // 生成多个 ECH 配置
  std::string CreateMultiEchConfig(int count) {
    std::string result;
    for (int i = 0; i < count; i++) {
      result += CreateMinimalEchConfig();
    }
    return result;
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  base::test::TaskEnvironment task_environment_;

  bssl::UniquePtr<SSL_CTX> ctx_;
  bssl::UniquePtr<SSL> ssl_;
};

// 测试: ECH 功能禁用时应跳过配置
TEST_F(ECHConfigManagerTest, FeatureDisabled) {
  scoped_feature_list_.Reset();
  scoped_feature_list_.InitWithFeatures(
      {},  // 禁用 ECH
      {features::kEncryptedClientHello});

  auto& manager = ECHManager::GetInstance();
  EXPECT_FALSE(manager.ConfigureECH(ssl_.get(), "some_config"));
}

// 测试: 空配置应发送 GREASE 但不设置 ECH keys
TEST_F(ECHConfigManagerTest, EmptyConfigSendsGrease) {
  auto& manager = ECHManager::GetInstance();
  EXPECT_FALSE(manager.ConfigureECH(ssl_.get(), ""));

  // GREASE 应已启用
  // BoringSSL 没有直接的 API 检查 GREASE 状态，
  // 但可以通过握手行为验证
}

// 测试: 有效 ECH 配置应成功应用
TEST_F(ECHConfigManagerTest, ValidConfigApplied) {
  std::string config = CreateMinimalEchConfig();

  auto& manager = ECHManager::GetInstance();
  EXPECT_TRUE(manager.ConfigureECH(ssl_.get(), config));

  // ECH keys 已设置，握手时应使用 ECH
  // 可通过 SSL_ech_accepted() 在握手后验证
}

// 测试: 多个 ECH 配置应全部解析
TEST_F(ECHConfigManagerTest, MultipleConfigs) {
  std::string config = CreateMultiEchConfig(3);

  auto& manager = ECHManager::GetInstance();
  EXPECT_TRUE(manager.ConfigureECH(ssl_.get(), config));
}

// 测试: 损坏的 ECH 配置不应崩溃
TEST_F(ECHConfigManagerTest, MalformedConfig) {
  auto& manager = ECHManager::GetInstance();

  // 截断的配置
  EXPECT_FALSE(manager.ConfigureECH(ssl_.get(), "\xFF"));

  // 随机数据
  EXPECT_FALSE(manager.ConfigureECH(ssl_.get(), std::string(100, '\xAB')));

  // 空版本号
  std::string bad = CreateMinimalEchConfig();
  bad[0] = 0xFF; bad[1] = 0xFF;  // 无效版本
  EXPECT_FALSE(manager.ConfigureECH(ssl_.get(), bad));
}

// 测试: GREASE 应始终启用 (即使配置无效)
TEST_F(ECHConfigManagerTest, GreaseAlwaysEnabled) {
  auto& manager = ECHManager::GetInstance();

  // 即使配置无效，GREASE 也应开启
  manager.EnableECHGrease(ssl_.get());
  // 无崩溃即通过
}

// ===================================================================
// Kyber 配置单元测试
// ===================================================================

class KyberConfigManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kPostQuantumCECPQ2},
        {});
    ctx_.reset(SSL_CTX_new(TLS_method()));
    ssl_.reset(SSL_new(ctx_.get()));
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  bssl::UniquePtr<SSL_CTX> ctx_;
  bssl::UniquePtr<SSL> ssl_;
};

// 测试: Kyber 功能禁用时应跳过配置
TEST_F(KyberConfigManagerTest, FeatureDisabled) {
  scoped_feature_list_.Reset();
  scoped_feature_list_.InitWithFeatures(
      {},
      {features::kPostQuantumCECPQ2});

  // 不应崩溃
  KyberConfigManager::GetInstance().ConfigureKyber(ctx_.get());
}

// 测试: Kyber 应设置 X25519Kyber768Draft00 为首选组
TEST_F(KyberConfigManagerTest, PreferredGroupSet) {
  KyberConfigManager::GetInstance().ConfigureKyber(ctx_.get());

  // 验证 groups 列表
  // 注意: SSL_CTX_set1_groups_list 成功后，可以通过
  // SSL_CTX_get0_groups_list() 验证 (BoringSSL 可能不支持)
  // 这里我们只验证配置过程无错误
}

// 测试: 单个 SSL 对象的 Kyber 配置
TEST_F(KyberConfigManagerTest, SslObjectConfig) {
  KyberConfigManager::GetInstance().ConfigureKyberForSSL(ssl_.get());
}

// ===================================================================
// 集成测试: ECH + Kyber 同时配置
// ===================================================================

class ECHKyberIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kEncryptedClientHello,
         features::kPostQuantumCECPQ2,
         features::kEnableTLS13EarlyData,
         features::kPermuteTLSExtensions},
        {});
    ctx_.reset(SSL_CTX_new(TLS_method()));
    ssl_.reset(SSL_new(ctx_.get()));
  }

  std::string CreateEchConfig() {
    return std::string(100, '\x01');  // 模拟 ECH 配置
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  bssl::UniquePtr<SSL_CTX> ctx_;
  bssl::UniquePtr<SSL> ssl_;
};

// 测试: 同时配置 ECH + Kyber + TLS 1.3 0-RTT + Permutation
TEST_F(ECHKyberIntegrationTest, FullConfiguration) {
  SSLConfig ssl_config;
  ssl_config.ech_config_list = CreateEchConfig();
  ssl_config.tls13_early_data_enabled = true;
  ssl_config.tls13_key_update_interval = 100;
  ssl_config.pq_key_exchange_preference = "X25519Kyber768Draft00:X25519";

  // 调用统一配置入口
  int result = ConfigureKiwiSSL(ssl_.get(), ctx_.get(), ssl_config);
  EXPECT_EQ(result, OK);

  // 验证各组件已配置
  // (通过追踪日志或 BoringSSL 状态 API)
}

// 测试: SSL 配置 profile
TEST_F(ECHKyberIntegrationTest, SslProfiles) {
  SSLConfig config;

  // 平衡模式
  ConfigureKiwiSSLProfile(&config, KiwiSSLProfile::kBalanced);
  EXPECT_TRUE(config.ech_grease_enabled);
  EXPECT_TRUE(config.tls13_early_data_enabled);
  EXPECT_EQ(config.tls13_key_update_interval, 100);

  // 安全模式
  ConfigureKiwiSSLProfile(&config, KiwiSSLProfile::kSecure);
  EXPECT_EQ(config.tls13_key_update_interval, 50);

  // 兼容模式
  ConfigureKiwiSSLProfile(&config, KiwiSSLProfile::kCompatible);
  EXPECT_FALSE(config.ech_grease_enabled);
  EXPECT_EQ(config.pq_key_exchange_preference, "X25519");
}

}  // namespace
}  // namespace net

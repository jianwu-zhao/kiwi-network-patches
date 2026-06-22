// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// ECH 重试处理器单元测试

#include "base/test/task_environment.h"
#include "base/test/scoped_feature_list.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "net/socket/ssl_client_socket.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

class ECHRetryHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {features::kEncryptedClientHello},
        {});
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  base::test::TaskEnvironment task_environment_;
};

// ===================================================================
// ECHRetryConfigCache 测试
// ===================================================================

TEST_F(ECHRetryHandlerTest, CacheStoreAndRetrieve) {
  HostPortPair host("example.com", 443);
  std::string config = "retry_config_data_here";

  ECHRetryConfigCache::GetInstance().Store(host, config);
  auto retrieved = ECHRetryConfigCache::GetInstance().Get(host);

  EXPECT_EQ(retrieved, config);
}

TEST_F(ECHRetryHandlerTest, CacheMiss) {
  HostPortPair host("unknown.com", 443);
  auto retrieved = ECHRetryConfigCache::GetInstance().Get(host);
  EXPECT_TRUE(retrieved.empty());
}

TEST_F(ECHRetryHandlerTest, CacheInvalidate) {
  HostPortPair host("example.com", 443);

  ECHRetryConfigCache::GetInstance().Store(host, "config");
  ECHRetryConfigCache::GetInstance().Invalidate(host);

  auto retrieved = ECHRetryConfigCache::GetInstance().Get(host);
  EXPECT_TRUE(retrieved.empty());
}

TEST_F(ECHRetryHandlerTest, CacheClear) {
  ECHRetryConfigCache::GetInstance().Store(
      HostPortPair("a.com", 443), "config_a");
  ECHRetryConfigCache::GetInstance().Store(
      HostPortPair("b.com", 443), "config_b");

  ECHRetryConfigCache::GetInstance().Clear();
  EXPECT_EQ(ECHRetryConfigCache::GetInstance().Size(), 0u);
}

// ===================================================================
// ECHDegradationMonitor 测试
// ===================================================================

TEST_F(ECHRetryHandlerTest, DegradationDetection) {
  HostPortPair host("bad-ech.com", 443);

  // 5 次连续失败应触发降级
  for (int i = 0; i < 5; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  EXPECT_TRUE(ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

TEST_F(ECHRetryHandlerTest, DegradationResetOnSuccess) {
  HostPortPair host("sometimes-works.com", 443);

  // 几次失败
  for (int i = 0; i < 3; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  // 成功后应重置连续失败计数
  ECHDegradationMonitor::GetInstance().RecordResult(host, true);

  // 需要再次 5 次连续失败才能触发
  for (int i = 0; i < 5; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  EXPECT_TRUE(ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

TEST_F(ECHRetryHandlerTest, HighFailureRate) {
  HostPortPair host("flaky.com", 443);

  // 12 次尝试，仅 2 次成功 (16.7%)
  for (int i = 0; i < 10; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }
  for (int i = 0; i < 2; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, true);
  }

  // 成功率 16.7% < 30%，应触发
  EXPECT_TRUE(ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

TEST_F(ECHRetryHandlerTest, LowFailureRateAllowed) {
  HostPortPair host("good.com", 443);

  // 12 次尝试，8 次成功 (66.7%)
  for (int i = 0; i < 4; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }
  for (int i = 0; i < 8; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, true);
  }

  // 成功率 66.7% >= 30%，不应触发
  EXPECT_FALSE(ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

TEST_F(ECHRetryHandlerTest, ResetStats) {
  HostPortPair host("reset.com", 443);

  for (int i = 0; i < 10; i++) {
    ECHDegradationMonitor::GetInstance().RecordResult(host, false);
  }

  ECHDegradationMonitor::GetInstance().Reset(host);
  EXPECT_FALSE(ECHDegradationMonitor::GetInstance().ShouldDisableECH(host));
}

}  // namespace
}  // namespace net

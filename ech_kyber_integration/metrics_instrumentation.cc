// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// 协议采用率 UMA 指标
// ====================
//
// 以下 UMA 指标用于追踪各网络协议的采用率和性能。
// 在 chrome://histograms 中可查看实时数据。

#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/stringprintf.h"

namespace kiwi_metrics {

// ===================================================================
// ECH 指标
// ===================================================================

// ECH 握手结果 (int, 避免跨 TU 依赖 enum)
//   0: 成功 (ECH accepted)
//   1: 失败 - 需重试
//   2: 失败 - 回退明文 SNI
//   3: 无配置可用
//   4: 功能未启用
void RecordECHResult(int result) {
  base::UmaHistogramExactLinear("Network.Kiwi.ECH.Result", result, 5);
}

// ECH 配置来源 (int)
//   0: DNS HTTPS 记录
//   1: 内置配置
//   2: 用户手动
void RecordECHConfigSource(int source) {
  base::UmaHistogramExactLinear("Network.Kiwi.ECH.ConfigSource", source, 3);
}

// ECH 重试次数 (每次连接)
void RecordECHRetryCount(int count) {
  base::UmaHistogramCounts100(
      "Network.Kiwi.ECH.RetryCount", count);
}

// ECH 握手延迟 (毫秒)
void RecordECHLatency(base::TimeDelta latency) {
  base::UmaHistogramMediumTimes(
      "Network.Kiwi.ECH.Latency", latency);
}

// ===================================================================
// Kyber 指标
// ===================================================================

// 后量子密钥交换使用情况
//   0: 未使用 (传统 X25519)
//   1: 使用 Kyber768 混合
void RecordKyberUsage(bool used) {
  base::UmaHistogramBoolean(
      "Network.Kiwi.Kyber.Used", used);
}

// Kyber 握手延迟增加 (相比传统 X25519)
void RecordKyberLatencyOverhead(base::TimeDelta overhead) {
  base::UmaHistogramMediumTimes(
      "Network.Kiwi.Kyber.LatencyOverhead", overhead);
}

// ===================================================================
// TLS 1.3 指标
// ===================================================================

// TLS 1.3 0-RTT 使用情况
void RecordTLS13EarlyData(bool used) {
  base::UmaHistogramBoolean(
      "Network.Kiwi.TLS13.EarlyData", used);
}

// TLS 版本分布
void RecordTLSVersion(int version) {
  // 0x0304 = TLS 1.3
  // 0x0303 = TLS 1.2
  base::UmaHistogramSparse(
      "Network.Kiwi.TLS.Version", version);
}

// ===================================================================
// HTTP/3 指标
// ===================================================================

// HTTP 协议版本 (int)
//   0: HTTP/1.1  1: HTTP/2  2: HTTP/3  3: QUIC
void RecordHttpProtocol(int protocol) {
  base::UmaHistogramExactLinear(
      "Network.Kiwi.HTTP.Protocol", protocol, 4);
}

// Alt-Svc 来源 (int)
//   0: HTTP 响应头  1: HTTPS DNS 记录  2: 手动配置
void RecordAltSvcSource(int source) {
  base::UmaHistogramExactLinear(
      "Network.Kiwi.AltSvc.Source", source, 3);
}

// ===================================================================
// DNS 指标
// ===================================================================

// DNS 查询类型 (int)
//   0: 仅系统 DNS  1: 仅 DoH  2: DoH + HTTPS 记录
void RecordDnsQueryType(int query_type) {
  base::UmaHistogramExactLinear(
      "Network.Kiwi.DNS.QueryType", query_type, 3);
}

// DNS HTTPS 记录中的参数数量
void RecordHttpsRecordParamCount(int count) {
  base::UmaHistogramCounts100(
      "Network.Kiwi.DNS.HttpsRecordParamCount", count);
}

// ===================================================================
// 综合连接质量评分
// ===================================================================

// 连接安全评分 (0-100)
//   +20: 使用 DoH
//   +30: 使用 ECH
//   +20: 使用 Kyber
//   +15: 使用 HTTP/3
//   +15: TLS 1.3 +
void RecordConnectionSecurityScore(int score) {
  base::UmaHistogramPercentage(
      "Network.Kiwi.Connection.SecurityScore", score);
}

// 计算连接安全评分
int CalculateSecurityScore(bool ech, bool kyber,
                           bool http3, bool doh,
                           int tls_version) {
  int score = 0;
  if (doh)    score += 20;
  if (ech)    score += 30;
  if (kyber)  score += 20;
  if (http3)  score += 15;
  if (tls_version >= 0x0304) score += 15;  // TLS 1.3
  return score;
}

// ===================================================================
// 日活/周活统计
// ===================================================================

// 每日使用 ECH 的主机数
void RecordDailyECHHosts(int count) {
  base::UmaHistogramCounts1000(
      "Network.Kiwi.ECH.DailyHosts", count);
}

// 每周 Kyber 使用率
void RecordWeeklyKyberRate(float rate) {
  base::UmaHistogramPercentage(
      "Network.Kiwi.Kyber.WeeklyRate",
      static_cast<int>(rate * 100));
}

}  // namespace kiwi_metrics

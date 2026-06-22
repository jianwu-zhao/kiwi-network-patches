// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// DNS HTTPS/SVCB 记录 → ECH 配置 集成
// =======================================
//
// 当 DNS 解析返回 HTTPS 记录时，提取 echconfig 参数，
// 并将其传递到 URLRequest 层，最终到达 SSL 连接。
//
// 集成点: net/dns/host_resolver_manager.cc

#include "net/dns/host_resolver_manager.h"

#include <string>
#include <vector>

#include "base/base64.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/base/features.h"
#include "net/dns/public/dns_protocol.h"
#include "net/dns/public/resolve_error_info.h"
#include "net/ssl/ssl_config.h"

namespace net {

// =======================================================================
// HTTPS/SVCB 记录解析器
// =======================================================================

// RFC 9460 HTTPS/SVCB 参数键
namespace svcb_params {
constexpr char kAlpn[] = "alpn";             // ALPN 协议列表
constexpr char kEchConfig[] = "echconfig";    // ECH 配置 (Base64)
constexpr char kIpv4Hint[] = "ipv4hint";      // IPv4 地址提示
constexpr char kIpv6Hint[] = "ipv6hint";      // IPv6 地址提示
constexpr char kPort[] = "port";             // 非标准端口
constexpr char kMandatory[] = "mandatory";    // 必需参数键列表
}  // namespace svcb_params

// HTTPS 记录解析结果
struct NET_EXPORT HttpsRecordResult {
  // ECH 配置 (Base64 解码后的原始 ECHConfigList)
  std::string ech_config;

  // ALPN 协议列表 (如 "h3,h2" 解析为 {"h3", "h2"})
  std::vector<std::string> alpn;

  // IPv4 地址提示
  std::vector<IPAddress> ipv4_hints;

  // IPv6 地址提示
  std::vector<IPAddress> ipv6_hints;

  // 非标准端口
  absl::optional<uint16_t> port;

  // 必需的参数键列表 (如果服务器要求的参数客户端不支持，应放弃使用此记录)
  std::set<std::string> mandatory_keys;

  // 如果需要连接其他域名 (target != ".")
  std::string alternative_service;

  // 是否成功解析
  bool valid = false;
};

// HTTPS/SVCB 记录解析器
//
// 解析 DNS HTTPS 记录的参数，提取连接优化信息。
//
// HTTPS 记录示例:
//   example.com. 3600 IN HTTPS 1 .
//     alpn="h3,h2"
//     echconfig="AAD+DQAq4Q..."
//     ipv4hint="1.2.3.4,5.6.7.8"
//     ipv6hint="2001:db8::1"
//     port=443
class HttpsRecordParser {
 public:
  explicit HttpsRecordParser(const std::string& raw_record)
      : raw_record_(raw_record) {}

  // 解析 HTTPS 记录
  //
  // 返回解析结果，如果记录无效则 result.valid = false
  HttpsRecordResult Parse() {
    HttpsRecordResult result;

    // HTTPS 记录格式 (RFC 9460):
    // priority target-service params...
    //
    // 在 DNS 线缆格式中:
    // uint16 priority
    // ServiceName target
    // SvcParams[0..N]

    CBS cbs;
    CBS_init(&cbs,
             reinterpret_cast<const uint8_t*>(raw_record_.data()),
             raw_record_.size());

    // 读取 priority
    uint16_t priority;
    if (!CBS_get_u16(&cbs, &priority)) {
      DVLOG(1) << "HTTPS: Failed to read priority";
      return result;
    }

    // priority == 0 表示 AliasMode
    if (priority == 0) {
      DVLOG(2) << "HTTPS: AliasMode record, skipping";
      return result;
    }

    // 读取 target (ServiceName)
    // 长度前缀的 DNS 名称
    // "." 表示使用请求的原始域名
    // 其他值表示 ServiceMode 的替代目标
    std::string target;
    if (!ReadServiceName(&cbs, &target)) {
      return result;
    }

    // 如果 target 不是 "."，表示应该使用不同的域名连接
    // 这种情况需要在 DNS 中递归解析
    if (target != ".") {
      result.alternative_service = target;
    }

    // 读取 SvcParams
    while (CBS_len(&cbs) > 0) {
      if (!ParseOneParam(&cbs, &result)) {
        break;
      }
    }

    result.valid = true;

    // 验证 mandatory 参数
    if (!CheckMandatoryParams(result)) {
      DVLOG(1) << "HTTPS: Missing mandatory parameters";
      result.valid = false;
    }

    return result;
  }

 private:
  const std::string& raw_record_;

  // 读取 DNS 名称 (ServiceName)
  // 格式: 长度前缀的标签序列，以空标签结束
  static bool ReadServiceName(CBS* cbs, std::string* out) {
    // 简化的 DNS 名称读取
    // 完整实现需要处理压缩指针等
    std::string name;
    while (CBS_len(cbs) > 0) {
      uint8_t label_len;
      if (!CBS_get_u8(cbs, &label_len)) {
        return false;
      }
      if (label_len == 0) {
        break;  // 根标签
      }
      if (!name.empty()) {
        name += '.';
      }
      const uint8_t* label_data;
      if (!CBS_get_bytes(cbs, &label_data, label_len)) {
        return false;
      }
      name.append(reinterpret_cast<const char*>(label_data), label_len);
    }

    *out = name.empty() ? "." : name;
    return true;
  }

  // 解析一个 SVCB 参数
  //
  // 格式:
  //   uint16 key
  //   uint16 value_length
  //   uint8 value[value_length]
  static bool ParseOneParam(CBS* cbs, HttpsRecordResult* result) {
    if (CBS_len(cbs) < 4)
      return false;

    uint16_t key;
    uint16_t value_length;

    if (!CBS_get_u16(cbs, &key) ||
        !CBS_get_u16(cbs, &value_length)) {
      return false;
    }

    if (CBS_len(cbs) < value_length) {
      return false;
    }

    const uint8_t* value_data;
    if (!CBS_get_bytes(cbs, &value_data, value_length)) {
      return false;
    }

    std::string value(reinterpret_cast<const char*>(value_data),
                      value_length);

    // 根据 key 处理参数
    switch (key) {
      case dns_protocol::kSvcParamKeyMandatory:
        ParseMandatoryParam(value, result);
        break;

      case dns_protocol::kSvcParamKeyAlpn:
        ParseAlpnParam(value, result);
        break;

      case dns_protocol::kSvcParamKeyEchConfig:
        ParseEchConfigParam(value, result);
        break;

      case dns_protocol::kSvcParamKeyIpv4Hint:
        ParseIpv4HintParam(value, result);
        break;

      case dns_protocol::kSvcParamKeyIpv6Hint:
        ParseIpv6HintParam(value, result);
        break;

      case dns_protocol::kSvcParamKeyPort:
        ParsePortParam(value, result);
        break;

      default:
        DVLOG(3) << "HTTPS: Unknown param key " << key
                 << " (value=" << value.size() << " bytes)";
        break;
    }

    return true;
  }

  // == 各参数解析器 ==

  static void ParseMandatoryParam(const std::string& value,
                                  HttpsRecordResult* result) {
    // mandatory 参数的值是以逗号分隔的键 ID 列表
    // 每个键 ID 是 2 字节大端整数
    for (size_t i = 0; i + 1 < value.size(); i += 2) {
      uint16_t key = (static_cast<uint8_t>(value[i]) << 8) |
                     static_cast<uint8_t>(value[i + 1]);
      result->mandatory_keys.insert(
          dns_protocol::SvcParamKeyToString(
              static_cast<dns_protocol::SvcParamKey>(key)));
    }
  }

  static void ParseAlpnParam(const std::string& value,
                             HttpsRecordResult* result) {
    // ALPN 参数是协议 ID 列表，每个前有一个长度字节
    size_t offset = 0;
    while (offset < value.size()) {
      uint8_t len = static_cast<uint8_t>(value[offset]);
      if (offset + 1 + len > value.size())
        break;

      std::string alpn = value.substr(offset + 1, len);
      if (!alpn.empty()) {
        result->alpn.push_back(alpn);
        DVLOG(2) << "HTTPS ALPN: " << alpn;
      }
      offset += 1 + len;
    }
  }

  static void ParseEchConfigParam(const std::string& value,
                                  HttpsRecordResult* result) {
    // ECH 配置已经是原始 ECHConfigList 格式
    result->ech_config = value;
    DVLOG(2) << "HTTPS ECH: " << value.size() << " bytes";
  }

  static void ParseIpv4HintParam(const std::string& value,
                                 HttpsRecordResult* result) {
    // IPv4 地址提示: 每个地址 4 字节
    for (size_t i = 0; i + 3 < value.size(); i += 4) {
      IPAddress addr(
          reinterpret_cast<const uint8_t*>(value.data() + i), 4);
      if (addr.IsValid()) {
        result->ipv4_hints.push_back(addr);
      }
    }
  }

  static void ParseIpv6HintParam(const std::string& value,
                                 HttpsRecordResult* result) {
    // IPv6 地址提示: 每个地址 16 字节
    for (size_t i = 0; i + 15 < value.size(); i += 16) {
      IPAddress addr(
          reinterpret_cast<const uint8_t*>(value.data() + i), 16);
      if (addr.IsValid()) {
        result->ipv6_hints.push_back(addr);
      }
    }
  }

  static void ParsePortParam(const std::string& value,
                            HttpsRecordResult* result) {
    if (value.size() >= 2) {
      result->port = (static_cast<uint8_t>(value[0]) << 8) |
                      static_cast<uint8_t>(value[1]);
    }
  }

  // 检查客户端是否支持所有 mandatory 参数
  static bool CheckMandatoryParams(const HttpsRecordResult& result) {
    static const std::set<std::string> supported_keys = {
        "alpn", "echconfig", "ipv4hint", "ipv6hint", "port"
    };

    for (const auto& key : result.mandatory_keys) {
      if (supported_keys.find(key) == supported_keys.end()) {
        DVLOG(1) << "HTTPS: Unsupported mandatory key: " << key;
        return false;
      }
    }
    return true;
  }
};

// =======================================================================
// HostResolverManager 集成
// =======================================================================

// 在 DNS 解析完成后，处理 HTTPS 记录结果
//
// 此函数应在以下位置调用:
//   HostResolverManager::FinishDnsTransaction() 或
//   HostResolverManager::ProcessDnsResults()
//
// 处理流程:
//   1. 检查 DnsResponse 中是否有 HTTPS 记录
//   2. 解析 HTTPS 记录参数
//   3. 提取 ECH 配置，存储在 ResolveContext 中
//   4. 根据 ALPN 标记 HTTP/3 支持
//   5. 应用 IP 地址提示 (可跳过后续 A/AAAA 查询)
void ProcessHttpsRecord(const HostCache::Entry& results,
                        const NetworkAnonymizationKey& nik,
                        ResolveContext* context) {
  if (!base::FeatureList::IsEnabled(features::kUseDnsHttpsSvcb))
    return;

  if (!results.https_records())
    return;

  for (const auto& record : *results.https_records()) {
    // 解析 HTTPS 记录
    HttpsRecordParser parser(record);
    auto parsed = parser.Parse();

    if (!parsed.valid)
      continue;

    // 1. 存储 ECH 配置
    if (!parsed.ech_config.empty()) {
      context->SetEchConfig(nik, parsed.ech_config);
      DVLOG(1) << "HTTPS: Stored ECH config (" 
               << parsed.ech_config.size() << " bytes) for NIK";
    }

    // 2. 标记 HTTP/3 支持
    bool supports_h3 = false;
    for (const auto& alpn : parsed.alpn) {
      if (alpn == "h3") {
        supports_h3 = true;
        break;
      }
    }
    if (supports_h3) {
      context->SetSupportsHttp3(nik, true);
      DVLOG(1) << "HTTPS: Server supports HTTP/3";
    }

    // 3. 应用 IP 地址提示
    if (!parsed.ipv4_hints.empty() || !parsed.ipv6_hints.empty()) {
      std::vector<IPAddress> all_hints;
      all_hints.insert(all_hints.end(),
                       parsed.ipv4_hints.begin(),
                       parsed.ipv4_hints.end());
      all_hints.insert(all_hints.end(),
                       parsed.ipv6_hints.begin(),
                       parsed.ipv6_hints.end());
      context->SetAddressHints(nik, all_hints);
      DVLOG(1) << "HTTPS: Applied " << all_hints.size() << " address hints";
    }

    // 4. 应用端口提示
    if (parsed.port.has_value()) {
      context->SetAlternativePort(nik, parsed.port.value());
    }

    // 只处理第一条有效的 HTTPS 记录
    break;
  }
}

// =======================================================================
// HTTP → HTTPS 升级 (基于 HTTPS 记录)
// =======================================================================

// 当请求使用 HTTP 协议但 DNS 中存在 HTTPS 记录时，
// 应该将请求升级到 HTTPS。
//
// 实现位置:
//   URLRequestHttpJob::Start() 中检查
bool ShouldUpgradeHttpToHttps(const HostCache::Entry& dns_results) {
  if (!base::FeatureList::IsEnabled(features::kUseDnsHttpsSvcb))
    return false;

  if (!dns_results.https_records())
    return false;

  for (const auto& record : *dns_results.https_records()) {
    HttpsRecordParser parser(record);
    auto parsed = parser.Parse();

    if (parsed.valid) {
      // HTTPS 记录存在表示服务器支持 HTTPS
      // 除非记录显式拒绝 (alpn 为空表示不升级)
      if (!parsed.alpn.empty()) {
        return true;
      }
    }
  }

  return false;
}

}  // namespace net

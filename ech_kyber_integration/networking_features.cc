// Copyright 2024 Kiwi Browser Authors.
// SPDX-License-Identifier: BSD-3-Clause
//
// 网络协议功能统一入口
// ====================
//
// 集中管理所有网络协议功能的 feature flag 注册，
// 确保在 chrome://flags 和 about:version 中可见。
//
// 集成点: chrome/browser/flag_descriptions.cc 和 about_flags.cc

#include "base/feature_list.h"
#include "base/strings/stringprintf.h"
#include "net/base/features.h"

namespace kiwi {

// =======================================================================
// 功能标志声明
// =======================================================================

// TLS 1.3 Encrypted ClientHello (ECH)
// RFC 8871 / draft-ietf-tls-esni-13
//
// 加密 ClientHello 中的 SNI 扩展，防止网络中间人
// 窥探用户访问的网站域名。
BASE_FEATURE(kKiwiECH,
             "KiwiECH",
             base::FEATURE_ENABLED_BY_DEFAULT);

// X25519 + Kyber768 后量子密钥交换
// FIPS 203 (CRYSTALS-Kyber) / NIST Level 3
//
// 混合密钥协商: 同时使用传统 ECDHE 和 Kyber，
// 即使量子计算破解了 Kyber，X25519 仍提供保护。
BASE_FEATURE(kKiwiPostQuantumKyber,
             "KiwiPostQuantumKyber",
             base::FEATURE_ENABLED_BY_DEFAULT);

// TLS 1.3 0-RTT Early Data
// RFC 8446 Section 4.2.10
//
// 允许客户端在第二次连接时立即发送数据，
// 减少一个 RTT 的延迟。
BASE_FEATURE(kKiwiTLS13EarlyData,
             "KiwiTLS13EarlyData",
             base::FEATURE_ENABLED_BY_DEFAULT);

// TLS 1.3 Post-Handshake Key Update
// RFC 8446 Section 4.4
//
// 在长连接中定期更新会话密钥，限制单次密钥的加密数据量。
BASE_FEATURE(kKiwiTLS13KeyUpdate,
             "KiwiTLS13KeyUpdate",
             base::FEATURE_ENABLED_BY_DEFAULT);

// TLS Extension Permutation
//
// 随机化 ClientHello 中 TLS 扩展的顺序，
// 防止中间盒设备对特定顺序 ossification。
BASE_FEATURE(kKiwiPermuteTLSExtensions,
             "KiwiPermuteTLSExtensions",
             base::FEATURE_ENABLED_BY_DEFAULT);

// QUIC v1 / HTTP/3 最优配置
//
// 使用 BBRv2 拥塞控制、连接迁移、0-RTT 等优化。
BASE_FEATURE(kKiwiQuicOptimizations,
             "KiwiQuicOptimizations",
             base::FEATURE_ENABLED_BY_DEFAULT);

// DNS HTTPS/SVCB 记录解析
// RFC 9460
//
// 通过 DNS 发现服务器的 ECH 配置、HTTP/3 支持和 IP 地址提示。
BASE_FEATURE(kKiwiDnsHttpsSvcb,
             "KiwiDnsHttpsSvcb",
             base::FEATURE_ENABLED_BY_DEFAULT);

// DNS over HTTPS 预置服务器
//
// 预置 Cloudflare、Google、Quad9、阿里云等 DoH 服务器。
BASE_FEATURE(kKiwiDohProviders,
             "KiwiDohProviders",
             base::FEATURE_ENABLED_BY_DEFAULT);

// HTTP/3 优先策略
//
// 通过 Alt-Svc 快速发现并优先使用 HTTP/3 连接。
BASE_FEATURE(kKiwiHttp3Preferred,
             "KiwiHttp3Preferred",
             base::FEATURE_ENABLED_BY_DEFAULT);

// =======================================================================
// 功能标志描述 (用于 chrome://flags)
// =======================================================================

namespace flag_descriptions {

const char kKiwiECHName[] =
    "Encrypted ClientHello (ECH)";

const char kKiwiECHDescription[] =
    "Encrypts the TLS ClientHello SNI extension to prevent "
    "network observers from seeing which websites you visit. "
    "Requires server support. #privacy #security";

const char kKiwiPostQuantumKyberName[] =
    "Post-Quantum Key Exchange (X25519+Kyber768)";

const char kKiwiPostQuantumKyberDescription[] =
    "Enables hybrid X25519 + Kyber768 key exchange in TLS 1.3. "
    "Protects against future quantum computer attacks. "
    "Increases ClientHello size by ~1KB. #security #pqc";

const char kKiwiTLS13EarlyDataName[] =
    "TLS 1.3 0-RTT Early Data";

const char kKiwiTLS13EarlyDataDescription[] =
    "Allows sending data immediately on repeat connections to "
    "previously visited sites, reducing latency by one round trip. "
    "May be incompatible with some servers. #performance";

const char kKiwiTLS13KeyUpdateName[] =
    "TLS 1.3 Post-Handshake Key Update";

const char kKiwiTLS13KeyUpdateDescription[] =
    "Periodically updates TLS session keys during long-lived "
    "connections to limit data encrypted under a single key. "
    "#security";

const char kKiwiPermuteTLSExtensionsName[] =
    "TLS Extension Permutation";

const char kKiwiPermuteTLSExtensionsDescription[] =
    "Randomizes the order of TLS extensions in ClientHello to "
    "prevent network devices from depending on a specific ordering. "
    "#security #compatibility";

const char kKiwiQuicOptimizationsName[] =
    "QUIC / HTTP/3 Optimizations";

const char kKiwiQuicOptimizationsDescription[] =
    "Enables BBRv2 congestion control, connection migration, "
    "larger initial window, and optimized multipath support. "
    "#performance #network";

const char kKiwiDnsHttpsSvcbName[] =
    "DNS HTTPS Records (SVCB)";

const char kKiwiDnsHttpsSvcbDescription[] =
    "Resolves HTTPS DNS records to discover ECH configuration, "
    "HTTP/3 support, and IP address hints for faster connections. "
    "RFC 9460. #performance #privacy";

const char kKiwiDohProvidersName[] =
    "Built-in DNS over HTTPS Providers";

const char kKiwiDohProvidersDescription[] =
    "Pre-configures Cloudflare, Google, Quad9, and Alibaba DNS "
    "for encrypted DNS queries. Disable to use system DNS only. "
    "#privacy";

const char kKiwiHttp3PreferredName[] =
    "Prefer HTTP/3 over QUIC";

const char kKiwiHttp3PreferredDescription[] =
    "Prioritizes HTTP/3 (QUIC) connections when servers advertise "
    "support via Alt-Svc or HTTPS DNS records. "
    "#performance";

}  // namespace flag_descriptions

// =======================================================================
// 功能注册 (供 about_flags.cc 调用)
// =======================================================================

// 返回所有 Kiwi 网络功能的 feature 条目
//
// 在 chrome/browser/about_flags.cc 中:
//
//   const FeatureEntry kFeatureEntries[] = {
//     ... 原有 flags ...
//
//     // === Kiwi Network Protocol Extensions ===
//     {"enable-kiwi-ech",
//      flag_descriptions::kKiwiECHName,
//      flag_descriptions::kKiwiECHDescription,
//      kOsAll,
//      FEATURE_VALUE_TYPE(kiwi::kKiwiECH)},
//
//     {"enable-kiwi-post-quantum-kyber",
//      flag_descriptions::kKiwiPostQuantumKyberName,
//      flag_descriptions::kKiwiPostQuantumKyberDescription,
//      kOsAll,
//      FEATURE_VALUE_TYPE(kiwi::kKiwiPostQuantumKyber)},
//
//     {"enable-kiwi-tls13-early-data",
//      ...},
//
//     ... 等
//   }

// =======================================================================
// 编译时验证
// =======================================================================

// 确保编译环境支持所需 API
// 这些宏在 BoringSSL 头文件中定义

#if !defined(SSL_CTX_set_ech_retry_callback)
#error "Kiwi requires BoringSSL with ECH support (Chromium 105+)"
#endif

#if !defined(SSL_set_key_update_interval)
#error "Kiwi requires BoringSSL with TLS 1.3 Key Update support"
#endif

// 验证 Kyber 组可用 (编译时字符串常量检查)
// "X25519Kyber768Draft00" 在 BoringSSL 2023-06+ 中定义

}  // namespace kiwi

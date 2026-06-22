# Kiwi Browser - 最新网络协议适配方案

## 概述

将 Kiwi Browser (基于 Chromium 105) 修改以支持最新网络协议。
由于 Kiwi 项目已归档（2025年1月），本方案提供直接可用的代码补丁。

## 修改内容

### 1. 核心网络协议启用 (`net/base/features.cc`)

| 协议 | 功能 | 状态 |
|------|------|------|
| **TLS Encrypted ClientHello (ECH)** | 加密 SNI，防止网络嗅探 | 改为默认启用 |
| **TLS 1.3 0-RTT Early Data** | 减少连接延迟 | 改为默认启用 |
| **TLS 1.3 Key Update** | 定期更新会话密钥 | 改为默认启用 |
| **TLS Extension Permutation** | 随机化扩展顺序，防 ossification | 改为默认启用 |
| **Post-Quantum CECPQ2 (Kyber)** | 抗量子计算加密 | 改为默认启用 |
| **DNS HTTPS Records (SVCB)** | 通过 DNS 获取 HTTPS 连接参数 | 改为默认启用 |
| **DNS SVCB ALPN** | 从 DNS 获取协议协商信息 | 改为默认启用 |
| **TCP Connect Timeout** | 智能 TCP 连接超时 | 改为默认启用 |
| **Network Buffer Optimization** | 减少内存拷贝，提升性能 | 改为默认启用 |

## 使用方式

### 方案 A：本地编译（需要强大机器）

```bash
git clone -b kiwi https://github.com/kiwibrowser/src.next.git
cd src.next
bash /path/to/apply_kiwi_patches.sh
# 然后 gn gen + ninja
```

### 方案 B：GitHub Actions 云编译（推荐）

参见 `github-actions/SETUP.md`，复制 `github-actions/build-kiwi.yml` 到你的仓库即可。

---

## 修改内容

### 1. 核心网络协议启用 (`net/base/features.cc`)

| 协议 | 功能 | 状态 |
|------|------|------|
| **TLS Encrypted ClientHello (ECH)** | 加密 SNI，防止网络嗅探 | 改为默认启用 |
| **TLS 1.3 0-RTT Early Data** | 减少连接延迟 | 改为默认启用 |
| **TLS 1.3 Key Update** | 定期更新会话密钥 | 改为默认启用 |
| **TLS Extension Permutation** | 随机化扩展顺序，防 ossification | 改为默认启用 |
| **Post-Quantum CECPQ2 (Kyber)** | 抗量子计算加密 | 改为默认启用 |
| **DNS HTTPS Records (SVCB)** | 通过 DNS 获取 HTTPS 连接参数 | 改为默认启用 |
| **DNS SVCB ALPN** | 从 DNS 获取协议协商信息 | 改为默认启用 |
| **TCP Connect Timeout** | 智能 TCP 连接超时 | 改为默认启用 |
| **Network Buffer Optimization** | 减少内存拷贝，提升性能 | 改为默认启用 |

### 2. QUIC / HTTP/3 (`net/features.gni`)

- `enable_quic = true` — 强制启用 QUIC
- `enable_http3 = true` — 启用 HTTP/3

### 3. 新增实验 Flag (`chrome/browser/about_flags.cc`)

在 `chrome://flags` 页面新增以下用户可控选项：

- `enable-quic-proactive` — QUIC 协议开关
- `enable-encrypted-client-hello` — ECH 加密 SNI
- `enable-dns-https-svcb` — DNS HTTPS 记录
- `enable-post-quantum-kyber` — 抗量子加密

### 4. DNS over HTTPS (DoH) 增强

在 `chrome/browser/net/system_network_context_manager.cc` 中配置预置 DoH 服务器：

```cpp
// 在 ConfigureDefaultNetworkContextParams 中添加
params->dns_over_https_servers = {
    // Cloudflare
    {"https://cloudflare-dns.com/dns-query", true},
    // Google
    {"https://dns.google/dns-query", true},
    // Quad9
    {"https://dns.quad9.net/dns-query", true},
    // Alibaba
    {"https://dns.alidns.com/dns-query", true},
};
```

### 5. IPv6 优先策略

在 `net/base/address_tracker_linux.cc` 中配置 IPv6 优先：

```cpp
// 设置 IPv6 优先于 IPv4
const int kIPv6Preference = 1;  // 0=默认, 1=IPv6优先
```

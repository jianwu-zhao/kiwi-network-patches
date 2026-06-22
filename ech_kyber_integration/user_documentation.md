# Kiwi Browser 新一代网络协议 - 用户文档

## 概述

Kiwi Browser 已集成最新的互联网协议和安全技术，
提供更快、更安全、更私密的浏览体验。

## 🔒 隐私保护

### Encrypted ClientHello (ECH)

**作用**: 加密 TLS 握手中的 SNI (Server Name Indication)，
防止网络运营商、WiFi 热点、防火墙看到你访问的网站域名。

**效果**:
```
传统 TLS:  ClientHello → [SNI: example.com] → 明文可见
ECH 加密:  ClientHello → [Encrypted] → 中间人无法查看域名
```

**启用**: 默认开启 (`chrome://flags/#enable-encrypted-client-hello`)
**要求**: 服务器需部署 ECH (Cloudflare、Fastly 等 CDN 支持)
**回退**: 如果服务器不支持，自动降级到传统 TLS

### DNS over HTTPS (DoH)

**作用**: 通过加密 HTTPS 连接查询 DNS，防止 DNS 劫持和嗅探。

**预置服务器**:
| 提供商 | 地址 | 特点 |
|--------|------|------|
| Cloudflare | `https://cloudflare-dns.com/dns-query` | 最快，不记录 IP |
| Google | `https://dns.google/dns-query` | 稳定，全球覆盖 |
| Quad9 | `https://dns.quad9.net/dns-query` | 安全过滤 |
| 阿里云 | `https://dns.alidns.com/dns-query` | 中国地区优化 |

**设置**: `设置 → 安全与隐私 → 安全 DNS`

## 🚀 性能提升

### HTTP/3 (QUIC)

**作用**: 使用 UDP 代替 TCP，减少连接延迟。

**优势**:
- 0-RTT 快速重连 (第二次访问无需等待握手)
- 连接迁移 (WIFI 切 5G 不中断)
- 多路复用无队头阻塞
- BBRv2 拥塞控制适应高延迟网络

**启用**: 默认开启，优先使用。
**检测**: 地址栏左侧锁图标 → 连接信息 → "HTTP/3"

### DNS HTTPS/SVCB 记录

**作用**: 通过 DNS 一次性获取连接所需的所有信息。

包含:
- ✅ ECH 配置 (加密 SNI)
- ✅ HTTP/3 支持 (直接使用 QUIC)
- ✅ IP 地址 (跳过 A/AAAA 查询)
- ✅ 非标准端口

**效果**: 减少 1-2 次网络往返，连接速度提升 30-50%。

## 🛡️ 未来安全

### 后量子加密 (X25519 + Kyber768)

**作用**: 在 TLS 握手中使用混合密钥交换，
抵御未来量子计算机对传统加密的破解。

**标准**: FIPS 203 (CRYSTALS-Kyber)，NIST Level 3
**强度**: 与 AES-192 相当
**模式**: X25519 + Kyber768 混合 (传统+PQC双保险)

**启用**: 默认开启
**兼容性**: 需要 BoringSSL 2023-06+ (Chromium 105+)

## ⚙️ 配置选项

### chrome://flags

| 实验选项 | 默认 | 说明 |
|----------|------|------|
| `#enable-kiwi-ech` | 启用 | 加密 ClientHello SNI |
| `#enable-kiwi-post-quantum-kyber` | 启用 | 后量子密钥交换 |
| `#enable-kiwi-tls13-early-data` | 启用 | TLS 1.3 0-RTT |
| `#enable-kiwi-tls13-key-update` | 启用 | TLS 定期密钥更新 |
| `#enable-kiwi-permute-tls-ext` | 启用 | TLS 扩展排列 |
| `#enable-kiwi-quic-optimizations` | 启用 | QUIC 性能优化 |
| `#enable-kiwi-dns-https-svcb` | 启用 | HTTPS DNS 记录 |
| `#enable-kiwi-doh` | 启用 | DNS over HTTPS |
| `#enable-kiwi-http3-preferred` | 启用 | HTTP/3 优先 |

### 安全配置模式

在 `chrome://settings/security` 中选择:

| 模式 | 隐私 | 性能 | 兼容性 |
|------|------|------|--------|
| **平衡** (默认) | ✅ 高 | ✅ 优 | ✅ 好 |
| **安全优先** | ✅✅ 最高 | ✅ 良 | ⚠️ 部分站点可能不兼容 |
| **兼容优先** | ⚠️ 基础 | ✅ 良 | ✅✅ 最好 |

## 📊 查看统计

在 `chrome://histograms` 搜索以下指标:

| 指标 | 说明 |
|------|------|
| `Network.Kiwi.ECH.Result` | ECH 握手成功率 |
| `Network.Kiwi.Kyber.Used` | 后量子加密使用率 |
| `Network.Kiwi.HTTP.Protocol` | HTTP 协议版本分布 |
| `Network.Kiwi.DNS.QueryType` | DNS 查询类型统计 |
| `Network.Kiwi.Connection.SecurityScore` | 连接安全评分 |

## ❓ 故障排查

### ECH 不工作

1. 检查服务器是否支持 ECH
   ```
   curl --ech-devel https://example.com
   ```
2. 查看 `chrome://net-export` 的 ECH 日志
3. 尝试在 `chrome://flags` 中禁用再启用

### HTTP/3 连接慢

1. 某些防火墙会限速 UDP 流量
2. 尝试在 flags 中禁用 QUIC 优化
3. 如果使用 VPN，检查 VPN 是否支持 UDP

### 网站兼容性问题

1. 尝试切换到"兼容优先"模式
2. 在 `chrome://flags` 中逐个禁用协议
3. 该网站可能使用了过时的 TLS 配置

## 🔬 开发者信息

```
网络日志: chrome://net-export
协议统计: chrome://histograms
实时状态: chrome://net-internals/#quic
TLS 信息: chrome://net-internals/#ssl
DNS 信息: chrome://net-internals/#dns
```

---

*Kiwi Browser 网络协议扩展 v1.0 - 基于 Chromium 105+*

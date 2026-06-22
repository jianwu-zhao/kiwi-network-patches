# ECH + Kyber 端到端集成实现

本目录包含 ECH (Encrypted ClientHello) 和 Kyber 后量子加密的
完整生产级 C++ 实现代码，覆盖 DNS → TLS → HTTP 全链路。

## 文件结构

```
ech_kyber_integration/
├── README.md
├── ssl_config.h                 ← SSLConfig 扩展 (ech_config_list)
├── ssl_client_socket.cc         ← SSL 连接 ECH + Kyber 实现
├── host_resolver_manager.cc     ← DNS HTTPS 记录 → ECH 配置
├── url_request_http_job.cc      ← URLRequest 传递 ECH 配置到 SSL
├── system_network_context_manager.cc ← DoH + SVCB 全局配置
├── ech_retry_handler.cc         ← ECH 重试逻辑 (服务器返回 retry_config)
├── quic_ech_integration.cc      ← QUIC 连接的 ECH 支持
├── BUILD.gn                     ← 编译配置
└── networking_features.cc      ← 统一 feature 入口
```

## 数据流

```
DNS 解析 (HostResolverManager)
  │
  ├── HTTPS/SVCB 记录查询
  │     ├── echconfig -> SSLConfig.ech_config_list
  │     ├── alpn -> {"h3", "h2", "h1"}
  │     └── ipv4hint/ipv6hint -> 地址提示
  │
  ├── 普通 A/AAAA 查询
  │     └── 地址列表
  │
  └── 结果合并
        │
        ▼
URLRequestHttpJob
  │
  ├── 创建 SSLConfig
  │     └── ssl_config.ech_config_list = dns_result.ech_config
  │
  └── HTTP 流协商
        ├── h3 -> QUIC (HTTP/3)
        └── h2/h1 -> TCP + TLS (HTTP/2 或 HTTP/1.1)
              │
              ▼
        SSLClientSocket::Connect()
          │
          ├── 1. ConfigureECHForConnection()
          │     ├── SSL_set_ech_keys()       ← 设置 ECH 公钥
          │     ├── SSL_set_ech_grease_enabled() ← GREASE
          │     └── SSL_CTX_set_ech_retry_callback()
          │
          ├── 2. ConfigurePostQuantumKeys()
          │     └── SSL_CTX_set1_groups_list("X25519Kyber768Draft00:X25519")
          │
          ├── 3. ConfigureTLS13EarlyData()
          │     └── SSL_set_early_data_enabled() + SSL_set_max_early_data_size()
          │
          ├── 4. ConfigureTLSExtensionPermutation()
          │     └── SSL_CTX_set_permute_extensions()
          │
          └── 5. TLS 1.3 Handshake
                ├── 成功 -> HTTP/2 或 HTTP/1.1 数据传输
                └── ECH 重试 -> ECHRetryHandler
                      └── 使用 retry_config 重新握手
```

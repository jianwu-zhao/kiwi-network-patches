// Copyright 2024 Kiwi Browser Authors.  SPDX-License-Identifier: BSD-3-Clause
#ifndef NET_KIWI_SSL_CONFIG_H_
#define NET_KIWI_SSL_CONFIG_H_

#include "net/ssl/ssl_config.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {
enum class KiwiSSLProfile { kBalanced, kSecure, kCompatible };
void ConfigureKiwiSSLProfile(SSLConfig*, KiwiSSLProfile = KiwiSSLProfile::kBalanced);
int  ConfigureKiwiSSL(SSL* ssl, SSL_CTX* ctx, const SSLConfig& ssl_config);
void AnalyzeKiwiSSLResult(const SSL* ssl, SSLInfo* ssl_info);
}  // namespace net
#endif

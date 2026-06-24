// Copyright 2024 Kiwi Browser Authors.  SPDX-License-Identifier: BSD-3-Clause
#ifndef NET_KIWI_URL_REQUEST_H_
#define NET_KIWI_URL_REQUEST_H_

namespace net {
class URLRequest;
struct SSLConfig;
void ApplyEchConfigToRequest(URLRequest* request, SSLConfig* ssl_config);
std::string GetEchConfigForRequest(const URLRequest* request);
}  // namespace net
#endif

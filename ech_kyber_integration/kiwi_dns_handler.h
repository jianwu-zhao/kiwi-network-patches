// Copyright 2024 Kiwi Browser Authors.  SPDX-License-Identifier: BSD-3-Clause
#ifndef NET_KIWI_DNS_HANDLER_H_
#define NET_KIWI_DNS_HANDLER_H_

#include "net/base/host_port_pair.h"
namespace net {
class HostCache;
class NetworkAnonymizationKey;
class ResolveContext;
void ProcessHttpsRecord(const HostCache::Entry& results,
                        const NetworkAnonymizationKey& nik,
                        ResolveContext* context);
bool ShouldUpgradeHttpToHttps(const HostCache::Entry& dns_results);
}  // namespace net
#endif

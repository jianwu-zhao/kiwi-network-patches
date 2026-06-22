// Copyright 2024 Kiwi Browser Authors.  SPDX-License-Identifier: BSD-3-Clause
#ifndef NET_KIWI_SYSTEM_NET_H_
#define NET_KIWI_SYSTEM_NET_H_

namespace network { namespace mojom { struct NetworkContextParams; } }
namespace chrome_browser_net {
void AddKiwiDohProviders(network::mojom::NetworkContextParams* params);
void ConfigureKiwiNetworkFeatures(class PrefService* local_state);
}  // namespace chrome_browser_net
#endif

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/test/test_network_context.h"

#include <utility>

namespace network {

// Out-of-line so that the no-op implementations are not inline virtual bodies
// in the header (chromium-style).

void TestNetworkContext::Retire(
    mojo::PendingReceiver<mojom::NetworkContext> completion,
    RetireCallback callback) {
  std::move(callback).Run({});
}

void TestNetworkContext::AcknowledgeStorageHandoff(
    AcknowledgeStorageHandoffCallback callback) {
  std::move(callback).Run();
}

void TestNetworkContext::GetStorageHandoffBoundCountForTesting(
    GetStorageHandoffBoundCountForTestingCallback callback) {
  std::move(callback).Run(0);
}

void TestNetworkContext::PrepareStorageHandoff(
    const base::UnguessableToken& transaction,
    mojom::StorageHandoffMemberPtr member,
    mojo::PendingReceiver<mojom::NetworkContext> workload,
    PrepareStorageHandoffCallback callback) {
  std::move(callback).Run(false);
}

}  // namespace network

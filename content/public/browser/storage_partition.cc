// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/storage_partition.h"

#include <utility>

namespace content {

NetworkContextHandoffProof::NetworkContextHandoffProof(
    BrowserContext* context, StoragePartition* participant,
    const base::UnguessableToken& attempt, uint64_t generation,
    base::RepeatingCallback<bool()> owns_handoff)
    : context_(context), participant_(participant),
      owns_handoff_(std::move(owns_handoff)), attempt_(attempt),
      generation_(generation) {}
NetworkContextHandoffProof::NetworkContextHandoffProof(
    NetworkContextHandoffProof&& other)
    : context_(std::exchange(other.context_, nullptr)),
      participant_(std::exchange(other.participant_, nullptr)),
      owns_handoff_(std::move(other.owns_handoff_)),
      attempt_(std::exchange(other.attempt_, base::UnguessableToken())),
      generation_(std::exchange(other.generation_, 0)),
      bound_(std::exchange(other.bound_, false)) {}
NetworkContextHandoffProof& NetworkContextHandoffProof::operator=(
    NetworkContextHandoffProof&& other) {
  if (this != &other) {
    context_ = std::exchange(other.context_, nullptr);
    participant_ = std::exchange(other.participant_, nullptr);
    owns_handoff_ = std::move(other.owns_handoff_);
    attempt_ = std::exchange(other.attempt_, base::UnguessableToken());
    generation_ = std::exchange(other.generation_, 0);
    bound_ = std::exchange(other.bound_, false);
  }
  return *this;
}
NetworkContextHandoffProof::~NetworkContextHandoffProof() = default;
bool NetworkContextHandoffProof::IsValidFor(
    BrowserContext* context, StoragePartition* participant) const {
  return context == context_ && participant == participant_ && owns_handoff_ &&
         owns_handoff_.Run();
}
bool NetworkContextHandoffProof::Bind(
    const base::UnguessableToken& attempt, uint64_t generation) {
  if (bound_ || attempt.is_empty() || attempt_ != attempt ||
      generation_ != generation || !generation)
    return false;
  bound_ = true;
  return true;
}
bool NetworkContextHandoffProof::AuthorizesConfiguration(
    BrowserContext* context, StoragePartition* participant) const {
  if (!bound_ || !IsValidFor(context, participant))
    return false;
  auto current = participant->GetNetworkContextHandoffConstructionMember();
  return current && current->attempt == attempt_ &&
         current->generation == generation_ + 1;
}
bool NetworkContextHandoffProof::IsBoundTo(
    const base::UnguessableToken& attempt, uint64_t generation) const {
  return bound_ && !attempt_.is_empty() && attempt_ == attempt && generation_ == generation;
}

// Out-of-line defaults. A partition that takes no part in a network context
// handoff has no handoff state, stages nothing and restores nothing; the
// StoragePartitionImpl overrides the ones it implements.

std::shared_ptr<NetworkContextHandoffState>
StoragePartition::GetNetworkContextHandoffState() const {
  return nullptr;
}

std::shared_ptr<NetworkContextHandoffState>
StoragePartition::BeginNetworkContextHandoffState() {
  return GetNetworkContextHandoffState();
}

void StoragePartition::StageNetworkContextHandoff(
    NetworkContextHandoffProof proof,
    base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(false);
}

void StoragePartition::FenceNetworkContextHandoff(
    base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(false);
}

network::mojom::StorageHandoffMemberPtr
StoragePartition::GetNetworkContextHandoffConstructionMember() const {
  return nullptr;
}

bool StoragePartition::IsNetworkContextHandoffRestored() const {
  return false;
}

network::mojom::StorageHandoffMemberPtr
StoragePartition::GetNetworkContextHandoffMember() const {
  return nullptr;
}

void StoragePartition::SetNetworkContextHandoffTransaction(
    const base::UnguessableToken& transaction) {}

void StoragePartition::SetNetworkContextHandoffReleasePossible(bool possible) {}

void StoragePartition::ActivateNetworkContextHandoff(
    base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(false);
}

void StoragePartition::FinalizeNetworkContextHandoff(bool success) {}

void StoragePartition::AbortNetworkContextHandoff(
    base::OnceClosure callback) {
  std::move(callback).Run();
}

}  // namespace content

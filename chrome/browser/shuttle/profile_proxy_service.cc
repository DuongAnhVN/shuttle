// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/profile_proxy_service.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_partition.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/proxy_config_with_annotation.mojom.h"

namespace shuttle {

namespace {

// The stored entry of a persistent agent, or nullptr for a throwaway one.
const base::Value* FindStoredAgent(const PrefService* pref_service,
                                   std::string_view id) {
  return pref_service->GetDict(prefs::kProfiles).Find(id);
}

// The agent's storage partition, or nullptr if it was never created.
content::StoragePartition* FindAgentPartition(Profile* profile,
                                              const std::string& id) {
  // Throwaway agents have in-memory storage (ProfileRegistry::PartitionConfig).
  content::StoragePartitionConfig config =
      content::StoragePartitionConfig::Create(
          profile, std::string(kAgentPartitionDomainPrefix) + id,
          /*partition_name=*/"",
          /*in_memory=*/FindStoredAgent(profile->GetPrefs(), id) == nullptr);
  return profile->GetStoragePartition(config, /*can_create=*/false);
}

// Runs `next` in a later task on the current sequence. Two cases need that:
//  - a mojo reply that never comes runs its wrapped default inside the
//    teardown of the Remote (Forget, a disconnect, a replaced state); the chain
//    must not re-enter the service from there;
//  - a pipe's disconnect handler must not destroy the pipe it runs in.
// Every caller is a member of ProfileProxyService or a mojo callback bound to
// its sequence (the UI thread), so the task runs there too; the DCHECK checks
// that a sequence is current.
void PostLater(base::OnceClosure next) {
  DCHECK(base::SequencedTaskRunner::HasCurrentDefault());
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                           std::move(next));
}

}  // namespace

// One agent partition's connection to the network stack. Two pipes, with
// separate lifetimes (see the header):
//  - `system`: receives the monitor's system config. Bound only if the monitor
//    gave this partition a pipe (`system_pipe_bound`);
//  - `network`: pushes to the NetworkContext. Its disconnect erases the state.
// `generation` tells a state from the one that replaced it for the same agent.
struct ProfileProxyService::AgentState final
    : public network::mojom::ProxyConfigClient {
  AgentState(base::WeakPtr<ProfileProxyService> service,
             std::string agent_id,
             uint64_t gen)
      : owner(std::move(service)), id(std::move(agent_id)), generation(gen) {}

  // ProxyConfigClient, from ProxyConfigMonitor: the system config changed.
  void OnProxyConfigUpdated(
      const net::ProxyConfigWithAnnotation& proxy_config) override {
    system_config = proxy_config;
    if (owner) {
      owner->PushConfig(this);
    }
  }

  // The monitor never asks for a flush; answer like ProxyConfigServiceMojo.
  void FlushProxyConfig(FlushProxyConfigCallback callback) override {
    std::move(callback).Run();
  }

  base::WeakPtr<ProfileProxyService> owner;
  std::string id;
  uint64_t generation;
  mojo::Receiver<network::mojom::ProxyConfigClient> system{this};
  bool system_pipe_bound = false;
  mojo::Remote<network::mojom::ProxyConfigClient> network;
  std::optional<net::ProxyConfigWithAnnotation> system_config;
  // The state of the last config sent to the NetworkContext (kNone until one
  // was sent). OnEncryptorReady uses it to find the agents that need a resend.
  StoredProxy::State last_state = StoredProxy::State::kNone;
};

ProfileProxyService::ProfileProxyService(
    Profile* profile,
    os_crypt_async::OSCryptAsync* os_crypt_async)
    : profile_(profile) {
  InitEncryption(os_crypt_async);
}

ProfileProxyService::~ProfileProxyService() = default;

void ProfileProxyService::InitEncryption(
    os_crypt_async::OSCryptAsync* os_crypt_async) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (encryption_requested_ || !os_crypt_async) {
    return;
  }
  encryption_requested_ = true;
  os_crypt_async->GetInstance(base::BindOnce(
      &ProfileProxyService::OnEncryptorReady, weak_factory_.GetWeakPtr()));
}

void ProfileProxyService::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void ProfileProxyService::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

StoredProxy ProfileProxyService::Lookup(const std::string& id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const base::Value* agent = FindStoredAgent(profile_->GetPrefs(), id)) {
    const base::DictValue* entry = agent->GetIfDict();
    return ProxyFromPrefValue(entry ? entry->Find(prefs::kProxy) : nullptr,
                              encryptor_.get());
  }
  StoredProxy result;
  if (auto it = throwaway_.find(id); it != throwaway_.end()) {
    result.state = StoredProxy::State::kProxy;
    result.spec = it->second;
  }
  return result;
}

std::optional<ProxySpec> ProfileProxyService::Get(const std::string& id) const {
  StoredProxy stored = Lookup(id);
  if (stored.state != StoredProxy::State::kProxy) {
    return std::nullopt;
  }
  return stored.spec;
}

bool ProfileProxyService::PasswordStorageReady() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return encryptor_ && encryptor_->IsEncryptionAvailable();
}

bool ProfileProxyService::Set(
    std::vector<std::pair<std::string, std::optional<ProxySpec>>> changes) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A later entry for the same id wins: each agent is written, pushed and
  // notified once.
  std::map<std::string, std::optional<ProxySpec>> normalized;
  for (auto& [id, proxy] : changes) {
    normalized[id] = std::move(proxy);
  }

  // Encode every persistent agent's proxy before anything changes: if one
  // cannot be stored, nothing is written and nothing is pushed. Clearing and
  // throwaway agents need no encoding.
  std::map<std::string, std::optional<base::DictValue>> encoded;
  for (const auto& [id, proxy] : normalized) {
    std::optional<base::DictValue> value;
    if (proxy && FindStoredAgent(profile_->GetPrefs(), id)) {
      value = ProxyToPrefValue(*proxy, encryptor_.get());
      if (!value) {
        return false;
      }
    }
    encoded.emplace(id, std::move(value));
  }

  {
    // Every persistent agent's write goes in one commit.
    ScopedDictPrefUpdate update(profile_->GetPrefs(), prefs::kProfiles);
    for (const auto& [id, proxy] : normalized) {
      base::Value* agent = update->Find(id);
      if (!agent) {
        if (proxy) {
          throwaway_[id] = *proxy;
        } else {
          throwaway_.erase(id);
        }
        continue;
      }
      base::DictValue* entry = agent->GetIfDict();
      if (!entry) {
        continue;  // Not an agent entry; leave it alone.
      }
      if (!proxy) {
        entry->Remove(prefs::kProxy);
      } else {
        entry->Set(prefs::kProxy, base::Value(std::move(*encoded[id])));
      }
    }
  }

  for (const auto& [id, unused] : normalized) {
    ResetProxyAuthState(id);
    auto it = agents_.find(id);
    if (it == agents_.end()) {
      NotifyObservers(id);
      continue;
    }
    ApplyLive(it->second.get());
  }
  return true;
}

bool ProfileProxyService::Set(const std::string& id,
                              std::optional<ProxySpec> proxy) {
  std::vector<std::pair<std::string, std::optional<ProxySpec>>> changes;
  changes.emplace_back(id, std::move(proxy));
  return Set(std::move(changes));
}

void ProfileProxyService::Forget(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  throwaway_.erase(id);
  ResetProxyAuthState(id);
  // Destroying the state runs no callback synchronously (pending replies fall
  // back into PostLater), so no observer runs inside this call.
  agents_.erase(id);
}

void ProfileProxyService::MarkProxyAuthFailed(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_auth_failures_[id] = base::TimeTicks::Now();
}

bool ProfileProxyService::ProxyAuthFailed(const std::string& id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return proxy_auth_failures_.count(id) > 0;
}

bool ProfileProxyService::ProxyAuthBackoffActive(const std::string& id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = proxy_auth_failures_.find(id);
  if (it == proxy_auth_failures_.end()) {
    return false;
  }
  return base::TimeTicks::Now() - it->second < kProxyAuthBackoff;
}

void ProfileProxyService::ResetProxyAuthState(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  proxy_auth_failures_.erase(id);
}

bool ProfileProxyService::ConfigureNetworkContextParams(
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* params) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string id = AgentIdForPartitionPath(relative_partition_path);
  if (id.empty()) {
    return false;
  }

  const uint64_t generation = ++next_generation_;
  auto state = std::make_unique<AgentState>(weak_factory_.GetWeakPtr(), id,
                                            generation);
  // The monitor (AddToNetworkContextParams ran first) filled in the initial
  // system config and, when it set up a pipe for this partition, the receiver
  // of its end. Keep the config either way.
  if (params->initial_proxy_config) {
    state->system_config = *params->initial_proxy_config;
  }
  // No valid receiver: the monitor gave this partition no pipe. Then the state
  // gets no system updates at all, and a kNone agent runs on
  // initial_proxy_config only.
  if (params->proxy_config_client_receiver.is_valid()) {
    state->system.Bind(std::move(params->proxy_config_client_receiver));
    state->system.set_disconnect_handler(base::BindOnce(
        &ProfileProxyService::SystemPipeDisconnected,
        weak_factory_.GetWeakPtr(), id, generation));
    state->system_pipe_bound = true;
  }

  // The NetworkContext gets the other end: pushes come from `state->network`.
  // The disconnect handler is only kept on a bound Remote, so bind first.
  params->proxy_config_client_receiver =
      state->network.BindNewPipeAndPassReceiver();
  state->network.set_disconnect_handler(base::BindOnce(
      &ProfileProxyService::NetworkPipeDisconnected,
      weak_factory_.GetWeakPtr(), id, generation));

  StoredProxy stored = Lookup(id);
  if (stored.state == StoredProxy::State::kProxy) {
    params->initial_proxy_config = ProxyToNetConfig(*stored.spec);
  } else if (stored.state == StoredProxy::State::kCorrupt) {
    params->initial_proxy_config = FailClosedNetConfig();
  }
  // Otherwise initial_proxy_config stays the system config (or unset while the
  // system config is pending: the monitor will send it).
  state->last_state = stored.state;

  // A new NetworkContext for the partition replaces the old state; the old
  // state's pending steps see the new generation and do nothing to the
  // NetworkContext.
  agents_[id] = std::move(state);
  return true;
}

void ProfileProxyService::ApplyLive(AgentState* state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!state->network.is_bound()) {
    // No NetworkContext channel to push through: only the observers hear.
    NotifyObservers(state->id);
    return;
  }
  PushConfig(state);
  // The flush rides the same pipe as the config push, so its reply means the
  // NetworkContext applied the config. Then connections and cache go.
  state->network->FlushProxyConfig(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(&PostLater,
                     base::BindOnce(&ProfileProxyService::CloseConnections,
                                    weak_factory_.GetWeakPtr(), state->id,
                                    state->generation))));
}

void ProfileProxyService::PushConfig(AgentState* state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!state->network.is_bound()) {
    return;
  }
  StoredProxy stored = Lookup(state->id);
  state->last_state = stored.state;
  switch (stored.state) {
    case StoredProxy::State::kProxy:
      state->network->OnProxyConfigUpdated(ProxyToNetConfig(*stored.spec));
      return;
    case StoredProxy::State::kCorrupt:
      state->network->OnProxyConfigUpdated(FailClosedNetConfig());
      return;
    case StoredProxy::State::kNone:
      // Follows the system proxy. Nothing to send until the monitor reported
      // one.
      if (state->system_config) {
        state->network->OnProxyConfigUpdated(*state->system_config);
      }
      return;
  }
}

void ProfileProxyService::CloseConnections(std::string id,
                                           uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  content::StoragePartition* partition = FindAgentPartition(profile_, id);
  if (!IsCurrent(id, generation) || !partition) {
    NotifyObservers(id);
    return;
  }
  // In-flight requests end here; new ones use the new proxy chain.
  partition->GetNetworkContext()->CloseAllConnections(
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&PostLater,
                         base::BindOnce(&ProfileProxyService::ClearHostCache,
                                        weak_factory_.GetWeakPtr(),
                                        std::move(id), generation))));
}

void ProfileProxyService::ClearHostCache(std::string id, uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  content::StoragePartition* partition = FindAgentPartition(profile_, id);
  if (!IsCurrent(id, generation) || !partition) {
    NotifyObservers(id);
    return;
  }
  partition->GetNetworkContext()->ClearHostCache(
      /*filter=*/nullptr,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&PostLater,
                         base::BindOnce(&ProfileProxyService::NotifyObservers,
                                        weak_factory_.GetWeakPtr(),
                                        std::move(id)))));
}

void ProfileProxyService::NotifyObservers(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Observers re-sync the open tabs of the agent (renderer prefs, WebRTC
  // policy) from this call.
  for (Observer& observer : observers_) {
    observer.OnAgentProxyChanged(id);
  }
}

bool ProfileProxyService::IsCurrent(const std::string& id,
                                    uint64_t generation) const {
  auto it = agents_.find(id);
  return it != agents_.end() && it->second->generation == generation;
}

void ProfileProxyService::NetworkPipeDisconnected(const std::string& id,
                                                  uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Erase later: this runs inside the pipe that the erase would destroy.
  PostLater(base::BindOnce(&ProfileProxyService::EraseDisconnected,
                           weak_factory_.GetWeakPtr(), id, generation));
}

void ProfileProxyService::EraseDisconnected(const std::string& id,
                                            uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Only the state that lost its NetworkContext goes; a replacement stays.
  if (IsCurrent(id, generation)) {
    agents_.erase(id);
  }
}

void ProfileProxyService::SystemPipeDisconnected(const std::string& id,
                                                 uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Reset later, outside the pipe's own handler. The state is kept: its
  // NetworkContext is still live and keeps the last system config.
  PostLater(base::BindOnce(&ProfileProxyService::ResetSystemPipe,
                           weak_factory_.GetWeakPtr(), id, generation));
}

void ProfileProxyService::ResetSystemPipe(const std::string& id,
                                          uint64_t generation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsCurrent(id, generation)) {
    return;
  }
  AgentState* state = agents_[id].get();
  DCHECK(state->system_pipe_bound);
  state->system.reset();
}

void ProfileProxyService::OnEncryptorReady(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  encryptor_ = std::move(encryptor);
  // Only an agent that was sent kCorrupt (its password proxy failed closed
  // while the encryptor was pending) and now reads differently needs a resend.
  // Every other agent is left alone.
  for (const auto& [id, state] : agents_) {
    if (state->last_state == StoredProxy::State::kCorrupt &&
        Lookup(id).state != StoredProxy::State::kCorrupt) {
      ApplyLive(state.get());
    }
  }
}

}  // namespace shuttle

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_H_
#define CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "chrome/browser/shuttle/agent_proxy.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace network::mojom {
class NetworkContextParams;
}

namespace os_crypt_async {
class Encryptor;
class OSCryptAsync;
}  // namespace os_crypt_async

namespace shuttle {

// The proxy of each agent: where it is stored, and how it reaches the agent's
// NetworkContext (one StoragePartition "shuttle-<id>" per agent, so one
// NetworkContext). Lives in the `agent_proxy` source_set on purpose: the
// network stack (ProfileNetworkContextService) and the login delegate call it,
// and they must not depend on ShuttleService / ProfileRegistry.
//
// Storage:
//  - persistent agent: key prefs::kProxy of the agent's entry in
//    prefs::kProfiles (ProxyToPrefValue; the entry's removal deletes it);
//  - throwaway agent (its id is not in prefs::kProfiles): memory only, gone
//    with the agent (Forget) or the browser.
//
// Created through ProfileProxyServiceFactory::GetForProfile(profile), which
// returns null for profiles without agents (off-the-record, system).
//
// Threading: UI thread only (every method checks the sequence).
class ProfileProxyService : public KeyedService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Agent `id`'s proxy was set, replaced or cleared. Runs after the pref is
    // committed and after the NetworkContext was told and its connections
    // closed and host cache cleared (the chain of Set). This call is how the
    // agent's open tabs pick the change up: re-sync their renderer prefs here.
    // It can also run for an agent that was Forget()-ed meanwhile: check that
    // the agent exists.
    virtual void OnAgentProxyChanged(const std::string& id) {}
  };

  // How long after a proxy authentication failure the agent refuses to answer
  // another challenge (see ProxyAuthBackoffActive).
  static constexpr base::TimeDelta kProxyAuthBackoff = base::Seconds(60);

  // `os_crypt_async` may be null: passwords then read as kCorrupt until
  // InitEncryption gets one.
  ProfileProxyService(Profile* profile,
                      os_crypt_async::OSCryptAsync* os_crypt_async);
  ProfileProxyService(const ProfileProxyService&) = delete;
  ProfileProxyService& operator=(const ProfileProxyService&) = delete;
  ~ProfileProxyService() override;

  // Requests the os_crypt encryptor, once. A null pointer, or a call after the
  // first request, does nothing. ProfileProxyServiceFactory::GetForProfile
  // calls it each time, so a service built before the browser had os_crypt
  // gets its encryptor on the next GetForProfile.
  void InitEncryption(os_crypt_async::OSCryptAsync* os_crypt_async);
  // Bootstrap hands off the exact validated encryptor synchronously, before
  // any partition can ask for proxy auth/configuration.
  void AdoptBootstrapEncryptor(
      scoped_refptr<os_crypt_async::Encryptor> encryptor);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // The agent's stored proxy (kNone for an unknown id).
  StoredProxy Lookup(const std::string& id) const;
  // The agent's proxy if it has a readable one (state kProxy), else nullopt.
  std::optional<ProxySpec> Get(const std::string& id) const;

  // True when a proxy with a password can be stored (the os_crypt encryptor is
  // ready). Clearing and password-less proxies never need it.
  bool PasswordStorageReady() const;

  // Sets (a value) or clears (nullopt) the proxy of each agent, all or nothing.
  // Callers must have validated each spec with CheckProxy and checked that the
  // ids are existing agents (ProfileRegistry::Exists); nothing is validated
  // here. An id that is in prefs::kProfiles is persistent, any other is a
  // throwaway agent (memory only, no encryption). If an id appears twice, the
  // last entry wins: each agent is written, pushed and notified once.
  //
  // Returns false, with NOTHING written (no pref, no memory, no live push, no
  // observer), only when a persistent agent's proxy cannot be stored: it has a
  // password and the encryptor is not ready (see PasswordStorageReady), or
  // encryption fails. Otherwise applies every change and returns true:
  //  - the pref is committed, and the agent's proxy-auth failure state is
  //    reset (ResetProxyAuthState);
  //  - for an agent whose NetworkContext exists, its effective config is
  //    pushed live (no restart, tabs stay open), then the NetworkContext
  //    closes the agent's connections (CloseAllConnections), clears its host
  //    cache, and the observers are notified (OnAgentProxyChanged). Requests
  //    already in flight at that moment are terminated by CloseAllConnections:
  //    until then they may still have used the old route. New requests resolve
  //    the new proxy chain (the socket pool is keyed by proxy chain);
  //  - an agent without a NetworkContext has nothing to push: the observers
  //    are notified now, and the value is read when its partition is created.
  bool Set(
      std::vector<std::pair<std::string, std::optional<ProxySpec>>> changes);
  bool Set(const std::string& id, std::optional<ProxySpec> proxy);

  // The agent was deleted or expired: drops its memory entry, its live config
  // client and its proxy-auth state. (A persistent agent's pref goes with its
  // entry.) A live change still pending for it does not touch its
  // NetworkContext; its observers may still be notified once (see Observer).
  void Forget(const std::string& id);

  // Proxy authentication failures, per agent, UI thread only. Used by
  // proxy_auth.cc: after a wrong proxy password the agent refuses further
  // challenges for a while instead of looping on 407.
  // Records a failure now (a later one replaces the time).
  void MarkProxyAuthFailed(const std::string& id);
  // True if a failure was recorded since the agent's proxy last changed.
  bool ProxyAuthFailed(const std::string& id) const;
  // True if a failure was recorded less than kProxyAuthBackoff ago.
  bool ProxyAuthBackoffActive(const std::string& id) const;
  // Forgets the failure of the agent (called when its proxy changes).
  void ResetProxyAuthState(const std::string& id);

  // Called by ProfileNetworkContextService::ConfigureNetworkContextParamsInternal
  // for every partition, after proxy_config_monitor_->AddToNetworkContextParams
  // filled `params`. For an agent partition (shuttle::AgentIdForPartitionPath)
  // it takes over the system config client the monitor set up: it keeps the
  // monitor's initial config as the system config, takes the monitor's
  // receiver, and puts its own pipe in proxy_config_client_receiver. Then it
  // sets initial_proxy_config and returns true:
  //  - the agent has a proxy: its config (ProxyToNetConfig);
  //  - kCorrupt: FailClosedNetConfig();
  //  - kNone (no own proxy): the system config. That is the monitor's initial
  //    config if it had one (left as it is in `params`), and later the config
  //    the monitor reports, which the service forwards, until the monitor's
  //    pipe goes away (shutdown). The last one stays in use. Until the monitor
  //    has reported one, the NetworkContext gets none, as without this service.
  // The two pipes have separate lifetimes: the NetworkContext's pipe going
  // away erases the state; the monitor's pipe going away does not.
  // The service keeps its end of the pipe to push later changes. For any other
  // partition returns false and leaves `params` alone.
  bool ConfigureNetworkContextParams(
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* params);

 private:
  struct AgentState;

  // Pushes the agent's effective config and starts the live-change chain
  // (flush, then CloseConnections, ClearHostCache, NotifyObservers). An agent
  // whose NetworkContext channel is not bound only notifies the observers.
  void ApplyLive(AgentState* state);
  // Sends the agent's effective config to its NetworkContext (no-op if the
  // channel is not bound) and records the state it sent.
  void PushConfig(AgentState* state);
  // Step 2: the NetworkContext drops the agent's connections. Does nothing to
  // the NetworkContext if the state was replaced or forgotten (observers are
  // still notified).
  void CloseConnections(std::string id, uint64_t generation);
  // Step 3: the NetworkContext clears the agent's host cache. Then the
  // observers are notified (step 4). A stale state skips the NetworkContext.
  void ClearHostCache(std::string id, uint64_t generation);
  void NotifyObservers(const std::string& id);
  // True if agent `id` has the state of `generation` (not replaced, forgotten).
  bool IsCurrent(const std::string& id, uint64_t generation) const;
  // The NetworkContext's pipe (`network`) disconnected: the NetworkContext is
  // gone, so the state is erased in a later task.
  void NetworkPipeDisconnected(const std::string& id, uint64_t generation);
  void EraseDisconnected(const std::string& id, uint64_t generation);
  // The monitor's pipe (`system`) disconnected (the monitor went away, at
  // shutdown): the receiver is reset in a later task. The state itself stays,
  // with its last system config, and keeps serving the agent.
  void SystemPipeDisconnected(const std::string& id, uint64_t generation);
  void ResetSystemPipe(const std::string& id, uint64_t generation);
  // Stores the encryptor. Each agent that was last sent kCorrupt (its password
  // proxy failed closed while the encryptor was pending) and now reads
  // differently gets the chain of Set; the others are left alone.
  void OnEncryptorReady(scoped_refptr<os_crypt_async::Encryptor> encryptor);

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Profile> profile_;
  bool encryption_requested_ = false;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  uint64_t next_generation_ = 0;
  base::ObserverList<Observer> observers_;
  // Memory-only proxies of throwaway agents (an absent id has none).
  std::map<std::string, ProxySpec> throwaway_;
  std::map<std::string, std::unique_ptr<AgentState>> agents_;
  std::map<std::string, base::TimeTicks> proxy_auth_failures_;
  base::WeakPtrFactory<ProfileProxyService> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_H_

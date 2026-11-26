// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_ROUTER_H_
#define CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_ROUTER_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/shuttle/browser_owner_index.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"
#include "chrome/browser/shuttle/managed_profile_bootstrap.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/network_service.mojom.h"

class Profile;
class ScopedProfileKeepAlive;
namespace content {
class StoragePartition;
class NetworkContextHandoffProof;
struct NetworkContextHandoffState;
}

namespace os_crypt_async {
class Encryptor;
}

namespace shuttle {

// What the router asks of the layer that owns the Profile's work (B2:
// ShuttleService / session manager / CDP). Implemented outside this file; the
// router never reaches into ShuttleService itself.
class BrowserProfileDrainDelegate {
 public:
  virtual ~BrowserProfileDrainDelegate() = default;

  // Drains `profile`. The router has ALREADY: persisted `draining` and the
  // incremented lifecycle epoch, and revoked every session/target entry of the
  // Profile in the owner indexes (so no new scoped handle resolves).
  // The delegate must: close admission of new work for the Profile (tool
  // calls, session open, task/worker dispatch), cancel/wait running tasks,
  // close the Profile's sessions, tabs, workers and CDP connections (and their
  // endpoint grants), and flush agent storage/cookies. `done(true)` when
  // everything is closed, `done(false)` if it cannot be (the Profile then
  // goes to `failed`). It may take longer than the router's timeout: the
  // router then reports drain_timeout and leaves the Profile `draining`; a
  // late `done` still completes the stop. Never closes other Profiles'
  // work and never stops the browser.
  virtual void DrainProfile(Profile* profile,
                            const std::string& core_profile_key,
                            int64_t lifecycle_epoch,
                            base::OnceCallback<void(bool success)> done) = 0;

  // Whether any session, tab, worker or connection of `profile` still
  // exists (delete-browser-profile answers profile_busy if so).
  virtual bool HasDescendantHandles(Profile* profile) = 0;
};

// `ProfileRecord` of the contract: no paths, credentials, seeds or endpoints.
struct ProfileRecord {
  ProfileRecord();
  ProfileRecord(const ProfileRecord&);
  ProfileRecord& operator=(const ProfileRecord&);
  ~ProfileRecord();

  // {coreProfileKey, workspaceAgent, lifecycleEpoch, browserEpoch, lifecycle,
  // loaded, deletionPending, personaSummary, lastErrorCode}
  base::DictValue ToValue() const;

  std::string core_profile_key;
  std::string workspace_agent;
  int64_t lifecycle_epoch = 0;
  std::string browser_epoch;
  std::string lifecycle;
  bool loaded = false;
  bool deletion_pending = false;
  bool persona_replacement_pending = false;
  // JSON of personaSummary (no seed), "" = null.
  std::string persona_summary_json;
  std::string last_error_code;
};

struct ProfileList {
  ProfileList();
  ProfileList(const ProfileList&);
  ProfileList& operator=(const ProfileList&);
  ~ProfileList();

  // {profiles: [ProfileRecord], browserEpoch}
  base::DictValue ToValue() const;

  std::vector<ProfileRecord> profiles;
  std::string browser_epoch;
};

// Arguments of create-browser-profile, already schema-checked by the tool
// layer (unknown properties rejected there). The router validates the values.
struct CreateProfileArgs {
  CreateProfileArgs();
  CreateProfileArgs(CreateProfileArgs&&);
  CreateProfileArgs& operator=(CreateProfileArgs&&);
  ~CreateProfileArgs();

  std::string request_id;
  std::string workspace_agent;
  // The `persona` object (ParseFingerprintPersonaInput form, preset required).
  base::Value persona;
  // The `proxy` argument (string or object, ParseProxyValue form), if any.
  std::optional<base::Value> proxy;
};

// A resolved scope: the Profile a request runs in.
struct ResolvedProfile {
  raw_ptr<Profile> profile = nullptr;
  // "" for the startup Profile (legacy scope).
  std::string core_profile_key;
  // "" for the startup Profile (the caller supplies the agent).
  std::string workspace_agent;
  // 0 for the startup Profile.
  int64_t lifecycle_epoch = 0;
  bool is_startup = false;
};

using ProfileRecordCallback =
    base::OnceCallback<void(base::expected<ProfileRecord, BrowserProfileFailure>)>;

// The browser-level router of isolated Chromium Profiles
// (tools/shuttle/docs/PROFILES.md). One instance per browser process, owned
// here (Initialize/Shutdown, called from ChromeBrowserMainExtraPartsProfiles),
// UI thread only. It never uses GetLastUsedProfile()/last-used selection.
//
// Not yet compiled (see PROFILES.md, "Implementation status").
class BrowserProfileRouter : public ManagedProfileProvider,
                             public ProfileObserver {
 public:
  static constexpr base::TimeDelta kDefaultDrainTimeout = base::Seconds(30);

  // True if --shuttle-isolated-profiles is on the command line.
  static bool IsEnabledBySwitch();

  // Creates the process-wide router when the switch is on (blocking: reads the
  // manifest and the admin token, so call it where blocking is allowed, e.g.
  // PreProfileInit). Returns the router or null when the switch is off. When
  // the manifest or the admin token is unusable the router still exists but
  // IsAvailable() is false and every operation fails closed.
  static BrowserProfileRouter* Initialize(const base::FilePath& user_data_dir);
  // Destroys it (PostMainMessageLoopRun). Releases keep-alives only; it does
  // not stop or delete anything (a restart recovers every entry as stopped).
  static void Shutdown();
  // Null when the switch is off.
  static BrowserProfileRouter* Get();

  ~BrowserProfileRouter() override;

  // --- Startup / legacy scope ---------------------------------------------
  // Captures the fixed startup Profile once (ChromeBrowserMainExtraParts::
  // PostProfileInit with is_initial_profile). Later calls are ignored.
  void CaptureStartupProfile(Profile* profile);
  // The Profile of requests without a selector; null before the capture.
  Profile* ResolveStartupProfile() const { return startup_profile_; }

  // --- Resolution ------------------------------------------------------------
  // `key` absent = the startup Profile. Present: malformed -> invalid_profile_key,
  // unknown or deleted -> profile_not_found, not `ready` (or the Profile object
  // is gone) -> profile_not_ready. Never falls back.
  base::expected<ResolvedProfile, BrowserProfileFailure> Resolve(
      const std::optional<std::string>& key) const;
  // The managed key of `profile` while it is served by this router, or "" (the
  // startup/unmanaged Profile).
  std::string KeyOfProfile(Profile* profile) const;
  // The owner tuple for a session of `workspace_agent` in `resolved`.
  OwnerTuple MakeOwner(const ResolvedProfile& resolved,
                       const std::string& workspace_agent) const;

  // --- Browser-level operations (contract section 3) --------------------------
  // All callbacks run asynchronously on the UI thread.
  void CreateProfile(CreateProfileArgs args, ProfileRecordCallback callback);
  base::expected<ProfileList, BrowserProfileFailure> ListProfiles();
  void LoadProfile(const std::string& core_profile_key,
                   ProfileRecordCallback callback);
  // Drain + flush; never stops the browser. drain_timeout leaves it draining.
  void StopProfile(const std::string& core_profile_key,
                   ProfileRecordCallback callback);
  // Requires `stopped` and no descendant handles; never the startup Profile.
  // Completes once the tombstone is persisted and the Chromium removal is
  // scheduled (deletionPending true until the directory is confirmed gone).
  void DeleteProfile(const std::string& core_profile_key,
                     ProfileRecordCallback callback);
  // Contract section 7: replaces the persona of a managed Profile that is
  // `stopped` (anything else: profile_busy / profile_not_ready /
  // profile_not_found), without loading it. `persona_json` is a full persona
  // input (ParseFingerprintPersonaInput form, preset required), validated with
  // ParseFingerprintPersonaInput + CheckFingerprintPersona (persona_invalid).
  // The new intent, the pending flag and the incremented lifecycle epoch are
  // persisted in one atomic registry write; every owner-index entry of the
  // Profile is revoked again (a handle of the old epoch can never resolve).
  // The bootstrap commits the intent to the Profile prefs at the next load;
  // a Profile still in memory (stop gives no unload guarantee) is committed
  // right away. Cookies and storage are never touched. The result is the new
  // record (personaSummary refreshed, no seed).
  void ReplacePersona(const std::string& core_profile_key,
                      std::string persona_json,
                      ProfileRecordCallback callback);

  // --- State -----------------------------------------------------------------
  // The isolated capability can serve requests (manifest and admin token OK).
  bool IsAvailable() const { return failure_ == BrowserProfileError::kNone; }
  // Why it cannot (storage_error / unsupported), or kNone.
  BrowserProfileError unavailable_reason() const { return failure_; }
  // The B2 lane sets this once scoped routing (MCP/CDP) is wired end to end;
  // until then get-capabilities reports scopedRouting:false and the manager
  // refuses isolated mode.
  void SetScopedRoutingReady(bool ready) { scoped_routing_ready_ = ready; }
  bool scoped_routing_ready() const { return scoped_routing_ready_; }
  // New on every browser start.
  const std::string& browser_epoch() const { return browser_epoch_; }

  void SetDrainDelegate(BrowserProfileDrainDelegate* delegate) {
    drain_delegate_ = delegate;
  }
  void SetDrainTimeoutForTesting(base::TimeDelta timeout) {
    drain_timeout_ = timeout;
  }

  SessionOwnerIndex& session_owners() { return session_owners_; }
  TargetOwnerIndex& target_owners() { return target_owners_; }

  // Re-checks tombstones whose removal is pending (directory gone =>
  // confirmed); with `reschedule`, asks Chromium again to remove directories
  // that still exist (startup recovery after a crash during deletion).
  void ReconcileTombstones(bool reschedule);

  // ManagedProfileProvider:
  std::optional<ManagedProfileIntent> LookupByDirectory(
      std::string_view directory) override;
  void OnBootstrapFinished(std::string_view directory,
                           BrowserProfileError error) override;

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

 private:
  friend class BrowserProfileLifecycleBrowserTest;
  // Test interception of the asynchronous pref flush completion (never skips
  // the real flush); permits deterministic stalled-flush/timeout coverage.
  base::RepeatingCallback<void(base::OnceClosure)> drain_flush_for_testing_;
  // One-shot fault boundary on the production replacement transaction. Tests
  // retain the continuation while disconnecting/crashing staged service owners.
  base::OnceCallback<void(base::OnceClosure)> handoff_before_commit_for_testing_;
  base::RepeatingCallback<void(network::mojom::StorageHandoffTransaction*)>
      handoff_release_for_testing_;
  base::RepeatingCallback<void(content::StoragePartition*,
                              base::OnceCallback<void(bool)>)>
      handoff_activate_for_testing_;
  struct Runtime {
    Runtime();
    Runtime(Runtime&&);
    Runtime& operator=(Runtime&&);
    ~Runtime();

    raw_ptr<Profile> profile = nullptr;
    std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
  };

  explicit BrowserProfileRouter(const base::FilePath& user_data_dir);

  base::FilePath PathForDirectory(const std::string& directory) const;
  ProfileRecord MakeRecord(const BrowserProfileEntry& entry) const;
  // A failure for a request that cannot be served (unavailable router).
  BrowserProfileFailure UnavailableFailure() const;
  static void RunLater(ProfileRecordCallback callback,
                       base::expected<ProfileRecord, BrowserProfileFailure> result);

  // create
  void AllocateDirectory(CreateProfileArgs args,
                         std::string persona_json,
                         std::string proxy_pref_json,
                         std::string fingerprint,
                         ProfileRecordCallback callback,
                         int attempts_left);
  void OnDirectoryChecked(CreateProfileArgs args,
                          std::string persona_json,
                          std::string proxy_pref_json,
                          std::string fingerprint,
                          std::string directory,
                          ProfileRecordCallback callback,
                          int attempts_left,
                          bool exists);
  void OnCreated(ProfileRecordCallback callback,
                 base::expected<BrowserProfileEntry, BrowserProfileFailure>);

  // load
  void BeginLoad(const std::string& key);
  void OnLoadingPersisted(
      std::string key, int64_t epoch, std::string intent,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnProfileInitialized(std::string key, int64_t epoch,
                            std::string intent, Profile* profile);
  void OnLoadedContextsRetired(std::string key, int64_t epoch,
                              std::string intent,
                              base::WeakPtr<Profile> profile, bool success);
  void OnProfileIdentityReady(std::string key, Profile* profile);
  void OnLoadedPersonaRecovered(std::string key, int64_t epoch,
                               std::string intent,
                               base::WeakPtr<Profile> profile);
  void OnInitializingPersisted(
      std::string key, int64_t epoch, std::string intent,
      base::WeakPtr<Profile> profile,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnReadyPersisted(
      std::string key,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnProvisionedPersisted(
      std::string key, int64_t epoch, std::string intent,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnLoadActivationFenced(std::string key, int64_t epoch,
                              std::string intent, bool success);
  void OnLoadActivated(std::string key, int64_t epoch,
                        std::string intent, bool success);
  void FailLoad(const std::string& key, BrowserProfileError error);
  void OnFailedPersisted(
      std::string key,
      BrowserProfileError error,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void CompleteLoad(const std::string& key);
  void CompleteLoadFailure(const std::string& key,
                           BrowserProfileFailure failure);

  // stop
  void BeginDrain(const std::string& key);
  void OnDrainingPersisted(
      std::string key,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnDrainDone(std::string key, int64_t epoch, bool success);
  void FlushDrainPrefs(std::string key, int64_t epoch);
  void OnDrainFlushed(std::string key, int64_t epoch);
  void OnStoppedPersisted(
      std::string key,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnDrainTimeout(std::string key);
  // Whether anything of the Profile (a session, tab, worker, connection or
  // sidecar) still exists. Fail closed: a loaded Profile with no drain
  // delegate counts as having handles.
  bool HasLiveHandles(const BrowserProfileEntry& entry) const;
  // A stop on a Profile that stayed `draining` after a drain gave up: runs the
  // drain again (the state never became failed or stopped).
  void ResumeDrain(const std::string& key);
  void CompleteStop(const std::string& key,
                    base::expected<ProfileRecord, BrowserProfileFailure> result);

  // replace persona
  // Ends the per-Profile persona operation (admission) and answers.
  void FinishPersonaOperation(
      std::string key,
      ProfileRecordCallback callback,
      base::expected<ProfileRecord, BrowserProfileFailure> result);
  void OnPersonaIntentPersisted(
      std::string key,
      ProfileRecordCallback callback,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void ApplyPersonaAfterRetirement(
      std::string key, int64_t epoch, std::string intent,
      base::WeakPtr<Profile> profile, ProfileRecordCallback callback,
      bool success);
  // `epoch` and `intent` are what OnPersonaIntentPersisted captured: every
  // continuation re-checks them against the registry.
  void OnPersonaCommitted(std::string key,
                          int64_t epoch,
                          std::string intent,
                          ProfileRecordCallback callback);
  void OnPersonaApplied(
      std::string key,
      int64_t epoch,
      std::string intent,
      ProfileRecordCallback callback,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnPersonaHandoffStaged(
      std::string key, int64_t epoch, std::string intent,
      std::string summary_json, ProfileRecordCallback callback, bool success);
  void OnPersonaActivationFenced(std::string key, int64_t epoch,
      std::string intent, ProfileRecordCallback callback, bool success);
  void OnPersonaActivated(std::string key, int64_t epoch,
      std::string intent, ProfileRecordCallback callback, bool success);

  // delete
  void OnTombstonePersisted(
      std::string key,
      ProfileRecordCallback callback,
      base::expected<BrowserProfileEntry, BrowserProfileFailure> result);
  void OnTombstoneChecked(std::string key,
                          base::FilePath path,
                          bool reschedule,
                          bool exists);

  // Runtime handles
  struct ProfileStorageHandoff {
    std::string key;
    int64_t epoch = 0;
    std::string intent;
    bool teardown_complete = false;
    bool retirement_complete = false;
    bool retirement_succeeded = false;
    bool teardown_started = false;
    bool failure_finishing = false;
    std::vector<base::OnceClosure> failure_waiters;
    std::vector<base::OnceClosure> teardown_waiters;
    std::vector<content::StoragePartition*> participants;
    // Strong operation-owned generations survive partition teardown and every
    // asynchronous fence. Browser shutdown loses session snapshots; there is no
    // full-browser-crash fidelity/replay guarantee.
    std::vector<std::shared_ptr<content::NetworkContextHandoffState>> snapshots;
    std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
    mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
    base::UnguessableToken service_instance;
    base::UnguessableToken token;
    enum class Phase { kRetiring, kRestoring, kRestoredStaged, kRegistryCommitted,
                       kServicePrepared,
                       kReleasePossible, kReleased, kAborting,
                       kFailedIndeterminate };
    Phase phase = Phase::kRetiring;
    void Transition(Phase next);
  };
  bool OwnsHandoff(Profile* profile,
      const std::shared_ptr<ProfileStorageHandoff>& operation) const;
  content::NetworkContextHandoffProof MakeHandoffProof(
      Profile* profile, content::StoragePartition* participant,
      const std::shared_ptr<ProfileStorageHandoff>& operation);
  void FailHandoffContinuation(Profile* profile,
      std::shared_ptr<ProfileStorageHandoff> operation,
      base::OnceCallback<void(bool)> callback);
  void RetireProfileContexts(Profile* profile, std::string key, int64_t epoch,
      std::string intent, base::OnceCallback<void(bool)> callback);
  bool FinishHandoffFailure(const std::string& key, base::OnceClosure callback);
  void StageProfileHandoff(Profile* profile, base::OnceCallback<void(bool)> callback,
      bool activate = false,
      base::RepeatingCallback<void(content::StoragePartition*,
          base::OnceCallback<void(bool)>)> activation_hook = {});
  bool ProfileHandoffRestored(Profile* profile);
  void FinalizeProfileHandoff(Profile* profile, bool success);
  void CompensateProfileHandoff(BrowserProfileRegistry* registry, Profile* profile,
      std::string key, int64_t epoch, std::string intent,
      BrowserProfileRegistry::CommitCallback callback);
  void AbortProfileHandoff(Profile* profile, base::OnceClosure callback);
  static void StartHandoffTeardown(std::shared_ptr<ProfileStorageHandoff> operation);
  std::map<Profile*, std::shared_ptr<ProfileStorageHandoff>> handoffs_;
  std::set<std::string> indeterminate_handoffs_;
  void ReleaseRuntime(const std::string& key);
  // Revokes every owner-index entry of the Profile.
  void RevokeOwners(const std::string& key);
  // Keeps the startup Profile the last-used one (see .cc): managed Profiles
  // with windows must not become what the next browser start opens.
  void SanitizeLastUsedProfile();

  SEQUENCE_CHECKER(sequence_checker_);
  const base::FilePath user_data_dir_;
  const std::string browser_epoch_;
  BrowserProfileError failure_ = BrowserProfileError::kNone;
  bool scoped_routing_ready_ = false;
  std::unique_ptr<BrowserProfileRegistry> registry_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
  raw_ptr<Profile> startup_profile_ = nullptr;
  raw_ptr<BrowserProfileDrainDelegate> drain_delegate_ = nullptr;
  base::TimeDelta drain_timeout_ = kDefaultDrainTimeout;

  SessionOwnerIndex session_owners_;
  TargetOwnerIndex target_owners_;

  std::map<std::string, Runtime> runtimes_;
  // Concurrent loads/stops of one key share one operation.
  std::map<std::string, std::vector<ProfileRecordCallback>> load_waiters_;
  std::map<std::string, std::vector<ProfileRecordCallback>> stop_waiters_;
  std::map<std::string, base::OneShotTimer> drain_timers_;
  // Full drain operation: initial persistence, delegate, async pref flush and
  // durable stopped write. Retries join while any of these are outstanding.
  std::set<std::string> drains_in_flight_;
  // Per-Profile operation admission of a persona replacement: while a key is
  // here, load, delete and another replacement of it are refused
  // (profile_busy) until the replacement is committed or failed.
  std::set<std::string> persona_operations_;
  // Last bootstrap failure by directory name (consumed by the load).
  std::map<std::string, BrowserProfileError> bootstrap_errors_;

  base::ScopedMultiSourceObservation<Profile, ProfileObserver>
      profile_observations_{this};
  base::WeakPtrFactory<BrowserProfileRouter> weak_factory_{this};
};

// get-capabilities (contract section 2): {capabilitiesVersion, isolatedProfiles:
// {supported, enabled}, scopedRouting, profileScopeMetadata, bindingVersion,
// browserEpoch}. Works with no router (switch off): supported reflects the
// build (IsolatedProfilesBootstrapSafe), enabled and scopedRouting are false.
// `enabled` is true only with the switch, a usable registry and admin token,
// and a safe bootstrap; `scopedRouting` only after the B2 lane called
// SetScopedRoutingReady(true).
base::DictValue BuildCapabilities();

// Opaque boot id of this browser process (UUID, new on every start). The
// router's browser_epoch() is this value.
const std::string& ProcessBrowserEpoch();

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_ROUTER_H_

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/browser_profile_router.h"
#include "chrome/browser/shuttle/managed_network_admission.h"
#include "base/barrier_closure.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/delete_profile_helper.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/shuttle/agent_proxy.h"
#include "chrome/browser/shuttle/browser_admin_auth.h"
#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "chrome/browser/shuttle/profile_persona_service.h"
#include "chrome/browser/shuttle/profile_persona_service_factory.h"
#include "chrome/browser/shuttle/switches.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/pref_names.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/network_service_instance.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"

namespace shuttle {

namespace {

BrowserProfileRouter* g_router = nullptr;

constexpr int kMaxDirectoryAttempts = 32;
constexpr char kManagedDirectoryPrefix[] = "Profile ";

// The names the router allocates: ProfileManager's multi-profile form
// "Profile <number>" (chrome::kMultiProfileDirPrefix), nothing else.
bool IsAllocatedDirectoryName(std::string_view name) {
  if (!name.starts_with(kManagedDirectoryPrefix)) {
    return false;
  }
  std::string_view digits = name.substr(sizeof(kManagedDirectoryPrefix) - 1);
  if (digits.empty() || digits.size() > 9) {
    return false;
  }
  for (char c : digits) {
    if (!base::IsAsciiDigit(c)) {
      return false;
    }
  }
  return true;
}

base::expected<ProfileRecord, BrowserProfileFailure> Failure(
    BrowserProfileError code) {
  return base::unexpected(BrowserProfileFailure(code));
}

void IgnoreEntry(base::expected<BrowserProfileEntry, BrowserProfileFailure>) {}

}  // namespace

void BrowserProfileRouter::ProfileStorageHandoff::Transition(Phase next) {
  const bool abort = next == Phase::kAborting &&
      phase != Phase::kReleasePossible && phase != Phase::kFailedIndeterminate;
  CHECK(abort ||
        (phase == Phase::kRetiring && next == Phase::kRestoring) ||
        (phase == Phase::kRestoring && next == Phase::kRestoredStaged) ||
        (phase == Phase::kRestoredStaged && next == Phase::kRegistryCommitted) ||
        (phase == Phase::kRegistryCommitted && next == Phase::kServicePrepared) ||
        (phase == Phase::kServicePrepared && next == Phase::kReleasePossible) ||
        (phase == Phase::kReleasePossible &&
         (next == Phase::kReleased || next == Phase::kFailedIndeterminate ||
          next == Phase::kAborting)));
  phase = next;
}

bool BrowserProfileRouter::OwnsHandoff(
    Profile* profile,
    const std::shared_ptr<ProfileStorageHandoff>& operation) const {
  auto owner = handoffs_.find(profile);
  const auto* entry = registry_->Find(operation->key);
  return owner != handoffs_.end() && owner->second == operation && entry &&
      entry->lifecycle_epoch == operation->epoch &&
      entry->persona_input_json == operation->intent &&
      !operation->teardown_started;
}

content::NetworkContextHandoffProof BrowserProfileRouter::MakeHandoffProof(
    Profile* profile, content::StoragePartition* participant,
    const std::shared_ptr<ProfileStorageHandoff>& operation) {
  auto member = participant->GetNetworkContextHandoffConstructionMember();
  return content::NetworkContextHandoffProof(profile, participant,
      member ? member->attempt : base::UnguessableToken(),
      member ? member->generation : 0,
      base::BindRepeating(
          [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
             std::weak_ptr<ProfileStorageHandoff> weak_operation,
             content::StoragePartition* participant) {
            auto operation = weak_operation.lock();
            if (!self || !operation || !self->OwnsHandoff(profile, operation) ||
                !operation->retirement_complete || !operation->retirement_succeeded ||
                operation->phase != ProfileStorageHandoff::Phase::kRestoring)
              return false;
            auto it = std::ranges::find(operation->participants, participant);
            return it != operation->participants.end() &&
                operation->snapshots[it - operation->participants.begin()] ==
                    participant->GetNetworkContextHandoffState();
          }, weak_factory_.GetWeakPtr(), profile,
          std::weak_ptr<ProfileStorageHandoff>(operation), participant));
}

bool BrowserProfileRouter::ProfileHandoffRestored(Profile* profile) {
  auto it = handoffs_.find(profile);
  if (it == handoffs_.end()) return true;
  bool ok = true;
  for (auto* partition : it->second->participants)
    ok &= partition->IsNetworkContextHandoffRestored();
  return ok;
}

void BrowserProfileRouter::FailHandoffContinuation(
    Profile* profile, std::shared_ptr<ProfileStorageHandoff> operation,
    base::OnceCallback<void(bool)> callback) {
  auto owner = handoffs_.find(profile);
  if (owner == handoffs_.end() || owner->second != operation) {
    // A stale callback must never tear down a newer operation on this Profile.
    std::move(callback).Run(false);
    return;
  }
  AbortProfileHandoff(profile, base::BindOnce(std::move(callback), false));
}

void BrowserProfileRouter::FinalizeProfileHandoff(Profile* profile, bool success) {
  auto it = handoffs_.find(profile);
  if (it == handoffs_.end()) return;
  CHECK(!success || it->second->phase == ProfileStorageHandoff::Phase::kReleased);
  for (auto* partition : it->second->participants)
    partition->FinalizeNetworkContextHandoff(success);
  handoffs_.erase(it);
}

void BrowserProfileRouter::AbortProfileHandoff(Profile* profile,
                                              base::OnceClosure callback) {
  auto it = handoffs_.find(profile);
  if (it == handoffs_.end()) {
    std::move(callback).Run();
    return;
  }
  auto operation = it->second;
  if (operation->teardown_complete) {
    std::move(callback).Run();
    return;
  }
  operation->teardown_waiters.push_back(std::move(callback));
  if (operation->teardown_started) return;
  operation->teardown_started = true;
  if (operation->phase == ProfileStorageHandoff::Phase::kReleasePossible) {
    indeterminate_handoffs_.insert(operation->key);
    operation->Transition(ProfileStorageHandoff::Phase::kFailedIndeterminate);
  } else {
    operation->Transition(ProfileStorageHandoff::Phase::kAborting);
  }
  operation->transaction.reset();
  // Retirement itself owns a full-destructor fence. Cancellation must not erase
  // the operation or unlock lifecycle admission while that fence is outstanding.
  if (operation->retirement_complete) StartHandoffTeardown(operation);
}

void BrowserProfileRouter::StartHandoffTeardown(
    std::shared_ptr<ProfileStorageHandoff> operation) {
  auto destroyed = base::BarrierClosure(operation->participants.size(),
      base::BindOnce([](std::shared_ptr<ProfileStorageHandoff> operation) {
         operation->teardown_complete = true;
         auto waiters = std::move(operation->teardown_waiters);
         for (auto& waiter : waiters) std::move(waiter).Run();
       }, operation));
  for (auto* partition : operation->participants)
    partition->AbortNetworkContextHandoff(destroyed);
}

void BrowserProfileRouter::CompensateProfileHandoff(BrowserProfileRegistry*, Profile*,
                              std::string key, int64_t, std::string,
                              BrowserProfileRegistry::CommitCallback callback) {
  // All failure exits share one teardown + conditional compensation. Callers
  // report failure regardless of compensation's result; admission stays closed.
  auto done = base::BindOnce(
      [](BrowserProfileRegistry::CommitCallback callback) {
        std::move(callback).Run(base::unexpected(
            BrowserProfileFailure(BrowserProfileError::kProfileFailed)));
      }, std::move(callback));
  bool exists = false;
  for (const auto& [profile, operation] : handoffs_)
    exists |= operation->key == key;
  if (!exists) {
    std::move(done).Run();
  } else {
    FinishHandoffFailure(key, std::move(done));
  }
}

void BrowserProfileRouter::StageProfileHandoff(Profile* profile,
                         base::OnceCallback<void(bool)> callback,
                          bool activate,
                          base::RepeatingCallback<void(content::StoragePartition*,
                              base::OnceCallback<void(bool)>)> activation_hook) {
  auto it = handoffs_.find(profile);
  if (it == handoffs_.end()) {
    std::move(callback).Run(true);
    return;
  }
  auto operation = it->second;
  if (!OwnsHandoff(profile, operation)) {
    AbortProfileHandoff(profile, base::BindOnce(std::move(callback), false));
    return;
  }
  if (!activate && operation->phase == ProfileStorageHandoff::Phase::kRetiring)
    operation->Transition(ProfileStorageHandoff::Phase::kRestoring);
  if (!activate && operation->phase == ProfileStorageHandoff::Phase::kRestoredStaged)
    operation->Transition(ProfileStorageHandoff::Phase::kRegistryCommitted);
  if (activate && operation->phase != ProfileStorageHandoff::Phase::kRegistryCommitted) {
    AbortProfileHandoff(profile, base::BindOnce(std::move(callback), false));
    return;
  }
  if (activate && operation->token.is_empty()) {
    auto split_begin = base::SplitOnceCallback(std::move(callback));
    std::vector<network::mojom::StorageHandoffMemberPtr> expected;
    for (auto* partition : operation->participants) {
      auto member = partition->GetNetworkContextHandoffMember();
      if (member) expected.push_back(std::move(member));
    }
    content::GetNetworkService()->BeginStorageHandoff(
        operation->transaction.BindNewPipeAndPassReceiver(), std::move(expected),
        mojo::WrapCallbackWithDropHandler(base::BindOnce(
          [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
             std::shared_ptr<ProfileStorageHandoff> operation,
             base::OnceCallback<void(bool)> callback,
             base::RepeatingCallback<void(content::StoragePartition*,
                 base::OnceCallback<void(bool)>)> hook,
             const base::UnguessableToken& instance,
             const base::UnguessableToken& token) {
             if (!self) return;
             if (!self->OwnsHandoff(profile, operation)) {
               self->FailHandoffContinuation(profile, operation, std::move(callback));
               return;
             }
            operation->service_instance = instance;
            operation->token = token;
            for (auto* partition : operation->participants)
              partition->SetNetworkContextHandoffTransaction(token);
            self->StageProfileHandoff(profile, std::move(callback), true, hook);
          }, weak_factory_.GetWeakPtr(), profile, operation, std::move(split_begin.first),
          activation_hook), base::BindOnce(
             [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
                std::shared_ptr<ProfileStorageHandoff> operation,
                base::OnceCallback<void(bool)> callback) {
               if (!self) return;
               auto owner = self->handoffs_.find(profile);
               if (owner == self->handoffs_.end() || owner->second != operation) {
                 std::move(callback).Run(false);
                 return;
               }
               self->AbortProfileHandoff(profile,
                   base::BindOnce(std::move(callback), false));
             }, weak_factory_.GetWeakPtr(), profile, operation,
             std::move(split_begin.second))));
    return;
  }
  auto success = std::make_shared<bool>(true);
  auto done = base::BarrierClosure(operation->participants.size(), base::BindOnce(
      [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
         std::shared_ptr<ProfileStorageHandoff> operation, bool activate,
         std::shared_ptr<bool> ok,
          base::OnceCallback<void(bool)> callback) {
         if (!self) return;
         if (!self->OwnsHandoff(profile, operation)) {
           self->FailHandoffContinuation(profile, operation, std::move(callback));
           return;
         }
        if (*ok) {
           if (!activate) {
             if (operation->phase == ProfileStorageHandoff::Phase::kRestoring)
                operation->Transition(ProfileStorageHandoff::Phase::kRestoredStaged);
            std::move(callback).Run(true);
            return;
          }
          if (!self->ProfileHandoffRestored(profile)) {
            self->AbortProfileHandoff(profile, base::BindOnce(std::move(callback), false));
            return;
          }
          const auto* entry = self->registry_->Find(operation->key);
          auto owner = self->handoffs_.find(profile);
          if (!entry || entry->lifecycle_epoch != operation->epoch ||
              entry->persona_input_json != operation->intent ||
              owner == self->handoffs_.end() || owner->second != operation) {
            self->AbortProfileHandoff(profile,
                base::BindOnce(std::move(callback), false));
            return;
          }
           // Every snapshot becomes nonreplayable BEFORE sending the only release.
           operation->Transition(ProfileStorageHandoff::Phase::kServicePrepared);
           operation->Transition(ProfileStorageHandoff::Phase::kReleasePossible);
          for (auto* partition : operation->participants)
            partition->SetNetworkContextHandoffReleasePossible(true);
          auto split = base::SplitOnceCallback(std::move(callback));
          operation->transaction.set_disconnect_handler(base::BindOnce(
               [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
                  std::shared_ptr<ProfileStorageHandoff> operation,
                  base::OnceCallback<void(bool)> callback) {
                 if (!self) return;
                 auto owner = self->handoffs_.find(profile);
                 if (owner == self->handoffs_.end() || owner->second != operation) {
                   std::move(callback).Run(false);
                   return;
                 }
                 self->AbortProfileHandoff(profile,
                     base::BindOnce(std::move(callback), false));
               }, self, profile, operation, std::move(split.first)));
           if (self->handoff_release_for_testing_)
             self->handoff_release_for_testing_.Run(operation->transaction.get());
           operation->transaction->Release(base::BindOnce(
              [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
                 std::shared_ptr<ProfileStorageHandoff> operation,
                 base::OnceCallback<void(bool)> callback,
                 network::mojom::StorageHandoffReleaseResult result) {
                 if (!self) return;
                 // A teardown already owns terminal completion. In particular,
                 // a late Released reply cannot resurrect quarantined state.
                  if (operation->teardown_started) {
                    self->AbortProfileHandoff(profile,
                        base::BindOnce(std::move(callback), false));
                   return;
                 }
                operation->transaction.set_disconnect_handler(base::DoNothing());
                if (result == network::mojom::StorageHandoffReleaseResult::kReleased) {
                   operation->Transition(ProfileStorageHandoff::Phase::kReleased);
                  // Retire every browser snapshot in this task, before any
                  // later lifecycle validation or admission callback can fail.
                   for (auto* partition : operation->participants)
                     partition->FinalizeNetworkContextHandoff(true);
                   if (!self->OwnsHandoff(profile, operation)) {
                     self->FailHandoffContinuation(profile, operation,
                                                   std::move(callback));
                     return;
                   }
                   std::move(callback).Run(true);
                } else {
                  // Explicit rejection certifies that no receiver was bound.
                   for (auto* partition : operation->participants)
                     partition->SetNetworkContextHandoffReleasePossible(false);
                   operation->Transition(ProfileStorageHandoff::Phase::kAborting);
                  self->AbortProfileHandoff(profile,
                      base::BindOnce(std::move(callback), false));
                }
              }, self, profile, operation, std::move(split.second)));
          return;
        }
        self->AbortProfileHandoff(profile, base::BindOnce(std::move(callback), false));
      }, weak_factory_.GetWeakPtr(), profile, operation, activate, success, std::move(callback)));
  for (auto* partition : operation->participants) {
    auto done_partition = base::BindOnce(
        [](std::shared_ptr<bool> ok, base::RepeatingClosure done, bool success) {
          *ok &= success;
          done.Run();
        }, success, done);
    if (activate && activation_hook)
      activation_hook.Run(partition, std::move(done_partition));
    else if (activate)
      partition->ActivateNetworkContextHandoff(std::move(done_partition));
    else if (operation->phase == ProfileStorageHandoff::Phase::kRegistryCommitted)
      partition->FenceNetworkContextHandoff(std::move(done_partition));
    else if (!OwnsHandoff(profile, operation) || !operation->retirement_complete ||
             !operation->retirement_succeeded ||
             operation->phase != ProfileStorageHandoff::Phase::kRestoring) {
      std::move(done_partition).Run(false);
    } else {
      partition->StageNetworkContextHandoff(
          MakeHandoffProof(profile, partition, operation),
          std::move(done_partition));
    }
  }
}

void BrowserProfileRouter::RetireProfileContexts(Profile* profile,
                            std::string key, int64_t epoch, std::string intent,
                            base::OnceCallback<void(bool)> callback) {
  CHECK(!handoffs_.contains(profile));
  PauseManagedNetworkAdmission(profile);
  auto operation = std::make_shared<ProfileStorageHandoff>();
  operation->key = std::move(key);
  operation->epoch = epoch;
  operation->intent = std::move(intent);
  operation->keep_alive = std::make_unique<ScopedProfileKeepAlive>(
      profile, ProfileKeepAliveOrigin::kBrowserWindow);
  handoffs_[profile] = operation;
  auto& partitions = operation->participants;
  profile->ForEachLoadedStoragePartition(
      [&partitions](content::StoragePartition* partition) {
        partitions.push_back(partition);
      });
  for (auto* partition : partitions)
    operation->snapshots.push_back(partition->BeginNetworkContextHandoffState());
  auto success = std::make_shared<bool>(true);
  auto done = base::BarrierClosure(
      partitions.size(), base::BindOnce(
           [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
              std::shared_ptr<ProfileStorageHandoff> operation,
              std::shared_ptr<bool> success,
              base::OnceCallback<void(bool)> callback) {
               operation->retirement_complete = true;
               operation->retirement_succeeded = *success;
              if (operation->teardown_complete) return;
             if (!self) {
               if (operation->teardown_started && !operation->teardown_complete)
                 StartHandoffTeardown(operation);
               return;
             }
             if (operation->teardown_started) {
               operation->teardown_waiters.push_back(
                   base::BindOnce(std::move(callback), false));
               self->StartHandoffTeardown(operation);
               return;
             }
              if (!self->OwnsHandoff(profile, operation)) {
                self->FailHandoffContinuation(profile, operation, std::move(callback));
               return;
             }
             std::move(callback).Run(*success);
           }, weak_factory_.GetWeakPtr(), profile, operation, success, std::move(callback)));
  for (auto* partition : partitions) {
    partition->RetireNetworkContext(base::BindOnce(
        [](std::shared_ptr<bool> success, base::RepeatingClosure done, bool ok) {
          *success &= ok;
          done.Run();
        }, success, done));
  }
}

bool BrowserProfileRouter::FinishHandoffFailure(
    const std::string& key, base::OnceClosure callback) {
  for (const auto& [profile, operation] : handoffs_) {
    if (operation->key != key) continue;
    operation->failure_waiters.push_back(std::move(callback));
    if (operation->failure_finishing) return true;
    operation->failure_finishing = true;
    AbortProfileHandoff(profile, base::BindOnce(
        [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
           std::shared_ptr<ProfileStorageHandoff> operation) {
          if (!self) return;
          self->registry_->ReinstatePersonaPending(operation->key,
              operation->epoch, operation->intent, base::BindOnce(
                [](base::WeakPtr<BrowserProfileRouter> self, Profile* profile,
                    std::shared_ptr<ProfileStorageHandoff> operation,
                   base::expected<BrowserProfileEntry, BrowserProfileFailure>) {
                  if (!self) return;
                   auto owner = self->handoffs_.find(profile);
                   if (owner == self->handoffs_.end() || owner->second != operation)
                     return;
                   self->handoffs_.erase(owner);
                   auto waiters = std::move(operation->failure_waiters);
                   for (auto& waiter : waiters) std::move(waiter).Run();
                 }, self, profile, operation));
         }, weak_factory_.GetWeakPtr(), profile, operation));
    return true;
  }
  return false;
}

// --- Free functions ----------------------------------------------------------

const std::string& ProcessBrowserEpoch() {
  static const base::NoDestructor<std::string> epoch(
      base::Uuid::GenerateRandomV4().AsLowercaseString());
  return *epoch;
}

base::DictValue BuildCapabilities() {
  BrowserProfileRouter* router = BrowserProfileRouter::Get();
  const bool supported = IsolatedProfilesBootstrapSafe();
  const bool enabled = supported && router && router->IsAvailable();
  const bool scoped_routing = enabled && router->scoped_routing_ready();
  return base::DictValue()
      .Set("capabilitiesVersion", 1)
      .Set("isolatedProfiles", base::DictValue()
                                   .Set("supported", supported)
                                   .Set("enabled", enabled))
      .Set("scopedRouting", scoped_routing)
      .Set("profileScopeMetadata", "shuttle/profileId")
      .Set("bindingVersion", 1)
      .Set("browserEpoch", ProcessBrowserEpoch());
}

// --- Value types -----------------------------------------------------------------

ProfileRecord::ProfileRecord() = default;
ProfileRecord::ProfileRecord(const ProfileRecord&) = default;
ProfileRecord& ProfileRecord::operator=(const ProfileRecord&) = default;
ProfileRecord::~ProfileRecord() = default;

base::DictValue ProfileRecord::ToValue() const {
  base::Value summary;
  if (!persona_summary_json.empty()) {
    if (std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
            persona_summary_json, base::JSON_PARSE_RFC)) {
      summary = base::Value(std::move(*dict));
    }
  }
  return base::DictValue()
      .Set("coreProfileKey", core_profile_key)
      .Set("workspaceAgent", workspace_agent)
      .Set("lifecycleEpoch", static_cast<int>(lifecycle_epoch))
      .Set("browserEpoch", browser_epoch)
      .Set("lifecycle", lifecycle)
      .Set("loaded", loaded)
      .Set("deletionPending", deletion_pending)
      .Set("personaSummary", std::move(summary))
      .Set("lastErrorCode", last_error_code);
}

ProfileList::ProfileList() = default;
ProfileList::ProfileList(const ProfileList&) = default;
ProfileList& ProfileList::operator=(const ProfileList&) = default;
ProfileList::~ProfileList() = default;

base::DictValue ProfileList::ToValue() const {
  base::ListValue list;
  for (const ProfileRecord& record : profiles) {
    list.Append(record.ToValue());
  }
  return base::DictValue()
      .Set("profiles", std::move(list))
      .Set("browserEpoch", browser_epoch);
}

CreateProfileArgs::CreateProfileArgs() = default;
CreateProfileArgs::CreateProfileArgs(CreateProfileArgs&&) = default;
CreateProfileArgs& CreateProfileArgs::operator=(CreateProfileArgs&&) = default;
CreateProfileArgs::~CreateProfileArgs() = default;

BrowserProfileRouter::Runtime::Runtime() = default;
BrowserProfileRouter::Runtime::Runtime(Runtime&&) = default;
BrowserProfileRouter::Runtime& BrowserProfileRouter::Runtime::operator=(
    Runtime&&) = default;
BrowserProfileRouter::Runtime::~Runtime() = default;

// --- Lifetime ----------------------------------------------------------------------

// static
bool BrowserProfileRouter::IsEnabledBySwitch() {
  return base::CommandLine::InitializedForCurrentProcess() &&
         base::CommandLine::ForCurrentProcess()->HasSwitch(
             kIsolatedProfilesSwitch);
}

// static
BrowserProfileRouter* BrowserProfileRouter::Initialize(
    const base::FilePath& user_data_dir) {
  if (!IsEnabledBySwitch()) {
    return nullptr;
  }
  CHECK(!g_router);
  g_router = new BrowserProfileRouter(user_data_dir);
  return g_router;
}

// static
void BrowserProfileRouter::Shutdown() {
  delete g_router;
  g_router = nullptr;
}

// static
BrowserProfileRouter* BrowserProfileRouter::Get() {
  return g_router;
}

BrowserProfileRouter::BrowserProfileRouter(const base::FilePath& user_data_dir)
    : user_data_dir_(user_data_dir), browser_epoch_(ProcessBrowserEpoch()) {
  // Startup, before the first Profile: reading two small files is accepted
  // here, as for the network access token (network_access.cc). No
  // ScopedAllowBlocking: it is only available to its friend list, and blocking
  // is still allowed on this thread during startup.
  const bool admin_ok = InitBrowserAdminAuth(user_data_dir_);
  BrowserProfileParseResult parsed =
      BrowserProfileRegistry::ReadManifestFile(user_data_dir_);
  if (!IsolatedProfilesBootstrapSafe() ||
      parsed.error == BrowserProfileParseError::kUnknownVersion) {
    // A manifest of a newer core is never interpreted nor overwritten (code
    // rollback keeps the metadata intact).
    failure_ = BrowserProfileError::kUnsupported;
  } else if (parsed.error != BrowserProfileParseError::kNone || !admin_ok) {
    failure_ = BrowserProfileError::kStorageError;
  }
  if (failure_ != BrowserProfileError::kNone) {
    LOG(ERROR) << "Shuttle: isolated profiles are disabled (registry or admin "
                  "credential unusable); the file is left untouched.";
    return;
  }

  registry_ = std::make_unique<BrowserProfileRegistry>(
      std::move(parsed.manifest),
      CreateFileManifestWriter(
          user_data_dir_.AppendASCII(kBrowserProfilesFileName),
          base::ThreadPool::CreateSequencedTaskRunner(
              {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
               base::TaskShutdownBehavior::BLOCK_SHUTDOWN})));
  SetManagedProfileProvider(this);

  // The encryptor that protects a proxy password written into the manifest.
  if (os_crypt_async::OSCryptAsync* os_crypt =
          g_browser_process->os_crypt_async()) {
    os_crypt->GetInstance(base::BindOnce(
        [](base::WeakPtr<BrowserProfileRouter> self,
           scoped_refptr<os_crypt_async::Encryptor> encryptor) {
          if (self) {
            self->encryptor_ = std::move(encryptor);
          }
        },
        weak_factory_.GetWeakPtr()));
  }
  SanitizeLastUsedProfile();
}

BrowserProfileRouter::~BrowserProfileRouter() {
  // Shutdown is cancellation, never activation/replay. Detach all continuations
  // before closing owner pipes; each teardown barrier retains its operation and
  // Profile keepalive until the lifetime fences close. Registry callbacks must
  // not dereference a registry destroyed with this router.
  weak_factory_.InvalidateWeakPtrs();
  for (const auto& [profile, operation] : handoffs_)
    AbortProfileHandoff(profile, base::DoNothing());
  handoffs_.clear();
  if (registry_) {
    SetManagedProfileProvider(nullptr);
    SanitizeLastUsedProfile();
  }
  // Releases the keep-alives while the Profiles still exist (extra parts run
  // PostMainMessageLoopRun before the browser tear down).
  runtimes_.clear();
}

// --- Startup / resolution -------------------------------------------------------------

void BrowserProfileRouter::CaptureStartupProfile(Profile* profile) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (startup_profile_ || !profile) {
    return;
  }
  startup_profile_ = profile;
  profile_observations_.AddObservation(profile);
  if (registry_) {
    registry_->RecordStartupProfileDir(profile->GetPath().BaseName().AsUTF8Unsafe());
    // Directory removals that a crash interrupted: ask Chromium again once
    // startup settled.
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BrowserProfileRouter::ReconcileTombstones,
                       weak_factory_.GetWeakPtr(), /*reschedule=*/true),
        base::Seconds(15));
  }
}

BrowserProfileFailure BrowserProfileRouter::UnavailableFailure() const {
  return BrowserProfileFailure(failure_ == BrowserProfileError::kNone
                                   ? BrowserProfileError::kUnsupported
                                   : failure_);
}

base::FilePath BrowserProfileRouter::PathForDirectory(
    const std::string& directory) const {
  // The directory name was validated by the registry (no separators, no dots);
  // the path is only ever user_data_dir_/<name>.
  return user_data_dir_.Append(base::FilePath::FromUTF8Unsafe(directory));
}

base::expected<ResolvedProfile, BrowserProfileFailure>
BrowserProfileRouter::Resolve(const std::optional<std::string>& key) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!key) {
    if (!startup_profile_) {
      return base::unexpected(
          BrowserProfileFailure(BrowserProfileError::kProfileNotReady));
    }
    ResolvedProfile resolved;
    resolved.profile = startup_profile_;
    resolved.is_startup = true;
    return resolved;
  }
  if (!IsValidBrowserProfileKey(*key)) {
    return base::unexpected(
        BrowserProfileFailure(BrowserProfileError::kInvalidProfileKey));
  }
  if (!registry_) {
    return base::unexpected(UnavailableFailure());
  }
  const BrowserProfileEntry* entry = registry_->Find(*key);
  if (!entry || entry->state == BrowserProfileState::kDeleted) {
    return base::unexpected(
        BrowserProfileFailure(BrowserProfileError::kProfileNotFound));
  }
  auto it = runtimes_.find(*key);
  if (entry->state != BrowserProfileState::kReady || load_waiters_.contains(*key) ||
      drains_in_flight_.contains(*key) || it == runtimes_.end() ||
      !it->second.profile) {
    return base::unexpected(
        BrowserProfileFailure(BrowserProfileError::kProfileNotReady));
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  if (!manager || manager->GetProfileByPath(PathForDirectory(entry->directory)) !=
                      it->second.profile) {
    return base::unexpected(
        BrowserProfileFailure(BrowserProfileError::kProfileNotReady));
  }
  ResolvedProfile resolved;
  resolved.profile = it->second.profile;
  resolved.core_profile_key = entry->key;
  resolved.workspace_agent = entry->workspace_agent;
  resolved.lifecycle_epoch = entry->lifecycle_epoch;
  resolved.is_startup = false;
  return resolved;
}

std::string BrowserProfileRouter::KeyOfProfile(Profile* profile) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const auto& [key, runtime] : runtimes_) {
    if (runtime.profile == profile) {
      return key;
    }
  }
  return std::string();
}

OwnerTuple BrowserProfileRouter::MakeOwner(
    const ResolvedProfile& resolved,
    const std::string& workspace_agent) const {
  OwnerTuple owner;
  owner.profile_key = resolved.core_profile_key;
  owner.workspace_agent =
      resolved.is_startup ? workspace_agent : resolved.workspace_agent;
  owner.lifecycle_epoch = resolved.lifecycle_epoch;
  owner.browser_epoch = browser_epoch_;
  return owner;
}

// --- Records -----------------------------------------------------------------------------

ProfileRecord BrowserProfileRouter::MakeRecord(
    const BrowserProfileEntry& entry) const {
  ProfileRecord record;
  record.core_profile_key = entry.key;
  record.workspace_agent = entry.workspace_agent;
  record.lifecycle_epoch = entry.lifecycle_epoch;
  record.browser_epoch = browser_epoch_;
  record.lifecycle = std::string(BrowserProfileStateToString(entry.state));
  if (drains_in_flight_.contains(entry.key)) {
    record.lifecycle = "draining";
  } else if (entry.state == BrowserProfileState::kReady &&
             load_waiters_.contains(entry.key)) {
    record.lifecycle = "initializing";
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  record.loaded = manager && manager->GetProfileByPath(
                                 PathForDirectory(entry.directory)) != nullptr;
  record.deletion_pending = entry.deletion_pending;
  // Committed summary (no seed); before the first load, the validated input.
  record.persona_summary_json = entry.persona_summary_json.empty()
                                    ? entry.persona_input_json
                                    : entry.persona_summary_json;
  record.last_error_code =
      std::string(BrowserProfileErrorCode(entry.last_error));
  return record;
}

// static
void BrowserProfileRouter::RunLater(
    ProfileRecordCallback callback,
    base::expected<ProfileRecord, BrowserProfileFailure> result) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(result)));
}

base::expected<ProfileList, BrowserProfileFailure>
BrowserProfileRouter::ListProfiles() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!registry_) {
    return base::unexpected(UnavailableFailure());
  }
  ProfileList list;
  list.browser_epoch = browser_epoch_;
  for (const BrowserProfileEntry& entry : registry_->List()) {
    // A tombstone whose removal is confirmed is gone; a pending one stays
    // visible as `deleted` with deletionPending.
    if (entry.state == BrowserProfileState::kDeleted &&
        !entry.deletion_pending) {
      continue;
    }
    list.profiles.push_back(MakeRecord(entry));
  }
  ReconcileTombstones(/*reschedule=*/false);
  return list;
}

// --- create ---------------------------------------------------------------------------------

void BrowserProfileRouter::CreateProfile(CreateProfileArgs args,
                                         ProfileRecordCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!registry_) {
    RunLater(std::move(callback), base::unexpected(UnavailableFailure()));
    return;
  }
  auto fail = [&](BrowserProfileError code, std::string message) {
    RunLater(std::move(callback),
             base::unexpected(BrowserProfileFailure(code, std::move(message))));
  };

  base::Uuid request_uuid = base::Uuid::ParseCaseInsensitive(args.request_id);
  if (!request_uuid.is_valid()) {
    fail(BrowserProfileError::kProfileFailed, "requestId is invalid.");
    return;
  }
  args.request_id = request_uuid.AsLowercaseString();
  if (!IsValidWorkspaceAgentSlug(args.workspace_agent)) {
    fail(BrowserProfileError::kProfileFailed, "workspaceAgent is invalid.");
    return;
  }

  // Persona: validated now with a throwaway seed (the committed seed is drawn
  // at the first load and never leaves the Profile prefs).
  const base::DictValue* persona_dict = args.persona.GetIfDict();
  if (!persona_dict) {
    fail(BrowserProfileError::kPersonaInvalid, "Persona is invalid.");
    return;
  }
  FingerprintPersona probe;
  std::string persona_error =
      ParseFingerprintPersonaInput(args.persona, /*current=*/nullptr,
                                   /*seed=*/1, &probe);
  if (persona_error.empty()) {
    persona_error = CheckFingerprintPersona(probe);
  }
  if (!persona_error.empty()) {
    fail(BrowserProfileError::kPersonaInvalid, std::move(persona_error));
    return;
  }
  std::string persona_json = base::WriteJson(*persona_dict).value_or("");

  // Proxy: validated, then stored ENCRYPTED. The plaintext only feeds the
  // idempotency fingerprint.
  ProxySpec proxy_spec;
  std::string proxy_plain_json;
  if (args.proxy) {
    std::string proxy_error = ParseProxyValue(*args.proxy, &proxy_spec);
    if (proxy_error.empty()) {
      proxy_error = CheckProxy(proxy_spec);
    }
    if (!proxy_error.empty()) {
      // The messages of the proxy parsers never contain the password.
      fail(BrowserProfileError::kProfileFailed, std::move(proxy_error));
      return;
    }
    proxy_plain_json = base::WriteJson(*args.proxy).value_or("");
  }
  std::string fingerprint = ComputeCreateRequestFingerprint(
      args.workspace_agent, persona_json, proxy_plain_json);

  // Replay of a known requestId: answered by the registry (same arguments =
  // the same entry, different arguments = request_conflict) before anything
  // is allocated or encrypted.
  if (registry_->FindByRequestId(args.request_id)) {
    BrowserProfileRegistry::CreateRequest replay;
    replay.request_id = args.request_id;
    replay.request_fingerprint = fingerprint;
    registry_->Create(std::move(replay),
                      base::BindOnce(&BrowserProfileRouter::OnCreated,
                                     weak_factory_.GetWeakPtr(),
                                     std::move(callback)));
    return;
  }

  std::string proxy_pref_json;
  if (args.proxy) {
    std::optional<base::DictValue> stored =
        ProxyToPrefValue(proxy_spec, encryptor_.get());
    if (!stored) {
      fail(BrowserProfileError::kProxyNotReady, "Proxy is not ready.");
      return;
    }
    proxy_pref_json = base::WriteJson(*stored).value_or("");
  }
  AllocateDirectory(std::move(args), std::move(persona_json),
                    std::move(proxy_pref_json), std::move(fingerprint),
                    std::move(callback), kMaxDirectoryAttempts);
}

void BrowserProfileRouter::AllocateDirectory(CreateProfileArgs args,
                                             std::string persona_json,
                                             std::string proxy_pref_json,
                                             std::string fingerprint,
                                             ProfileRecordCallback callback,
                                             int attempts_left) {
  if (attempts_left <= 0) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  if (!manager || !profiles::IsMultipleProfilesEnabled()) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kUnsupported));
    return;
  }
  // Chromium names the directory ("Profile <n>"); the name is not derived from
  // the key, the workspace slug or anything the client sent.
  base::FilePath path = manager->GenerateNextProfileDirectoryPath();
  std::string directory = path.BaseName().AsUTF8Unsafe();
  if (path.DirName() != user_data_dir_ || !IsAllocatedDirectoryName(directory) ||
      registry_->FindByDirectory(directory) ||
      manager->GetProfileByPath(path)) {
    AllocateDirectory(std::move(args), std::move(persona_json),
                      std::move(proxy_pref_json), std::move(fingerprint),
                      std::move(callback), attempts_left - 1);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([](base::FilePath p) { return base::PathExists(p); },
                     path),
      base::BindOnce(&BrowserProfileRouter::OnDirectoryChecked,
                     weak_factory_.GetWeakPtr(), std::move(args),
                     std::move(persona_json), std::move(proxy_pref_json),
                     std::move(fingerprint), std::move(directory),
                     std::move(callback), attempts_left));
}

void BrowserProfileRouter::OnDirectoryChecked(CreateProfileArgs args,
                                              std::string persona_json,
                                              std::string proxy_pref_json,
                                              std::string fingerprint,
                                              std::string directory,
                                              ProfileRecordCallback callback,
                                              int attempts_left,
                                              bool exists) {
  if (exists || registry_->FindByDirectory(directory)) {
    // A leftover directory (or a concurrent create took the name): take the
    // next one. Never reuse a directory that is not ours.
    AllocateDirectory(std::move(args), std::move(persona_json),
                      std::move(proxy_pref_json), std::move(fingerprint),
                      std::move(callback), attempts_left - 1);
    return;
  }
  BrowserProfileRegistry::CreateRequest request;
  do {
    request.key = GenerateBrowserProfileKey();
  } while (registry_->Find(request.key));
  request.request_id = args.request_id;
  request.directory = std::move(directory);
  request.workspace_agent = args.workspace_agent;
  request.persona_input_json = std::move(persona_json);
  request.proxy_pref_json = std::move(proxy_pref_json);
  request.request_fingerprint = std::move(fingerprint);
  registry_->Create(std::move(request),
                    base::BindOnce(&BrowserProfileRouter::OnCreated,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(callback)));
}

void BrowserProfileRouter::OnCreated(
    ProfileRecordCallback callback,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    std::move(callback).Run(base::unexpected(result.error()));
    return;
  }
  std::move(callback).Run(MakeRecord(*result));
}

// --- load ----------------------------------------------------------------------------------------

void BrowserProfileRouter::LoadProfile(const std::string& key,
                                       ProfileRecordCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (indeterminate_handoffs_.contains(key)) {
    BrowserProfileFailure failure(BrowserProfileError::kProfileFailed);
    failure.message = "Network handoff indeterminate; admission remains closed; snapshot replay is prohibited.";
    RunLater(std::move(callback), base::unexpected(failure));
    return;
  }
  if (!IsValidBrowserProfileKey(key)) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kInvalidProfileKey));
    return;
  }
  if (!registry_) {
    RunLater(std::move(callback), base::unexpected(UnavailableFailure()));
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state == BrowserProfileState::kDeleted) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kProfileNotFound));
    return;
  }
  if (persona_operations_.contains(key) || drains_in_flight_.contains(key)) {
    // A persona replacement of this Profile is not committed yet.
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  if (load_waiters_.contains(key)) {
    load_waiters_[key].push_back(std::move(callback));
    return;
  }
  switch (entry->state) {
    case BrowserProfileState::kReady: {
      if (load_waiters_.contains(key)) {
        load_waiters_[key].push_back(std::move(callback));
        return;
      }
      // Idempotent: no new epoch, no new load.
      auto it = runtimes_.find(key);
      ProfileManager* manager = g_browser_process->profile_manager();
      if (it == runtimes_.end() || !manager ||
          manager->GetProfileByPath(PathForDirectory(entry->directory)) !=
              it->second.profile) {
        RunLater(std::move(callback),
                 Failure(BrowserProfileError::kProfileFailed));
        return;
      }
      RunLater(std::move(callback), MakeRecord(*entry));
      return;
    }
    case BrowserProfileState::kLoading:
    case BrowserProfileState::kInitializing:
      // Concurrent loads of one key share one initialization.
      load_waiters_[key].push_back(std::move(callback));
      return;
    case BrowserProfileState::kDraining:
      RunLater(std::move(callback),
               Failure(BrowserProfileError::kProfileBusy));
      return;
    case BrowserProfileState::kStopped:
    case BrowserProfileState::kFailed:
      load_waiters_[key].push_back(std::move(callback));
      BeginLoad(key);
      return;
    case BrowserProfileState::kDeleted:
      break;
  }
  RunLater(std::move(callback), Failure(BrowserProfileError::kProfileNotFound));
}

void BrowserProfileRouter::BeginLoad(const std::string& key) {
  // The lifecycle epoch is incremented and persisted BEFORE the load starts.
  const auto* entry = registry_->Find(key);
  CHECK(entry);
  const int64_t epoch = entry->lifecycle_epoch + 1;
  const std::string intent = entry->persona_input_json;
  registry_->Transition(key, BrowserProfileState::kLoading,
                        BrowserProfileError::kNone,
                        base::BindOnce(&BrowserProfileRouter::OnLoadingPersisted,
                                        weak_factory_.GetWeakPtr(), key, epoch, intent));
}

void BrowserProfileRouter::OnLoadingPersisted(
    std::string key, int64_t epoch, std::string intent,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  const auto* owner = registry_->Find(key);
  // A failed write may roll back this attempt's increment, but must not settle
  // a later load's waiters. The busy load lock prevents another same-epoch op.
  if (!owner || owner->persona_input_json != intent ||
      owner->lifecycle_epoch > epoch || !load_waiters_.contains(key)) return;
  if (!result.has_value()) {
    // Rolled back to the previous durable state: nothing was started.
    CompleteLoadFailure(key, result.error());
    return;
  }
  const auto* current = registry_->Find(key);
  if (!current || current->lifecycle_epoch != result->lifecycle_epoch ||
      current->persona_input_json != result->persona_input_json ||
      !load_waiters_.contains(key)) return;
  ProfileManager* manager = g_browser_process->profile_manager();
  if (!manager) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  bootstrap_errors_.erase(result->directory);
  if (result->persona_replacement_pending) {
    if (Profile* loaded =
            manager->GetProfileByPath(PathForDirectory(result->directory))) {
      PauseManagedNetworkAdmission(loaded);
    }
  }
  // A regular Profile through ProfileManager's lifecycle: OnPrefsLoaded runs
  // PrepareManagedProfile() (via LookupByDirectory, state `loading`) before
  // bulk keyed-service creation; extension preference services can exist earlier.
  manager->CreateProfileAsync(
      PathForDirectory(result->directory),
      base::BindOnce(&BrowserProfileRouter::OnProfileInitialized,
                      weak_factory_.GetWeakPtr(), key, result->lifecycle_epoch,
                      result->persona_input_json));
}

void BrowserProfileRouter::OnProfileInitialized(std::string key, int64_t epoch,
                                                std::string intent,
                                                 Profile* profile) {
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent || !load_waiters_.contains(key)) return;
  if (!entry || entry->state != BrowserProfileState::kLoading) {
    CompleteLoadFailure(key,
                        BrowserProfileFailure(BrowserProfileError::kProfileFailed));
    return;
  }
  if (!profile) {
    BrowserProfileError error = BrowserProfileError::kProfileFailed;
    if (auto it = bootstrap_errors_.find(entry->directory);
        it != bootstrap_errors_.end() &&
        it->second != BrowserProfileError::kNone) {
      error = it->second;
      bootstrap_errors_.erase(it);
    }
    FailLoad(key, error);
    return;
  }
  // CreateProfileAsync returns an existing Profile without OnPrefsLoaded.
  // A failed live replacement therefore MUST be retried here. A valid OLD
  // persona is not proof of applying the durable pending intent.
  if (!entry->persona_replacement_pending &&
      !ManagedNetworkAdmissionReady(profile)) {
    // Failed compensation/persistence must not become ordinary reconnect.
    FailLoad(key, BrowserProfileError::kStorageError);
    return;
  }
  if (entry->persona_replacement_pending) {
    RetireProfileContexts(profile, key, entry->lifecycle_epoch,
        entry->persona_input_json, base::BindOnce(
        &BrowserProfileRouter::OnLoadedContextsRetired,
        weak_factory_.GetWeakPtr(), key, entry->lifecycle_epoch,
        entry->persona_input_json, profile->GetWeakPtr()));
    return;
  }
  OnProfileIdentityReady(key, profile);
}

void BrowserProfileRouter::OnLoadedContextsRetired(
    std::string key, int64_t epoch, std::string intent,
    base::WeakPtr<Profile> profile, bool success) {
  const auto* entry = registry_->Find(key);
  // Superseded completions own neither the current load waiters nor its lock.
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent) return;
  if (!success || !profile || !entry ||
      entry->state != BrowserProfileState::kLoading ||
      entry->lifecycle_epoch != epoch || entry->persona_input_json != intent) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  {
    if (!ManagedProfilePersonaMatchesIntent(profile.get(), intent)) {
      auto input = base::JSONReader::ReadDict(entry->persona_input_json,
                                            base::JSON_PARSE_RFC);
      FingerprintPersona persona;
      auto* service = ProfilePersonaServiceFactory::GetForProfileIfExists(profile.get());
      if (!input || !service ||
          !ParseFingerprintPersonaInput(base::Value(std::move(*input)), nullptr,
                                        base::RandUint64(), &persona).empty() ||
          !CheckFingerprintPersona(persona).empty() || !service->Set(persona)) {
        FailLoad(key, BrowserProfileError::kPersonaInvalid);
        return;
      }
    }
    // Even an exact match needs a commit: the previous failure may have been
    // between Set and its async flush. Cold bootstrap matches without changing
    // the seed again after its first NetworkContext was admitted.
    profile->GetPrefs()->CommitPendingWrite(base::BindOnce(
        &BrowserProfileRouter::OnLoadedPersonaRecovered,
        weak_factory_.GetWeakPtr(), key, entry->lifecycle_epoch,
        entry->persona_input_json, profile));
    return;
  }
}

void BrowserProfileRouter::OnProfileIdentityReady(std::string key,
                                                  Profile* profile) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kLoading ||
      !load_waiters_.contains(key)) return;
  Runtime runtime;
  runtime.profile = profile;
  runtime.keep_alive = std::make_unique<ScopedProfileKeepAlive>(
      profile, ProfileKeepAliveOrigin::kRemoteDebugging);
  runtimes_[key] = std::move(runtime);
  if (!profile_observations_.IsObservingSource(profile)) {
    profile_observations_.AddObservation(profile);
  }
  registry_->Transition(
      key, BrowserProfileState::kInitializing, BrowserProfileError::kNone,
      base::BindOnce(&BrowserProfileRouter::OnInitializingPersisted,
                      weak_factory_.GetWeakPtr(), key, entry->lifecycle_epoch,
                      entry->persona_input_json, profile->GetWeakPtr()));
}

void BrowserProfileRouter::OnInitializingPersisted(
    std::string key, int64_t epoch, std::string intent,
    base::WeakPtr<Profile> profile,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent || !load_waiters_.contains(key)) return;
  if (!result.has_value()) {
    FailLoad(key, BrowserProfileError::kStorageError);
    return;
  }
  if (!profile) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  // Ready only once the identity components are really in place.
  ManagedProfileIntent identity;
  identity.core_profile_key = result->key;
  identity.workspace_agent = result->workspace_agent;
  identity.persona_replacement_pending = result->persona_replacement_pending;
  identity.persona_input_json = result->persona_input_json;
  BrowserProfileError error =
      VerifyManagedProfileIdentity(profile.get(), identity);
  if (error != BrowserProfileError::kNone) {
    FailLoad(key, error);
    return;
  }
  // Clear intent conditionally BEFORE opening ready admission. An optimistic
  // ready transition must not allow routing while provisioning persistence is
  // still outstanding.
  OnReadyPersisted(key, std::move(result));
}

void BrowserProfileRouter::OnLoadedPersonaRecovered(
    std::string key, int64_t epoch, std::string intent,
    base::WeakPtr<Profile> profile) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent) return;
  if (!entry || entry->state != BrowserProfileState::kLoading ||
      entry->lifecycle_epoch != epoch || entry->persona_input_json != intent ||
      !profile || !ManagedProfilePersonaMatchesIntent(profile.get(), intent)) {
    FailLoad(key, BrowserProfileError::kPersonaInvalid);
    return;
  }
  StageProfileHandoff(profile.get(), base::BindOnce(
      [](base::WeakPtr<BrowserProfileRouter> self, std::string key,
          int64_t epoch, std::string intent,
          std::shared_ptr<ProfileStorageHandoff> operation,
          base::WeakPtr<Profile> profile, bool success) {
         if (!self) return;
         const auto* entry = self->registry_->Find(key);
         if (!entry || entry->lifecycle_epoch != epoch ||
             entry->persona_input_json != intent) return;
         if (profile) {
           auto owner = self->handoffs_.find(profile.get());
           if (owner == self->handoffs_.end() || owner->second != operation) return;
         }
        if (!success || !profile) {
          self->FailLoad(key, BrowserProfileError::kProfileFailed);
          return;
        }
        self->OnProfileIdentityReady(key, profile.get());
      }, weak_factory_.GetWeakPtr(), key, epoch, intent,
      handoffs_.at(profile.get()), profile));
}

void BrowserProfileRouter::OnReadyPersisted(
    std::string key,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    FailLoad(key, BrowserProfileError::kStorageError);
    return;
  }
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != result->lifecycle_epoch ||
      entry->persona_input_json != result->persona_input_json ||
      !load_waiters_.contains(key)) return;
  registry_->SetLoaded(key, true);
  SanitizeLastUsedProfile();
  // First load, or a persona replacement the bootstrap just committed.
  if (!result->provisioned || result->persona_replacement_pending) {
    std::string summary_json;
    auto it = runtimes_.find(key);
    if (it != runtimes_.end()) {
      if (std::optional<base::DictValue> summary =
              ManagedProfilePersonaSummary(it->second.profile)) {
        summary_json = base::WriteJson(*summary).value_or("");
      }
    }
    if (result->provisioned) {
      // A replacement the bootstrap just committed: cleared only for the
      // generation (epoch and intent) this load applied.
      registry_->MarkPersonaCommitted(
          key, result->lifecycle_epoch, result->persona_input_json,
          std::move(summary_json),
           base::BindOnce(&BrowserProfileRouter::OnProvisionedPersisted,
                          weak_factory_.GetWeakPtr(), key,
                          result->lifecycle_epoch, result->persona_input_json));
      return;
    }
    registry_->MarkProvisioned(
        key, result->lifecycle_epoch, result->persona_input_json,
        std::move(summary_json),
         base::BindOnce(&BrowserProfileRouter::OnProvisionedPersisted,
                        weak_factory_.GetWeakPtr(), key,
                        result->lifecycle_epoch, result->persona_input_json));
    return;
  }
  registry_->Transition(
      key, BrowserProfileState::kReady, BrowserProfileError::kNone,
      base::BindOnce(&BrowserProfileRouter::OnProvisionedPersisted,
                      weak_factory_.GetWeakPtr(), key,
                      result->lifecycle_epoch, result->persona_input_json));
}

void BrowserProfileRouter::OnProvisionedPersisted(
    std::string key, int64_t epoch, std::string intent,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  const auto* current = registry_->Find(key);
  if (!current || current->lifecycle_epoch != epoch ||
      current->persona_input_json != intent) return;
  if (!result.has_value()) {
    FailLoad(key, result.error().code);
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry ||
      entry->lifecycle_epoch != result->lifecycle_epoch ||
      entry->persona_input_json != result->persona_input_json) {
    // This reply cannot complete/unlock waiters belonging to a newer load.
    return;
  }
  if (entry->state == BrowserProfileState::kInitializing) {
    registry_->Transition(
        key, BrowserProfileState::kReady, BrowserProfileError::kNone,
         base::BindOnce(&BrowserProfileRouter::OnProvisionedPersisted,
                        weak_factory_.GetWeakPtr(), key, epoch, intent));
    return;
  }
  if (entry->state != BrowserProfileState::kReady) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  auto runtime = runtimes_.find(key);
  if (runtime == runtimes_.end() || !runtime->second.profile) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  StageProfileHandoff(runtime->second.profile, base::BindOnce(
      &BrowserProfileRouter::OnLoadActivationFenced,
      weak_factory_.GetWeakPtr(), key, entry->lifecycle_epoch,
      entry->persona_input_json));
}

void BrowserProfileRouter::OnLoadActivationFenced(
    std::string key, int64_t epoch, std::string intent, bool success) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent) return;
  auto runtime = runtimes_.find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent ||
      entry->state != BrowserProfileState::kReady ||
      runtime == runtimes_.end() || !runtime->second.profile) {
    FailLoad(key, BrowserProfileError::kProfileFailed);
    return;
  }
  if (runtime != runtimes_.end() && runtime->second.profile &&
      (!success || !ProfileHandoffRestored(runtime->second.profile))) {
    CompensateProfileHandoff(registry_.get(), runtime->second.profile,
        key, entry->lifecycle_epoch,
        entry->persona_input_json, base::BindOnce(
            [](base::WeakPtr<BrowserProfileRouter> self, std::string key,
               base::expected<BrowserProfileEntry, BrowserProfileFailure>) {
              if (self) self->FailLoad(key, BrowserProfileError::kProfileFailed);
            }, weak_factory_.GetWeakPtr(), key));
    return;
  }
  StageProfileHandoff(runtime->second.profile, base::BindOnce(
      &BrowserProfileRouter::OnLoadActivated, weak_factory_.GetWeakPtr(), key,
       epoch, intent), true, handoff_activate_for_testing_);
}

void BrowserProfileRouter::OnLoadActivated(
    std::string key, int64_t epoch, std::string intent, bool success) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent ||
      entry->state != BrowserProfileState::kReady) {
    return;
  }
  auto active_runtime = runtimes_.find(key);
  success = success && active_runtime != runtimes_.end() &&
            active_runtime->second.profile &&
            ProfileHandoffRestored(active_runtime->second.profile);
  if (!success) {
    auto runtime = runtimes_.find(key);
    CHECK(runtime != runtimes_.end() && runtime->second.profile);
    CompensateProfileHandoff(registry_.get(), runtime->second.profile,
        key, epoch, intent, base::BindOnce(
        [](base::WeakPtr<BrowserProfileRouter> self, std::string key,
           base::expected<BrowserProfileEntry, BrowserProfileFailure>) {
          if (self) self->FailLoad(key, BrowserProfileError::kProfileFailed);
        }, weak_factory_.GetWeakPtr(), key));
    return;
  }
  auto runtime = runtimes_.find(key);
  if (runtime != runtimes_.end() && runtime->second.profile)
    FinalizeProfileHandoff(runtime->second.profile, true);
  if (runtime != runtimes_.end() && runtime->second.profile &&
      !ManagedNetworkAdmissionReady(runtime->second.profile)) {
    FinishManagedNetworkAdmission(runtime->second.profile, true);
  }
  CompleteLoad(key);
}

void BrowserProfileRouter::FailLoad(const std::string& key,
                                    BrowserProfileError error) {
  for (const auto& [profile, operation] : handoffs_) {
    if (operation->key != key) continue;
    FinishHandoffFailure(key, base::BindOnce(&BrowserProfileRouter::FailLoad,
        weak_factory_.GetWeakPtr(), key, error));
    return;
  }
  ReleaseRuntime(key);
  registry_->Transition(
      key, BrowserProfileState::kFailed, error,
      base::BindOnce(&BrowserProfileRouter::OnFailedPersisted,
                     weak_factory_.GetWeakPtr(), key, error));
}

void BrowserProfileRouter::OnFailedPersisted(
    std::string key,
    BrowserProfileError error,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    // The disk cannot be written: still never leave it `loading`.
    registry_->ForceStateInMemory(key, BrowserProfileState::kFailed, error);
  }
  CompleteLoadFailure(key, BrowserProfileFailure(error));
}

void BrowserProfileRouter::CompleteLoad(const std::string& key) {
  std::vector<ProfileRecordCallback> waiters;
  waiters.swap(load_waiters_[key]);
  load_waiters_.erase(key);
  const BrowserProfileEntry* entry = registry_->Find(key);
  const std::optional<ProfileRecord> record =
      entry ? std::make_optional(MakeRecord(*entry)) : std::nullopt;
  for (ProfileRecordCallback& callback : waiters) {
    if (record) {
      std::move(callback).Run(*record);
    } else {
      std::move(callback).Run(Failure(BrowserProfileError::kProfileNotFound));
    }
  }
}

void BrowserProfileRouter::CompleteLoadFailure(const std::string& key,
                                               BrowserProfileFailure failure) {
  if (indeterminate_handoffs_.contains(key))
    failure.message = "Network handoff indeterminate; admission remains closed; snapshot replay is prohibited.";
  for (const auto& [profile, operation] : handoffs_) {
    if (operation->key != key) continue;
    FinishHandoffFailure(key, base::BindOnce(
        &BrowserProfileRouter::CompleteLoadFailure,
        weak_factory_.GetWeakPtr(), key, failure));
    return;
  }
  std::vector<ProfileRecordCallback> waiters;
  if (auto it = load_waiters_.find(key); it != load_waiters_.end()) {
    waiters.swap(it->second);
    load_waiters_.erase(it);
  }
  for (ProfileRecordCallback& callback : waiters) {
    std::move(callback).Run(base::unexpected(failure));
  }
}

// --- stop ------------------------------------------------------------------------------------------------

void BrowserProfileRouter::StopProfile(const std::string& key,
                                       ProfileRecordCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsValidBrowserProfileKey(key)) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kInvalidProfileKey));
    return;
  }
  if (!registry_) {
    RunLater(std::move(callback), base::unexpected(UnavailableFailure()));
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state == BrowserProfileState::kDeleted) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kProfileNotFound));
    return;
  }
  if (drains_in_flight_.contains(key)) {
    stop_waiters_[key].push_back(std::move(callback));
    if (!drain_timers_[key].IsRunning()) {
      drain_timers_[key].Start(
          FROM_HERE, drain_timeout_,
          base::BindOnce(&BrowserProfileRouter::OnDrainTimeout,
                         weak_factory_.GetWeakPtr(), key));
    }
    return;
  }
  switch (entry->state) {
    case BrowserProfileState::kStopped:
      // An idempotent stop must still never certify surviving handles.
      if (HasLiveHandles(*entry)) {
        RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
        return;
      }
      RunLater(std::move(callback), MakeRecord(*entry));
      return;
    case BrowserProfileState::kFailed:
      // Cleanup edge failed -> stopped, but never while a handle of the
      // Profile (a session, tab, worker, connection or sidecar) survives.
      if (HasLiveHandles(*entry)) {
        RunLater(std::move(callback),
                 Failure(BrowserProfileError::kProfileBusy));
        return;
      }
      stop_waiters_[key].push_back(std::move(callback));
      drains_in_flight_.insert(key);
      drain_timers_[key].Start(
          FROM_HERE, drain_timeout_,
          base::BindOnce(&BrowserProfileRouter::OnDrainTimeout,
                         weak_factory_.GetWeakPtr(), key));
      registry_->Transition(
          key, BrowserProfileState::kStopped, BrowserProfileError::kNone,
          base::BindOnce(&BrowserProfileRouter::OnStoppedPersisted,
                         weak_factory_.GetWeakPtr(), key));
      return;
    case BrowserProfileState::kReady:
      if (load_waiters_.contains(key)) {
        RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
        return;
      }
      if (!drain_delegate_) {
        // Without the layer that closes sessions/tabs/workers there is no
        // safe drain.
        RunLater(std::move(callback),
                 Failure(BrowserProfileError::kUnsupported));
        return;
      }
      stop_waiters_[key].push_back(std::move(callback));
      BeginDrain(key);
      return;
    case BrowserProfileState::kDraining:
      // Join the running drain; a new timeout covers the new caller. A drain
      // that gave up (drain_timeout) is resumed: the state stayed `draining`
      // and only a completed drain with no handle left may reach `stopped`.
      stop_waiters_[key].push_back(std::move(callback));
      drain_timers_[key].Start(
          FROM_HERE, drain_timeout_,
          base::BindOnce(&BrowserProfileRouter::OnDrainTimeout,
                         weak_factory_.GetWeakPtr(), key));
      if (!drains_in_flight_.contains(key)) {
        ResumeDrain(key);
      }
      return;
    case BrowserProfileState::kLoading:
    case BrowserProfileState::kInitializing:
      RunLater(std::move(callback),
               Failure(BrowserProfileError::kProfileBusy));
      return;
    case BrowserProfileState::kDeleted:
      break;
  }
  RunLater(std::move(callback), Failure(BrowserProfileError::kProfileNotFound));
}

void BrowserProfileRouter::BeginDrain(const std::string& key) {
  drains_in_flight_.insert(key);
  drain_timers_[key].Start(
      FROM_HERE, drain_timeout_,
      base::BindOnce(&BrowserProfileRouter::OnDrainTimeout,
                     weak_factory_.GetWeakPtr(), key));
  // `draining` closes admission at once (Resolve says profile_not_ready); the
  // epoch bump is persisted before the delegate starts closing things.
  registry_->Transition(
      key, BrowserProfileState::kDraining, BrowserProfileError::kNone,
      base::BindOnce(&BrowserProfileRouter::OnDrainingPersisted,
                     weak_factory_.GetWeakPtr(), key));
}

void BrowserProfileRouter::OnDrainingPersisted(
    std::string key,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    // Rolled back to `ready`: nothing was drained.
    drains_in_flight_.erase(key);
    CompleteStop(key, base::unexpected(result.error()));
    return;
  }
  auto it = runtimes_.find(key);
  if (it == runtimes_.end() || !it->second.profile || !drain_delegate_) {
    drains_in_flight_.erase(key);
    CompleteStop(key, Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  // Revoke every handle: no scoped session/target resolves from now on.
  RevokeOwners(key);
  const int64_t epoch = result->lifecycle_epoch;
  drains_in_flight_.insert(key);
  drain_delegate_->DrainProfile(
      it->second.profile, key, epoch,
      base::BindOnce(&BrowserProfileRouter::OnDrainDone,
                     weak_factory_.GetWeakPtr(), key, epoch));
}

bool BrowserProfileRouter::HasLiveHandles(
    const BrowserProfileEntry& entry) const {
  if (!session_owners_.SessionsForProfile(entry.key).empty()) {
    return true;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded =
      manager ? manager->GetProfileByPath(PathForDirectory(entry.directory))
              : nullptr;
  if (!loaded) {
    return false;  // Not in memory: nothing can hold a handle.
  }
  // Fail closed: a loaded Profile with no delegate to ask has unknown handles.
  return !drain_delegate_ || drain_delegate_->HasDescendantHandles(loaded);
}

void BrowserProfileRouter::ResumeDrain(const std::string& key) {
  if (drains_in_flight_.contains(key)) {
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kDraining) {
    return;
  }
  const int64_t epoch = entry->lifecycle_epoch;
  RevokeOwners(key);
  auto it = runtimes_.find(key);
  Profile* profile = it != runtimes_.end() ? it->second.profile.get() : nullptr;
  if (!profile) {
    ProfileManager* manager = g_browser_process->profile_manager();
    profile =
        manager ? manager->GetProfileByPath(PathForDirectory(entry->directory))
                : nullptr;
  }
  if (!profile) {
    // Not in memory: there is nothing to drain; finish the stop.
    drains_in_flight_.insert(key);
    OnDrainDone(key, epoch, /*success=*/true);
    return;
  }
  if (!drain_delegate_) {
    CompleteStop(key, Failure(BrowserProfileError::kUnsupported));
    return;
  }
  drains_in_flight_.insert(key);
  drain_delegate_->DrainProfile(
      profile, key, epoch,
      base::BindOnce(&BrowserProfileRouter::OnDrainDone,
                     weak_factory_.GetWeakPtr(), key, epoch));
}

void BrowserProfileRouter::OnDrainDone(std::string key,
                                       int64_t epoch,
                                       bool success) {
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kDraining ||
      entry->lifecycle_epoch != epoch) {
    return;  // A stale drain (another generation): ignore.
  }
  // `stopped` is reachable only with every handle gone: re-check, whatever
  // the delegate said.
  if (!success || HasLiveHandles(*entry)) {
    drains_in_flight_.erase(key);
    // The Profile stays `draining` (never `failed`, never `stopped`); a later
    // stop resumes the drain.
    drain_timers_.erase(key);
    CompleteStop(key, Failure(BrowserProfileError::kDrainTimeout));
    return;
  }
  auto it = runtimes_.find(key);
  if (it == runtimes_.end() || !it->second.profile) {
    OnDrainFlushed(key, epoch);
    return;
  }
  // Wait for every loaded partition's cookie flush. The delegate's previous
  // fire-and-forget flush is NOT completion evidence. Avoid creating a fresh
  // default partition just to stop the Profile.
  Profile* profile = it->second.profile;
  std::vector<content::StoragePartition*> partitions;
  profile->ForEachLoadedStoragePartition(
      [&partitions](content::StoragePartition* partition) {
        partitions.push_back(partition);
      });
  auto all_flushed = base::BarrierClosure(
      partitions.size(), base::BindOnce(&BrowserProfileRouter::FlushDrainPrefs,
                                       weak_factory_.GetWeakPtr(), key, epoch));
  for (auto* partition : partitions) {
    partition->Flush();
    partition->GetCookieManagerForBrowserProcess()->FlushCookieStore(all_flushed);
  }
}

void BrowserProfileRouter::FlushDrainPrefs(std::string key, int64_t epoch) {
  auto it = runtimes_.find(key);
  if (it == runtimes_.end() || !it->second.profile) {
    OnDrainFlushed(key, epoch);
    return;
  }
  Profile* profile = it->second.profile;
  base::OnceClosure flushed = base::BindOnce(
      &BrowserProfileRouter::OnDrainFlushed, weak_factory_.GetWeakPtr(), key, epoch);
  if (drain_flush_for_testing_) {
    flushed = base::BindOnce(drain_flush_for_testing_, std::move(flushed));
  }
  profile->GetPrefs()->CommitPendingWrite(std::move(flushed));
}

void BrowserProfileRouter::OnDrainFlushed(std::string key, int64_t epoch) {
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kDraining ||
      entry->lifecycle_epoch != epoch) {
    return;
  }
  // The flush was asynchronous: a delayed worker/sidecar may still exist.
  if (HasLiveHandles(*entry)) {
    drains_in_flight_.erase(key);
    CompleteStop(key, Failure(BrowserProfileError::kDrainTimeout));
    return;
  }
  // Keep ownership, deadline and in-flight marker through stopped persistence.
  registry_->Transition(
      key, BrowserProfileState::kStopped, BrowserProfileError::kNone,
      base::BindOnce(&BrowserProfileRouter::OnStoppedPersisted,
                     weak_factory_.GetWeakPtr(), key));
}

void BrowserProfileRouter::OnStoppedPersisted(
    std::string key,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    registry_->ForceStateInMemory(key, BrowserProfileState::kDraining,
                                  BrowserProfileError::kStorageError);
    drains_in_flight_.erase(key);
    CompleteStop(key, base::unexpected(result.error()));
    return;
  }
  drains_in_flight_.erase(key);
  ReleaseRuntime(key);
  registry_->SetLoaded(key, false);
  SanitizeLastUsedProfile();
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry) {
    CompleteStop(key, Failure(BrowserProfileError::kProfileNotFound));
    return;
  }
  CompleteStop(key, MakeRecord(*entry));
}

void BrowserProfileRouter::OnDrainTimeout(std::string key) {
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || (entry->state != BrowserProfileState::kDraining &&
                 !drains_in_flight_.contains(key))) {
    return;
  }
  // The Profile stays `draining` (never reported stopped); a late completion
  // of the delegate still finishes the stop.
  CompleteStop(key, Failure(BrowserProfileError::kDrainTimeout));
}

void BrowserProfileRouter::CompleteStop(
    const std::string& key,
    base::expected<ProfileRecord, BrowserProfileFailure> result) {
  drain_timers_.erase(key);
  std::vector<ProfileRecordCallback> waiters;
  if (auto it = stop_waiters_.find(key); it != stop_waiters_.end()) {
    waiters.swap(it->second);
    stop_waiters_.erase(it);
  }
  for (ProfileRecordCallback& callback : waiters) {
    std::move(callback).Run(result);
  }
}

// --- replace persona (contract section 7) -------------------------------------------------------------------

void BrowserProfileRouter::ReplacePersona(const std::string& key,
                                          std::string persona_json,
                                          ProfileRecordCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (indeterminate_handoffs_.contains(key)) {
    BrowserProfileFailure failure(BrowserProfileError::kProfileFailed);
    failure.message = "Network handoff indeterminate; admission remains closed; snapshot replay is prohibited.";
    RunLater(std::move(callback), base::unexpected(failure));
    return;
  }
  if (!IsValidBrowserProfileKey(key)) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kInvalidProfileKey));
    return;
  }
  if (!registry_) {
    RunLater(std::move(callback), base::unexpected(UnavailableFailure()));
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kProfileNotFound));
    return;
  }
  // Only `stopped` is accepted; nothing else is touched or loaded.
  if (BrowserProfileError state_error =
          PersonaReplacementStateError(entry->state);
      state_error != BrowserProfileError::kNone) {
    RunLater(std::move(callback), Failure(state_error));
    return;
  }
  // Operation admission: one replacement per Profile at a time, and no
  // load/delete in the middle of it.
  if (persona_operations_.contains(key) || drains_in_flight_.contains(key)) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
    return;
  }

  std::optional<base::DictValue> input =
      base::JSONReader::ReadDict(persona_json, base::JSON_PARSE_RFC);
  if (!input) {
    RunLater(std::move(callback),
             base::unexpected(BrowserProfileFailure(
                 BrowserProfileError::kPersonaInvalid, "Persona is invalid.")));
    return;
  }
  // Validated now with a throwaway seed; the committed seed is drawn when the
  // intent is applied and never leaves the Profile prefs.
  FingerprintPersona probe;
  std::string persona_error = ParseFingerprintPersonaInput(
      base::Value(input->Clone()), /*current=*/nullptr, /*seed=*/1, &probe);
  if (persona_error.empty()) {
    persona_error = CheckFingerprintPersona(probe);
  }
  if (!persona_error.empty()) {
    RunLater(std::move(callback),
             base::unexpected(BrowserProfileFailure(
                 BrowserProfileError::kPersonaInvalid,
                 std::move(persona_error))));
    return;
  }
  // A Profile that is still in memory is committed live, which needs its
  // persona service; check before anything is persisted.
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded =
      manager ? manager->GetProfileByPath(PathForDirectory(entry->directory))
              : nullptr;
  if (loaded && !ProfilePersonaServiceFactory::GetForProfileIfExists(loaded)) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  std::string canonical = base::WriteJson(*input).value_or(std::string());
  std::string summary_json =
      base::WriteJson(FingerprintPersonaToMcpValue(probe)).value_or("");
  // Admitted until the replacement is committed (or has failed): the wrapped
  // callback releases it on every path.
  persona_operations_.insert(key);
  ProfileRecordCallback finish =
      base::BindOnce(&BrowserProfileRouter::FinishPersonaOperation,
                     weak_factory_.GetWeakPtr(), key, std::move(callback));
  registry_->ReplacePersonaIntent(
      key, std::move(canonical), std::move(summary_json),
      base::BindOnce(&BrowserProfileRouter::OnPersonaIntentPersisted,
                     weak_factory_.GetWeakPtr(), key, std::move(finish)));
}

void BrowserProfileRouter::FinishPersonaOperation(
    std::string key,
    ProfileRecordCallback callback,
    base::expected<ProfileRecord, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    if (indeterminate_handoffs_.contains(key))
      result.error().message = "Network handoff indeterminate; admission remains closed; snapshot replay is prohibited.";
    for (const auto& [profile, operation] : handoffs_) {
      if (operation->key != key) continue;
      if (operation->phase == ProfileStorageHandoff::Phase::kReleasePossible)
        result.error().message = "Network handoff indeterminate; admission remains closed; snapshot replay is prohibited.";
      FinishHandoffFailure(key, base::BindOnce(
          &BrowserProfileRouter::FinishPersonaOperation,
          weak_factory_.GetWeakPtr(), key, std::move(callback), std::move(result)));
      return;
    }
  }
  persona_operations_.erase(key);
  std::move(callback).Run(std::move(result));
}

void BrowserProfileRouter::OnPersonaIntentPersisted(
    std::string key,
    ProfileRecordCallback callback,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    // Rolled back to the previous durable state (intent and epoch alike).
    std::move(callback).Run(base::unexpected(result.error()));
    return;
  }
  // The generation this replacement created; every continuation re-checks it.
  const int64_t epoch = result->lifecycle_epoch;
  const std::string intent = result->persona_input_json;
  if (const BrowserProfileEntry* now = registry_->Find(key);
      !now || now->state != BrowserProfileState::kStopped ||
      now->lifecycle_epoch != epoch || now->persona_input_json != intent) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  // The epoch moved: no handle of the old one may resolve (a stopped Profile
  // has none left after its drain; this is the explicit revocation).
  RevokeOwners(key);
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded =
      manager ? manager->GetProfileByPath(PathForDirectory(result->directory))
              : nullptr;
  if (!loaded) {
    // Unloaded: the bootstrap commits the intent at the next load.
    std::move(callback).Run(MakeRecord(*result));
    return;
  }
  RetireProfileContexts(loaded, key, epoch, intent, base::BindOnce(
      &BrowserProfileRouter::ApplyPersonaAfterRetirement,
      weak_factory_.GetWeakPtr(), key, epoch, intent, loaded->GetWeakPtr(),
      std::move(callback)));
}

void BrowserProfileRouter::ApplyPersonaAfterRetirement(
    std::string key, int64_t epoch, std::string intent,
    base::WeakPtr<Profile> loaded, ProfileRecordCallback callback, bool success) {
  const auto* entry = registry_->Find(key);
  if (!success || !loaded || !entry ||
      entry->state != BrowserProfileState::kStopped ||
      entry->lifecycle_epoch != epoch || entry->persona_input_json != intent) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  // Still in memory: the next load runs no bootstrap, so commit now through the
  // persona service (a new seed; open tabs: none, the Profile is stopped).
  ProfilePersonaService* service =
      ProfilePersonaServiceFactory::GetForProfileIfExists(loaded.get());
  std::optional<base::DictValue> input = base::JSONReader::ReadDict(
      intent, base::JSON_PARSE_RFC);
  FingerprintPersona persona;
  const bool ok =
      service && input &&
      ParseFingerprintPersonaInput(base::Value(std::move(*input)),
                                   /*current=*/nullptr, base::RandUint64(),
                                   &persona)
          .empty() &&
      CheckFingerprintPersona(persona).empty() && service->Set(persona);
  if (!ok) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  loaded->GetPrefs()->CommitPendingWrite(base::BindOnce(
      &BrowserProfileRouter::OnPersonaCommitted, weak_factory_.GetWeakPtr(),
      key, epoch, intent, std::move(callback)));
}

void BrowserProfileRouter::OnPersonaCommitted(std::string key,
                                              int64_t epoch,
                                              std::string intent,
                                              ProfileRecordCallback callback) {
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kStopped ||
      entry->lifecycle_epoch != epoch || entry->persona_input_json != intent) {
    // Another generation owns the Profile now; this one commits nothing.
    std::move(callback).Run(Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded =
      entry && manager
          ? manager->GetProfileByPath(PathForDirectory(entry->directory))
          : nullptr;
  std::string summary_json;
  if (loaded) {
    if (!ManagedProfilePersonaMatchesIntent(loaded, intent)) {
      std::move(callback).Run(Failure(BrowserProfileError::kPersonaInvalid));
      return;
    }
    ManagedProfileIntent identity;
    identity.core_profile_key = key;
    identity.workspace_agent = entry->workspace_agent;
    identity.persona_replacement_pending = true;
    identity.persona_input_json = intent;
    const auto error = VerifyManagedProfileIdentity(loaded, identity);
    if (error != BrowserProfileError::kNone) {
      std::move(callback).Run(Failure(error));
      return;
    }
    if (std::optional<base::DictValue> summary =
            ManagedProfilePersonaSummary(loaded)) {
      summary_json = base::WriteJson(*summary).value_or("");
    }
  }
  if (summary_json.empty()) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  StageProfileHandoff(loaded, base::BindOnce(
      &BrowserProfileRouter::OnPersonaHandoffStaged,
      weak_factory_.GetWeakPtr(), key, epoch, intent, std::move(summary_json),
      std::move(callback)));
}

void BrowserProfileRouter::OnPersonaHandoffStaged(
    std::string key, int64_t epoch, std::string intent,
    std::string summary_json, ProfileRecordCallback callback, bool success) {
  const auto* entry = registry_->Find(key);
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent ||
      entry->state != BrowserProfileState::kStopped) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  if (success && handoff_before_commit_for_testing_) {
    std::move(handoff_before_commit_for_testing_).Run(base::BindOnce(
        &BrowserProfileRouter::OnPersonaHandoffStaged,
        weak_factory_.GetWeakPtr(), key, epoch, intent, std::move(summary_json),
        std::move(callback), success));
    return;
  }
  if (!success) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* profile = manager ? manager->GetProfileByPath(
      PathForDirectory(entry->directory)) : nullptr;
  auto owner = handoffs_.find(profile);
  if (!profile || owner == handoffs_.end() ||
      !OwnsHandoff(profile, owner->second)) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  // The committed persona clears the pending flag (the summary is the one the
  // Profile really holds), only for the generation that was applied.
  registry_->MarkPersonaCommitted(
      key, epoch, intent, std::move(summary_json),
      base::BindOnce(&BrowserProfileRouter::OnPersonaApplied,
                     weak_factory_.GetWeakPtr(), key, epoch, intent,
                     std::move(callback)));
}

void BrowserProfileRouter::OnPersonaApplied(
    std::string key,
    int64_t epoch,
    std::string intent,
    ProfileRecordCallback callback,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    // The persona is committed in the Profile prefs either way; the flag stays
    // and the next load re-applies the same intent (a new seed, same persona).
    std::move(callback).Run(base::unexpected(result.error()));
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kStopped ||
      entry->lifecycle_epoch != epoch || entry->persona_input_json != intent) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded = manager ? manager->GetProfileByPath(
                                 PathForDirectory(entry->directory)) : nullptr;
  if (!loaded) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
    return;
  }
  StageProfileHandoff(loaded, base::BindOnce(
      &BrowserProfileRouter::OnPersonaActivationFenced,
      weak_factory_.GetWeakPtr(), key, epoch, intent, std::move(callback)));
}

void BrowserProfileRouter::OnPersonaActivationFenced(
    std::string key, int64_t epoch, std::string intent,
    ProfileRecordCallback callback, bool success) {
  const auto* entry = registry_->Find(key);
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded = entry && manager ? manager->GetProfileByPath(
      PathForDirectory(entry->directory)) : nullptr;
  if (!entry || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent || !loaded) {
    std::move(callback).Run(Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  if (!success || !ProfileHandoffRestored(loaded)) {
    CompensateProfileHandoff(registry_.get(), loaded,
        key, epoch, intent, base::BindOnce(
        [](ProfileRecordCallback callback,
           base::expected<BrowserProfileEntry, BrowserProfileFailure>) {
          std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
        }, std::move(callback)));
    return;
  }
  StageProfileHandoff(loaded, base::BindOnce(
      &BrowserProfileRouter::OnPersonaActivated, weak_factory_.GetWeakPtr(),
       key, epoch, intent, std::move(callback)), true,
       handoff_activate_for_testing_);
}

void BrowserProfileRouter::OnPersonaActivated(
    std::string key, int64_t epoch, std::string intent,
    ProfileRecordCallback callback, bool success) {
  const auto* entry = registry_->Find(key);
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded = entry && manager ? manager->GetProfileByPath(
      PathForDirectory(entry->directory)) : nullptr;
  if (!success || !entry || !loaded || entry->lifecycle_epoch != epoch ||
      entry->persona_input_json != intent || !ProfileHandoffRestored(loaded)) {
    if (!loaded) {
      std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
      return;
    }
    CompensateProfileHandoff(registry_.get(), loaded,
        key, epoch, intent, base::BindOnce(
        [](ProfileRecordCallback callback,
           base::expected<BrowserProfileEntry, BrowserProfileFailure>) {
          std::move(callback).Run(Failure(BrowserProfileError::kProfileFailed));
        }, std::move(callback)));
    return;
  }
  FinalizeProfileHandoff(loaded, true);
  if (!ManagedNetworkAdmissionReady(loaded)) {
    FinishManagedNetworkAdmission(loaded, true);
  }
  std::move(callback).Run(MakeRecord(*entry));
}

// --- delete ----------------------------------------------------------------------------------------------

void BrowserProfileRouter::DeleteProfile(const std::string& key,
                                         ProfileRecordCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsValidBrowserProfileKey(key)) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kInvalidProfileKey));
    return;
  }
  if (!registry_) {
    RunLater(std::move(callback), base::unexpected(UnavailableFailure()));
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry) {
    RunLater(std::move(callback),
             Failure(BrowserProfileError::kProfileNotFound));
    return;
  }
  // Never the startup Profile (it is not in the registry; defense in depth).
  if (entry->directory == registry_->startup_profile_dir() ||
      (startup_profile_ &&
       startup_profile_->GetPath() == PathForDirectory(entry->directory))) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kForbidden));
    return;
  }
  if (entry->state == BrowserProfileState::kDeleted) {
    // Replay of a delete: the tombstone answers while removal is pending.
    if (entry->deletion_pending) {
      RunLater(std::move(callback), MakeRecord(*entry));
    } else {
      RunLater(std::move(callback),
               Failure(BrowserProfileError::kProfileNotFound));
    }
    return;
  }
  if (entry->state != BrowserProfileState::kStopped ||
      persona_operations_.contains(key) || drains_in_flight_.contains(key)) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  // No descendant handle may survive a deletion.
  if (!session_owners_.SessionsForProfile(key).empty()) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* loaded =
      manager ? manager->GetProfileByPath(PathForDirectory(entry->directory))
              : nullptr;
  if (loaded && (!drain_delegate_ || drain_delegate_->HasDescendantHandles(loaded))) {
    RunLater(std::move(callback), Failure(BrowserProfileError::kProfileBusy));
    return;
  }
  // The tombstone and the incremented epoch are durable BEFORE Chromium is
  // asked to remove anything.
  registry_->Transition(
      key, BrowserProfileState::kDeleted, BrowserProfileError::kNone,
      base::BindOnce(&BrowserProfileRouter::OnTombstonePersisted,
                     weak_factory_.GetWeakPtr(), key, std::move(callback)));
}

void BrowserProfileRouter::OnTombstonePersisted(
    std::string key,
    ProfileRecordCallback callback,
    base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  if (!result.has_value()) {
    std::move(callback).Run(base::unexpected(result.error()));
    return;
  }
  ReleaseRuntime(key);
  RevokeOwners(key);
  ProfileManager* manager = g_browser_process->profile_manager();
  if (manager) {
    // Chromium's removal lifecycle: closes browsers, wipes browsing data,
    // marks the directory in kProfilesDeleted (removed at the next start when
    // the Profile was loaded), orphans the attributes entry. Never rm while
    // loaded.
    manager->GetDeleteProfileHelper().MaybeScheduleProfileForDeletion(
        PathForDirectory(result->directory), base::DoNothing(),
        ProfileMetrics::DELETE_PROFILE_SETTINGS);
  }
  SanitizeLastUsedProfile();
  std::move(callback).Run(MakeRecord(*result));
}

void BrowserProfileRouter::ReconcileTombstones(bool reschedule) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!registry_) {
    return;
  }
  for (const BrowserProfileEntry& entry : registry_->List()) {
    if (entry.state != BrowserProfileState::kDeleted || !entry.deletion_pending) {
      continue;
    }
    base::FilePath path = PathForDirectory(entry.directory);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce([](base::FilePath p) { return base::PathExists(p); },
                       path),
        base::BindOnce(&BrowserProfileRouter::OnTombstoneChecked,
                       weak_factory_.GetWeakPtr(), entry.key, path,
                       reschedule));
  }
}

void BrowserProfileRouter::OnTombstoneChecked(std::string key,
                                              base::FilePath path,
                                              bool reschedule,
                                              bool exists) {
  if (!registry_) {
    return;
  }
  const BrowserProfileEntry* entry = registry_->Find(key);
  if (!entry || entry->state != BrowserProfileState::kDeleted ||
      !entry->deletion_pending) {
    return;
  }
  if (!exists) {
    registry_->ConfirmDeletion(key, base::BindOnce(&IgnoreEntry));
    return;
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  if (reschedule && manager && !manager->GetProfileByPath(path)) {
    // The load that the deletion helper does is refused by the bootstrap
    // (state `deleted`), which makes the helper remove the directory.
    manager->GetDeleteProfileHelper().MaybeScheduleProfileForDeletion(
        path, base::DoNothing(), ProfileMetrics::DELETE_PROFILE_SETTINGS);
  }
}

// --- runtime handles ---------------------------------------------------------------------------------------

void BrowserProfileRouter::ReleaseRuntime(const std::string& key) {
  auto it = runtimes_.find(key);
  if (it == runtimes_.end()) {
    return;
  }
  Runtime runtime = std::move(it->second);
  runtimes_.erase(it);
  // Destroys the keep-alive; the Profile stays observed (it may stay loaded).
  runtime.keep_alive.reset();
}

void BrowserProfileRouter::RevokeOwners(const std::string& key) {
  session_owners_.RemoveAllForProfile(key);
  target_owners_.RemoveAllForProfile(key);
}

void BrowserProfileRouter::OnProfileWillBeDestroyed(Profile* profile) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto handoff = handoffs_.find(profile);
  if (handoff != handoffs_.end()) {
    // Forced browser shutdown can override keepalives. No continuation may
    // touch soon-destroyed participants, reopen admission, or replay snapshots.
    auto operation = handoff->second;
    operation->teardown_started = true;
    operation->teardown_complete = true;
    operation->transaction.reset();
    operation->teardown_waiters.clear();
    operation->failure_waiters.clear();
    (void)operation->keep_alive.release();  // same shutdown policy as runtime
    handoffs_.erase(handoff);
  }
  profile_observations_.RemoveObservation(profile);
  if (profile == startup_profile_) {
    startup_profile_ = nullptr;
  }
  for (auto it = runtimes_.begin(); it != runtimes_.end();) {
    if (it->second.profile != profile) {
      ++it;
      continue;
    }
    const std::string key = it->first;
    // The Profile is already being destroyed (browser shutdown): releasing
    // the keep-alive now would touch it. Leak the small handle instead.
    (void)it->second.keep_alive.release();
    it = runtimes_.erase(it);
    RevokeOwners(key);
    if (registry_) {
      // Not persisted here (shutdown): a restart recovers `stopped`.
      registry_->ForceStateInMemory(key, BrowserProfileState::kFailed,
                                    BrowserProfileError::kProfileFailed);
    }
  }
}

// --- last-used Profile ------------------------------------------------------------------------------------------

void BrowserProfileRouter::SanitizeLastUsedProfile() {
  // Browser windows of a managed Profile make it the last used one, which is
  // what the NEXT start opens as its startup Profile. The startup/legacy
  // Profile is fixed, so managed directories are kept out of kProfileLastUsed
  // and kProfilesLastActive (best effort: a crash can leave one behind, which
  // the next Initialize() repairs before Chromium picks the startup Profile).
  if (!registry_ || !g_browser_process || !g_browser_process->local_state()) {
    return;
  }
  const std::vector<BrowserProfileEntry> entries = registry_->List();
  const std::string last_used =
      ProfileManager::GetLastUsedProfileBaseName().AsUTF8Unsafe();
  for (const BrowserProfileEntry& entry : entries) {
    if (entry.directory == last_used) {
      std::string fallback = registry_->startup_profile_dir();
      if (fallback.empty()) {
        fallback = chrome::kInitialProfile;
      }
      profiles::SetLastUsedProfile(base::FilePath::FromUTF8Unsafe(fallback));
      break;
    }
  }
  PrefService* local_state = g_browser_process->local_state();
  bool listed = false;
  for (const base::Value& value : local_state->GetList(::prefs::kProfilesLastActive)) {
    const std::string* name = value.GetIfString();
    if (name && registry_->FindByDirectory(*name)) {
      listed = true;
      break;
    }
  }
  if (!listed) {
    return;  // Do not rewrite Local State for nothing.
  }
  ScopedListPrefUpdate update(local_state, ::prefs::kProfilesLastActive);
  for (const BrowserProfileEntry& entry : entries) {
    update->EraseValue(base::Value(entry.directory));
  }
}

// --- ManagedProfileProvider --------------------------------------------------------------------------------------

std::optional<ManagedProfileIntent> BrowserProfileRouter::LookupByDirectory(
    std::string_view directory) {
  if (!registry_) {
    return std::nullopt;
  }
  const BrowserProfileEntry* entry = registry_->FindByDirectory(directory);
  if (!entry) {
    return std::nullopt;
  }
  ManagedProfileIntent intent;
  intent.core_profile_key = entry->key;
  intent.workspace_agent = entry->workspace_agent;
  intent.lifecycle_epoch = entry->lifecycle_epoch;
  intent.state = entry->state;
  intent.provisioned = entry->provisioned;
  intent.persona_replacement_pending = entry->persona_replacement_pending;
  intent.persona_input_json = entry->persona_input_json;
  intent.proxy_pref_json = entry->proxy_pref_json;
  return intent;
}

void BrowserProfileRouter::OnBootstrapFinished(std::string_view directory,
                                               BrowserProfileError error) {
  if (error == BrowserProfileError::kNone) {
    bootstrap_errors_.erase(std::string(directory));
  } else {
    bootstrap_errors_[std::string(directory)] = error;
  }
}

}  // namespace shuttle

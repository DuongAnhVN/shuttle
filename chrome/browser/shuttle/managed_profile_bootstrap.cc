// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/managed_profile_bootstrap.h"
#include "chrome/browser/shuttle/managed_network_admission.h"

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/values_util.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_proxy.h"
#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "chrome/browser/shuttle/profile_persona_service_factory.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "crypto/random.h"

namespace shuttle {

namespace {

ManagedProfileProvider* g_provider = nullptr;

// Same key as the `created` field ProfileRegistry writes in a kProfiles entry.
constexpr char kAgentCreatedKey[] = "created";

// The user value of the marker pref, or null.
const base::Value* MarkerValue(PrefService* prefs) {
  if (!prefs || !prefs->FindPreference(prefs::kManagedProfile)) {
    return nullptr;
  }
  return prefs->GetUserPrefValue(prefs::kManagedProfile);
}

// The core profile key stored in the marker, "" if absent or unreadable.
std::string MarkerKey(PrefService* prefs) {
  const base::Value* value = MarkerValue(prefs);
  const base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  if (!dict || dict->FindInt(prefs::kManagedProfileVersionKey) !=
                   prefs::kManagedProfileVersion) {
    return std::string();
  }
  const std::string* key = dict->FindString(prefs::kManagedProfileKeyKey);
  return key ? *key : std::string();
}

StoredFingerprintPersona StoredPersona(PrefService* prefs) {
  if (!prefs || !prefs->FindPreference(prefs::kFingerprintPersona)) {
    // Unregistered pref: nothing can be read, so nothing can be applied.
    StoredFingerprintPersona corrupt;
    corrupt.state = StoredFingerprintPersona::State::kCorrupt;
    return corrupt;
  }
  return FingerprintPersonaFromPrefValue(
      prefs->GetUserPrefValue(prefs::kFingerprintPersona));
}

const base::DictValue* FindWorkspaceEntry(PrefService* prefs,
                                          const std::string& agent) {
  if (!prefs || !prefs->FindPreference(prefs::kProfiles)) {
    return nullptr;
  }
  const base::Value* value = prefs->GetDict(prefs::kProfiles).Find(agent);
  return value ? value->GetIfDict() : nullptr;
}

uint64_t RandomSeed() {
  std::array<uint8_t, 8> raw = crypto::RandBytesAsArray<8>();
  uint64_t seed = 0;
  for (uint8_t byte : raw) {
    seed = (seed << 8) | byte;
  }
  return seed;
}

// One bootstrap run. Reference counted because the encryptor and the pref
// commit complete asynchronously; it never touches the Profile after it is
// destroyed.
class Bootstrap : public base::RefCounted<Bootstrap> {
 public:
  Bootstrap(Profile* profile,
            std::string directory,
            ManagedProfileIntent intent,
            bool marker_present,
            bool allow_async,
            ManagedBootstrapCallback completion)
      : profile_(profile->GetWeakPtr()),
        directory_(std::move(directory)),
        intent_(std::move(intent)),
        marker_present_(marker_present),
        allow_async_(allow_async),
        completion_(std::move(completion)) {}

  void Start() {
    if (!IsolatedProfilesBootstrapSafe()) {
      Finish(BrowserProfileError::kUnsupported);
      return;
    }
    PrefService* prefs = prefs_or_null();
    if (!prefs) {
      Finish(BrowserProfileError::kProfileFailed);
      return;
    }
    // Extension preference services may exist. Identity services must not:
    // content defers NetworkContext configuration until admission opens.
    if (ProfilePersonaServiceFactory::GetForProfileIfExists(profile_.get())) {
      Finish(BrowserProfileError::kProfileFailed);
      return;
    }
    // Only a load the router started may bring a managed Profile up: this
    // refuses session restore, --profile-directory, the deletion helper's
    // "polite" load and any Chromium-initiated load of a stopped/deleted
    // Profile.
    if (intent_.state != BrowserProfileState::kLoading) {
      Finish(BrowserProfileError::kProfileFailed);
      return;
    }
    if (marker_present_) {
      // A marker that does not name this registry entry (or cannot be read)
      // is never repaired silently.
      if (MarkerKey(prefs) != intent_.core_profile_key) {
        Finish(BrowserProfileError::kProfileFailed);
        return;
      }
    } else if (intent_.provisioned) {
      // The registry says the persona was committed, but the Profile has no
      // marker (prefs lost or reset): regenerating would silently change the
      // identity. Fail closed.
      Finish(BrowserProfileError::kPersonaInvalid);
      return;
    }

    if (!ProxyConfigured(prefs)) {
      Proceed(nullptr);
      return;
    }
    os_crypt_async::OSCryptAsync* os_crypt = g_browser_process->os_crypt_async();
    if (!os_crypt) {
      Finish(BrowserProfileError::kProxyNotReady);
      return;
    }
    // GetInstance may run the callback before it returns (encryptor already
    // available); otherwise it needs the message loop.
    os_crypt->GetInstance(
        base::BindOnce(&Bootstrap::OnEncryptor, scoped_refptr<Bootstrap>(this)));
    if (!finished_ && !encryptor_seen_ && !allow_async_) {
      Finish(BrowserProfileError::kProxyNotReady);
    }
  }

 private:
  friend class base::RefCounted<Bootstrap>;
  ~Bootstrap() = default;

  PrefService* prefs_or_null() const {
    return profile_ ? profile_->GetPrefs() : nullptr;
  }

  bool ProxyConfigured(PrefService* prefs) const {
    if (!intent_.proxy_pref_json.empty()) {
      return true;
    }
    const base::DictValue* entry =
        FindWorkspaceEntry(prefs, intent_.workspace_agent);
    return entry && entry->contains(prefs::kProxy);
  }

  void OnEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor) {
    encryptor_seen_ = true;
    if (finished_ || !profile_) {
      return;
    }
    if (!encryptor || !encryptor->IsEncryptionAvailable()) {
      Finish(BrowserProfileError::kProxyNotReady);
      return;
    }
    encryptor_ = std::move(encryptor);
    Proceed(encryptor_.get());
  }

  void Proceed(const os_crypt_async::Encryptor* encryptor) {
    PrefService* prefs = prefs_or_null();
    if (!prefs) {
      Finish(BrowserProfileError::kProfileFailed);
      return;
    }
    // A persona replacement persisted while the Profile was stopped
    // (PROFILES.md section 7) is committed here, before any workload.
    const bool replacing = marker_present_ && intent_.persona_replacement_pending;
    BrowserProfileError error =
        !marker_present_ ? Provision(prefs, encryptor)
        : replacing      ? ApplyReplacement(prefs, encryptor)
                         : CheckCommitted(prefs, encryptor,
                                          /*check_persona=*/true);
    if (error != BrowserProfileError::kNone) {
      Finish(error);
      return;
    }
    if (marker_present_ && !replacing) {
      Finish(BrowserProfileError::kNone);
      return;
    }
    // First load or replacement: the committed persona must be durable before
    // any workload, or a crash would regenerate a different one.
    if (!allow_async_) {
      Finish(BrowserProfileError::kStorageError);
      return;
    }
    prefs->CommitPendingWrite(
        base::BindOnce(&Bootstrap::OnCommitted, scoped_refptr<Bootstrap>(this)));
  }

  void OnCommitted() {
    if (finished_ || !profile_) {
      return;
    }
    Finish(BrowserProfileError::kNone);
  }

  // Later loads: the committed identity must be intact.
  BrowserProfileError CheckCommitted(PrefService* prefs,
                                     const os_crypt_async::Encryptor* encryptor,
                                     bool check_persona) {
    if (check_persona &&
        StoredPersona(prefs).state != StoredFingerprintPersona::State::kPersona) {
      return BrowserProfileError::kPersonaInvalid;
    }
    const base::DictValue* entry =
        FindWorkspaceEntry(prefs, intent_.workspace_agent);
    if (!entry) {
      return BrowserProfileError::kProfileFailed;
    }
    if (entry->contains(prefs::kProxy) &&
        ProxyFromPrefValue(entry->Find(prefs::kProxy), encryptor).state !=
            StoredProxy::State::kProxy) {
      return BrowserProfileError::kProxyNotReady;
    }
    return BrowserProfileError::kNone;
  }

  // Replacement while stopped: the identity components other than the persona
  // (workspace entry, proxy) must be intact; the new intent is validated, then
  // written with a new seed. The previous persona may be unreadable (that is
  // one reason to replace it). Nothing is written when validation fails;
  // cookies and storage are never touched.
  BrowserProfileError ApplyReplacement(
      PrefService* prefs,
      const os_crypt_async::Encryptor* encryptor) {
    BrowserProfileError error =
        CheckCommitted(prefs, encryptor, /*check_persona=*/false);
    if (error != BrowserProfileError::kNone) {
      return error;
    }
    std::optional<base::DictValue> input = base::JSONReader::ReadDict(
        intent_.persona_input_json, base::JSON_PARSE_RFC);
    if (!input) {
      return BrowserProfileError::kPersonaInvalid;
    }
    FingerprintPersona persona;
    std::string parse_error = ParseFingerprintPersonaInput(
        base::Value(std::move(*input)), /*current=*/nullptr, RandomSeed(),
        &persona);
    if (!parse_error.empty() || !CheckFingerprintPersona(persona).empty()) {
      return BrowserProfileError::kPersonaInvalid;
    }
    if (!prefs->FindPreference(prefs::kFingerprintPersona)) {
      return BrowserProfileError::kProfileFailed;
    }
    prefs->SetDict(prefs::kFingerprintPersona,
                   FingerprintPersonaToPrefValue(persona));
    return BrowserProfileError::kNone;
  }

  // First load: validate everything, then write everything. Nothing is
  // written when validation fails.
  BrowserProfileError Provision(PrefService* prefs,
                                const os_crypt_async::Encryptor* encryptor) {
    StoredFingerprintPersona stored = StoredPersona(prefs);
    if (stored.state == StoredFingerprintPersona::State::kCorrupt) {
      return BrowserProfileError::kPersonaInvalid;
    }
    if (stored.state == StoredFingerprintPersona::State::kPersona &&
        !ManagedProfilePersonaMatchesIntent(profile_.get(),
                                             intent_.persona_input_json)) {
      return BrowserProfileError::kPersonaInvalid;
    }
    std::optional<base::DictValue> persona_value;
    if (stored.state == StoredFingerprintPersona::State::kNone) {
      std::optional<base::DictValue> input = base::JSONReader::ReadDict(
          intent_.persona_input_json, base::JSON_PARSE_RFC);
      if (!input) {
        return BrowserProfileError::kPersonaInvalid;
      }
      FingerprintPersona persona;
      std::string error = ParseFingerprintPersonaInput(
          base::Value(std::move(*input)), /*current=*/nullptr, RandomSeed(),
          &persona);
      if (!error.empty() || !CheckFingerprintPersona(persona).empty()) {
        return BrowserProfileError::kPersonaInvalid;
      }
      persona_value = FingerprintPersonaToPrefValue(persona);
    }

    std::optional<base::DictValue> proxy_value;
    if (!intent_.proxy_pref_json.empty()) {
      proxy_value = base::JSONReader::ReadDict(intent_.proxy_pref_json,
                                               base::JSON_PARSE_RFC);
      if (!proxy_value) {
        return BrowserProfileError::kProxyNotReady;
      }
      // It must read back (password decryptable) before it is committed.
      base::Value readback(proxy_value->Clone());
      if (ProxyFromPrefValue(&readback, encryptor).state !=
          StoredProxy::State::kProxy) {
        return BrowserProfileError::kProxyNotReady;
      }
    }

    if (!prefs->FindPreference(prefs::kProfiles) ||
        !prefs->FindPreference(prefs::kManagedProfile)) {
      return BrowserProfileError::kProfileFailed;
    }

    if (persona_value) {
      prefs->SetDict(prefs::kFingerprintPersona, std::move(*persona_value));
    }
    {
      ScopedDictPrefUpdate update(prefs, prefs::kProfiles);
      base::DictValue* entry = update->FindDict(intent_.workspace_agent);
      if (!entry) {
        base::DictValue created;
        created.Set(kAgentCreatedKey, base::TimeToValue(base::Time::Now()));
        entry = update->Set(intent_.workspace_agent, std::move(created))
                    ->GetIfDict();
      }
      if (entry && proxy_value) {
        entry->Set(prefs::kProxy, std::move(*proxy_value));
      }
    }
    // The marker goes last: its presence means "fully provisioned".
    prefs->SetDict(prefs::kManagedProfile,
                   base::DictValue()
                       .Set(prefs::kManagedProfileVersionKey,
                            prefs::kManagedProfileVersion)
                       .Set(prefs::kManagedProfileKeyKey,
                            intent_.core_profile_key));
    return BrowserProfileError::kNone;
  }

  void Finish(BrowserProfileError error) {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (g_provider) {
      g_provider->OnBootstrapFinished(directory_, error);
    }
    if (!profile_) {
      return;  // The Profile is gone: nobody to continue.
    }
    FinishManagedNetworkAdmission(profile_.get(),
                                  error == BrowserProfileError::kNone,
                                  encryptor_);
    std::move(completion_).Run(error == BrowserProfileError::kNone);
  }

  base::WeakPtr<Profile> profile_;
  const std::string directory_;
  const ManagedProfileIntent intent_;
  const bool marker_present_;
  const bool allow_async_;
  ManagedBootstrapCallback completion_;
  bool encryptor_seen_ = false;
  bool finished_ = false;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;
};

}  // namespace

ManagedProfileIntent::ManagedProfileIntent() = default;
ManagedProfileIntent::ManagedProfileIntent(const ManagedProfileIntent&) =
    default;
ManagedProfileIntent& ManagedProfileIntent::operator=(
    const ManagedProfileIntent&) = default;
ManagedProfileIntent::~ManagedProfileIntent() = default;

void SetManagedProfileProvider(ManagedProfileProvider* provider) {
  CHECK(!provider || !g_provider || provider == g_provider);
  g_provider = provider;
}

bool IsManagedProfile(Profile* profile) {
  return profile && MarkerValue(profile->GetPrefs()) != nullptr;
}

void PrepareManagedProfile(Profile* profile,
                           bool allow_async,
                           ManagedBootstrapCallback completion) {
  CHECK(profile);
  PrefService* prefs = profile->GetPrefs();
  const bool marker_present = MarkerValue(prefs) != nullptr;
  const std::string directory = profile->GetPath().BaseName().AsUTF8Unsafe();
  std::optional<ManagedProfileIntent> intent;
  if (g_provider) {
    intent = g_provider->LookupByDirectory(directory);
  }

  if (!marker_present && !intent) {
    // Legacy / startup / any unmanaged Profile: nothing to do.
    FinishManagedNetworkAdmission(profile, true);
    std::move(completion).Run(true);
    return;
  }
  if (!intent) {
    // A managed Profile (marker) that the registry does not know or that is
    // loaded without the feature: never loaded as a stock Profile.
    LOG(ERROR) << "Shuttle: a managed profile was loaded without a registry "
                  "entry; refusing to load it.";
    if (g_provider) {
      g_provider->OnBootstrapFinished(directory,
                                      BrowserProfileError::kProfileFailed);
    }
    FinishManagedNetworkAdmission(profile, false);
    std::move(completion).Run(false);
    return;
  }
  base::MakeRefCounted<Bootstrap>(profile, directory, std::move(*intent),
                                  marker_present, allow_async,
                                  std::move(completion))
      ->Start();
}

BrowserProfileError VerifyManagedProfileIdentity(
    Profile* profile,
    const ManagedProfileIntent& intent) {
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!prefs || MarkerKey(prefs) != intent.core_profile_key) {
    return BrowserProfileError::kProfileFailed;
  }
  if (StoredPersona(prefs).state != StoredFingerprintPersona::State::kPersona) {
    return BrowserProfileError::kPersonaInvalid;
  }
  if (intent.persona_replacement_pending &&
      !ManagedProfilePersonaMatchesIntent(profile, intent.persona_input_json)) {
    return BrowserProfileError::kPersonaInvalid;
  }
  const base::DictValue* entry = FindWorkspaceEntry(prefs, intent.workspace_agent);
  if (!entry) {
    return BrowserProfileError::kProfileFailed;
  }
  if (entry->contains(prefs::kProxy)) {
    ProfileProxyService* proxies = ProfileProxyServiceFactory::GetForProfileIfExists(profile);
    if (!proxies ||
        proxies->Lookup(intent.workspace_agent).state !=
            StoredProxy::State::kProxy) {
      return BrowserProfileError::kProxyNotReady;
    }
  }
  return BrowserProfileError::kNone;
}

std::optional<base::DictValue> ManagedProfilePersonaSummary(Profile* profile) {
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  StoredFingerprintPersona stored = StoredPersona(prefs);
  if (stored.state != StoredFingerprintPersona::State::kPersona ||
      !stored.persona) {
    return std::nullopt;
  }
  return FingerprintPersonaToMcpValue(*stored.persona);
}

bool ManagedProfilePersonaMatchesIntent(Profile* profile,
                                        std::string_view persona_input_json) {
  auto stored = StoredPersona(profile ? profile->GetPrefs() : nullptr);
  auto input = base::JSONReader::ReadDict(persona_input_json, base::JSON_PARSE_RFC);
  if (!input || !stored.persona ||
      stored.state != StoredFingerprintPersona::State::kPersona) {
    return false;
  }
  FingerprintPersona expected;
  return ParseFingerprintPersonaInput(base::Value(std::move(*input)), nullptr,
                                      stored.persona->seed, &expected).empty() &&
         CheckFingerprintPersona(expected).empty() &&
         FingerprintPersonaToPrefValue(expected) ==
             FingerprintPersonaToPrefValue(*stored.persona);
}

}  // namespace shuttle

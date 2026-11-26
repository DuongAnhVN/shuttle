// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/profile_persona_service.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_proxy.h"
#include "chrome/browser/shuttle/persona_runtime.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace shuttle {

ProfilePersonaService::ProfilePersonaService(Profile* profile)
    : profile_(profile) {
  DropLegacyAgentPersonas();
  StoreMigratedPersona();
  // The run starts with the service: a managed Profile's bootstrap has
  // committed its persona before services are created.
  StoredFingerprintPersona initial = Lookup();
  run_persona_unreadable_ = initial.state == StoredFingerprintPersona::State::kCorrupt;
  run_persona_ = std::move(initial.persona);
}

ProfilePersonaService::~ProfilePersonaService() = default;

base::CallbackListSubscription ProfilePersonaService::AddChangeCallback(
    ChangeCallbackList::CallbackType callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return change_callbacks_.Add(std::move(callback));
}

StoredFingerprintPersona ProfilePersonaService::Lookup() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service || !pref_service->FindPreference(prefs::kFingerprintPersona)) {
    return StoredFingerprintPersona();
  }
  // The user value only: the registered default is an empty dictionary, which
  // would read as corrupt.
  const base::Value* value =
      pref_service->GetUserPrefValue(prefs::kFingerprintPersona);
  if (!value && IsManaged()) {
    // A managed (isolated) Profile always has a committed persona: its
    // absence is corruption, never "no persona" (no stock fallback).
    StoredFingerprintPersona corrupt;
    corrupt.state = StoredFingerprintPersona::State::kCorrupt;
    return corrupt;
  }
  return FingerprintPersonaFromPrefValue(value);
}

bool ProfilePersonaService::IsManaged() const {
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  return pref_service && pref_service->FindPreference(prefs::kManagedProfile) &&
         pref_service->GetUserPrefValue(prefs::kManagedProfile) != nullptr;
}

std::optional<FingerprintPersona> ProfilePersonaService::Get() const {
  StoredFingerprintPersona stored = Lookup();
  if (stored.state != StoredFingerprintPersona::State::kPersona) {
    return std::nullopt;
  }
  return std::move(stored.persona);
}

const std::optional<FingerprintPersona>& ProfilePersonaService::RunPersona()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return run_persona_;
}

bool ProfilePersonaService::PendingRestart() const {
  StoredFingerprintPersona stored = Lookup();
  switch (stored.state) {
    case StoredFingerprintPersona::State::kPersona:
      return !run_persona_ || !(*run_persona_ == *stored.persona);
    case StoredFingerprintPersona::State::kNone:
      return run_persona_.has_value() || run_persona_unreadable_;
    case StoredFingerprintPersona::State::kCorrupt:
      // Nothing the next run could present either.
      return !run_persona_unreadable_;
  }
  return false;
}

std::optional<std::string> ProfilePersonaService::AcceptLanguageHeader() const {
  if (!run_persona_) {
    return std::nullopt;
  }
  return AcceptLanguageHeaderForPersona(*run_persona_);
}

bool ProfilePersonaService::Set(const FingerprintPersona& persona) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service || !pref_service->FindPreference(prefs::kFingerprintPersona) ||
      !CheckFingerprintPersona(persona).empty()) {
    return false;
  }
  pref_service->SetDict(prefs::kFingerprintPersona,
                        FingerprintPersonaToPrefValue(persona));
  if (IsManaged()) {
    // Only ever written between two runs (the Profile is stopped and its
    // contexts are retired), so the next run starts with it.
    run_persona_ = persona;
    run_persona_unreadable_ = false;
    NotifyChanged();
  }
  // An unmanaged Profile's run keeps its persona: a change while pages,
  // iframes and workers live would split them between two identities. The
  // next browser start presents it (PendingRestart()).
  return true;
}

void ProfilePersonaService::Clear() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service || !pref_service->FindPreference(prefs::kFingerprintPersona)) {
    return;
  }
  if (IsManaged()) {
    // Clearing would turn an isolated Profile into a stock one. A managed
    // persona is only ever replaced (Set), never removed.
    LOG(WARNING) << "Shuttle: refusing to clear the persona of a managed "
                    "profile.";
    return;
  }
  // Like Set on an unmanaged Profile: the run keeps its persona, the next
  // browser start has none.
  pref_service->ClearPref(prefs::kFingerprintPersona);
}

void ProfilePersonaService::NotifyChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  change_callbacks_.Notify();
}

bool ProfilePersonaService::ConfigureNetworkContextParams(
    network::mojom::NetworkContextParams* params) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!params) {
    return false;
  }
  const std::optional<FingerprintPersona>& persona = run_persona_;
  if (!persona) {
    if (IsManaged()) {
      // Defense in depth (the bootstrap does not let such a Profile load):
      // a managed Profile without a readable persona gets a context that
      // cannot reach the network, not the stock identity.
      params->initial_proxy_config = FailClosedNetConfig();
    }
    return false;
  }
  params->accept_language = AcceptLanguageHeaderForPersona(*persona);
  params->user_agent = persona->user_agent;
  return true;
}

void ProfilePersonaService::StoreMigratedPersona() {
  StoredFingerprintPersona stored = Lookup();
  if (stored.state != StoredFingerprintPersona::State::kPersona ||
      !stored.version_migrated) {
    return;
  }
  profile_->GetPrefs()->SetDict(prefs::kFingerprintPersona,
                                FingerprintPersonaToPrefValue(*stored.persona));
}

void ProfilePersonaService::DropLegacyAgentPersonas() {
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service || !pref_service->FindPreference(prefs::kProfiles)) {
    return;
  }
  std::vector<std::string> ids;
  for (auto [id, value] : pref_service->GetDict(prefs::kProfiles)) {
    const base::DictValue* dict = value.GetIfDict();
    if (dict && dict->contains(prefs::kLegacyAgentFingerprintPersona)) {
      ids.push_back(id);
    }
  }
  if (ids.empty()) {
    return;
  }
  ScopedDictPrefUpdate update(pref_service, prefs::kProfiles);
  for (const std::string& id : ids) {
    if (base::DictValue* dict = update->FindDict(id)) {
      dict->Remove(prefs::kLegacyAgentFingerprintPersona);
    }
  }
}

}  // namespace shuttle

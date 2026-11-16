// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_H_
#define CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_H_

#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace network::mojom {
class NetworkContextParams;
}

namespace shuttle {

// Where the runtime reads the fingerprint persona of the Chromium profile
// from, and how it reaches the profile's NetworkContexts. A profile has ONE
// persona: every tab, agent and storage partition of the profile presents it.
// There is no per-agent persona. Lives in the `agent_persona` source_set on
// purpose, like ProfileProxyService: the network stack and the tab prefs call
// it, and they must not depend on ShuttleService / ProfileRegistry.
//
// Storage: the profile pref prefs::kFingerprintPersona (registered by
// ProfilePersonaServiceFactory), not nested in prefs::kProfiles.
//
// Created through ProfilePersonaServiceFactory::GetForProfile(profile), which
// returns null for profiles without it (off-the-record, system).
//
// Threading: UI thread only.
class ProfilePersonaService : public KeyedService {
 public:
  using ChangeCallbackList = base::RepeatingCallbackList<void()>;

  explicit ProfilePersonaService(Profile* profile);
  ProfilePersonaService(const ProfilePersonaService&) = delete;
  ProfilePersonaService& operator=(const ProfilePersonaService&) = delete;
  ~ProfilePersonaService() override;

  // `callback` runs after the profile's persona was set, replaced or cleared
  // (NotifyChanged). Every open tab of the profile re-syncs its renderer prefs
  // there, and the NetworkContexts get their new Accept-Language. The
  // subscription must be destroyed before this service.
  base::CallbackListSubscription AddChangeCallback(
      ChangeCallbackList::CallbackType callback);

  // The stored persona: kNone if there is none, kCorrupt for an unreadable
  // value (never applied in part).
  StoredFingerprintPersona Lookup() const;
  // The persona if there is a readable one (state kPersona).
  std::optional<FingerprintPersona> Get() const;

  // The Accept-Language header value of the persona, or nullopt if there is no
  // readable persona (the profile's value applies).
  std::optional<std::string> AcceptLanguageHeader() const;

  // Stores `persona` (which must pass CheckFingerprintPersona; an incoherent
  // one is refused: returns false, nothing written) and notifies.
  bool Set(const FingerprintPersona& persona);
  // Removes the stored persona (readable or not) and notifies.
  void Clear();

  // Runs the change callbacks. Set and Clear call it; call it yourself only
  // after writing the pref some other way.
  void NotifyChanged();

  // Called by ProfileNetworkContextService::ConfigureNetworkContextParamsInternal
  // for every NetworkContext of the profile (main and storage partitions
  // alike). With a readable persona, sets `params`' accept_language (the
  // persona's languages) and user_agent (the persona's) and returns true.
  // Accept-Language of live contexts follows a later change
  // (ProfileNetworkContextService::UpdateAcceptLanguage). The NetworkContext
  // user agent has no setter and contexts are never recreated here: it is
  // read here only, so a changed or cleared persona's user agent reaches a
  // context only if the context is created after the change (a browser
  // restart, or a partition created later); a context created before keeps
  // the user agent it got. Requests from renderers carry the user agent of the
  // renderer prefs (see persona_runtime.h, persona_tab_prefs.h) instead, from
  // the page's next navigation.
  bool ConfigureNetworkContextParams(
      network::mojom::NetworkContextParams* params) const;

 private:
  // Deletes prefs::kLegacyAgentFingerprintPersona from the kProfiles entries.
  void DropLegacyAgentPersonas();

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Profile> profile_;
  ChangeCallbackList change_callbacks_;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_H_

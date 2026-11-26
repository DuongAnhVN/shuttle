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

  // `callback` runs after the persona of the profile's run changed
  // (NotifyChanged): only between two runs of a managed Profile (Set), never
  // while a run lasts. Every open tab of the profile re-syncs its renderer
  // prefs there, and the NetworkContexts get their new Accept-Language. The
  // subscription must be destroyed before this service.
  base::CallbackListSubscription AddChangeCallback(
      ChangeCallbackList::CallbackType callback);

  // The persona this run of the profile presents, or nullopt (none, or the
  // stored one is unreadable). It is read once, when the run starts, and does
  // not change while the run lasts: every page, iframe, worker and request of
  // the run sees the same one (a renderer process keeps what it was started
  // with). A run is the life of this service for an unmanaged Profile (one
  // browser session), and one load of a managed Profile (whose persona is only
  // replaced while it is stopped, see Set). Everything that applies the
  // persona reads this, not Get().
  const std::optional<FingerprintPersona>& RunPersona() const;
  // Whether the stored persona differs from RunPersona(): a change made while
  // the run lasts, which the next run presents.
  bool PendingRestart() const;
  bool RunPersonaUnreadable() const { return run_persona_unreadable_; }

  // Whether the Profile is a managed (isolated) one: it carries the marker
  // pref written by managed_profile_bootstrap (prefs::kManagedProfile). A
  // managed Profile fails closed: no persona reads as kCorrupt, Clear() is
  // refused and a context without a readable persona cannot reach the
  // network. Unmanaged (legacy/startup) Profiles keep the old semantics.
  bool IsManaged() const;

  // The stored persona: kNone if there is none (never for a managed Profile),
  // kCorrupt for an unreadable value (never applied in part).
  StoredFingerprintPersona Lookup() const;
  // The stored persona if there is a readable one (state kPersona). What the
  // NEXT run presents; the current run presents RunPersona().
  std::optional<FingerprintPersona> Get() const;

  // The Accept-Language header value of RunPersona(), or nullopt if the run
  // has no persona (the profile's value applies).
  std::optional<std::string> AcceptLanguageHeader() const;

  // Stores `persona` (which must pass CheckFingerprintPersona; an incoherent
  // one is refused: returns false, nothing written). A managed Profile is only
  // written between runs (stopped, its contexts retired: managed_profile_
  // bootstrap, BrowserProfileRouter), so its run persona follows and the
  // change callbacks run. An unmanaged Profile keeps presenting its run
  // persona until the next browser start (PendingRestart()).
  bool Set(const FingerprintPersona& persona);
  // Removes the stored persona (readable or not); like Set, it reaches the
  // next run. Refused (no effect) for a managed Profile.
  void Clear();

  // Runs the change callbacks. Set and Clear call it; call it yourself only
  // after writing the pref some other way.
  void NotifyChanged();

  // Called by ProfileNetworkContextService::ConfigureNetworkContextParamsInternal
  // for every NetworkContext of the profile (main and storage partitions
  // alike). With a run persona, sets `params`' accept_language (the persona's
  // languages) and user_agent (the persona's) and returns true. The run
  // persona does not change while contexts of the run live (a managed
  // Profile's contexts are retired before it is replaced), so a context never
  // needs a later update.
  bool ConfigureNetworkContextParams(
      network::mojom::NetworkContextParams* params) const;

 private:
  // Deletes prefs::kLegacyAgentFingerprintPersona from the kProfiles entries.
  void DropLegacyAgentPersonas();
  // Stores the persona back when reading it moved it onto this browser's
  // version (StoredFingerprintPersona::version_migrated), so it reads the
  // same from now on.
  void StoreMigratedPersona();

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Profile> profile_;
  ChangeCallbackList change_callbacks_;
  std::optional<FingerprintPersona> run_persona_;
  bool run_persona_unreadable_ = false;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_H_

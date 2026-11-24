// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_MANAGED_PROFILE_BOOTSTRAP_H_
#define CHROME_BROWSER_SHUTTLE_MANAGED_PROFILE_BOOTSTRAP_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"

class Profile;

namespace shuttle {

// Bootstrap of managed (isolated) Chromium Profiles: contract section 6 of
// tools/shuttle/docs/PROFILES.md. CreateProfileAsync's initialized callback is
// too late (keyed services, among them ProfileNetworkContextService, already
// exist), so ProfileImpl::OnPrefsLoaded calls PrepareManagedProfile() after
// its success/shutdown checks and BEFORE OnLocaleReady, which is what creates
// bulk keyed services (BrowserContextDependencyManager::
// CreateBrowserContextServices). Early extension pref services are independent.
// Nothing in preparation constructs an identity keyed service or a
// StoragePartition; everything goes through PrefService and OSCryptAsync.
//
// Not yet compiled (see PROFILES.md, "Implementation status").

// Source capability only: content binds a pending pipe, then waits on the
// BrowserContext admission barrier before configuration/NetworkContext creation.
// Runtime validation is still mandatory; rollout is off by default.
constexpr bool IsolatedProfilesBootstrapSafe() {
  return true;
}

// What the registry knows about a Profile directory, as the bootstrap needs
// it. Implemented by BrowserProfileRouter and registered with
// SetManagedProfileProvider(); keeps this file independent of the router.
struct ManagedProfileIntent {
  ManagedProfileIntent();
  ManagedProfileIntent(const ManagedProfileIntent&);
  ManagedProfileIntent& operator=(const ManagedProfileIntent&);
  ~ManagedProfileIntent();

  std::string core_profile_key;
  std::string workspace_agent;
  int64_t lifecycle_epoch = 0;
  BrowserProfileState state = BrowserProfileState::kStopped;
  bool provisioned = false;
  // `persona_input_json` is a replacement (ReplacePersonaIntent) that is not
  // committed to the Profile prefs yet: the bootstrap applies it on this load
  // (a new seed, the old persona is overwritten even when unreadable).
  bool persona_replacement_pending = false;
  // Validated persona input and the encrypted proxy pref value ("" = none).
  std::string persona_input_json;
  std::string proxy_pref_json;
};

class ManagedProfileProvider {
 public:
  virtual ~ManagedProfileProvider() = default;
  // The registry entry whose Chromium directory name is `directory` (tombstones
  // included), or nullopt.
  virtual std::optional<ManagedProfileIntent> LookupByDirectory(
      std::string_view directory) = 0;
  // The bootstrap of `directory` finished: kNone, or the failure code the
  // router reports (persona_invalid, proxy_not_ready, profile_failed, ...).
  virtual void OnBootstrapFinished(std::string_view directory,
                                   BrowserProfileError error) = 0;
};

// At most one provider (the router); nullptr clears it. A managed Profile
// loaded while no provider is registered fails closed.
void SetManagedProfileProvider(ManagedProfileProvider* provider);

using ManagedBootstrapCallback = base::OnceCallback<void(bool success)>;

// Called by ProfileImpl::OnPrefsLoaded. For an unmanaged Profile (no marker
// pref and no registry entry for its directory) runs `completion(true)`
// synchronously and does nothing else: legacy and startup Profiles are not
// affected. For a managed Profile:
//  - the registry entry must exist and be `loading` (the router started the
//    load); a Profile that Chromium loads on its own (session restore,
//    --profile-directory, deletion helper) fails closed;
//  - first load: validates the persona input, generates the seed, then writes
//    persona + workspace agent entry + proxy + the managed marker through
//    PrefService (never the Preferences file) and waits for the commit;
//  - later loads: the committed persona must be readable (corrupt or missing
//    => persona_invalid, never regenerated, never a stock fallback) and the
//    workspace entry must exist; EXCEPT when the registry holds a pending
//    persona replacement (set while stopped, PROFILES.md section 7): the new
//    intent is validated, written through PrefService (new seed; cookies and
//    storage untouched) and committed before the load continues;
//  - with a proxy: waits for the os_crypt encryptor and checks the proxy
//    reads back (proxy_not_ready otherwise) before any ProfileProxyService
//    exists.
// `completion(false)` means "do not continue loading" (ProfileImpl reports
// the creation as failed). `allow_async` is false for synchronous creation: an
// outcome that would need to wait fails closed instead.
// `completion` never runs after `profile` is destroyed.
void PrepareManagedProfile(Profile* profile,
                           bool allow_async,
                           ManagedBootstrapCallback completion);

// True if `profile` carries the managed marker pref (also readable from the
// registry-less contexts: persona service, network stack).
bool IsManagedProfile(Profile* profile);

// After the Profile is initialized (router, `initializing` -> `ready`):
// checks the identity components of a managed Profile are really in place
// (persona readable, workspace entry present, proxy readable if configured).
// kNone when ready for workload.
BrowserProfileError VerifyManagedProfileIdentity(
    Profile* profile,
    const ManagedProfileIntent& intent);

// Full normalized persona comparison, including generated fields; the stored
// seed is reused for comparison, never exposed or regenerated by verification.
bool ManagedProfilePersonaMatchesIntent(Profile* profile,
                                        std::string_view persona_input_json);

// personaSummary of the committed persona (FingerprintPersonaToMcpValue, no
// seed), or nullopt when there is no readable persona.
std::optional<base::DictValue> ManagedProfilePersonaSummary(Profile* profile);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_MANAGED_PROFILE_BOOTSTRAP_H_

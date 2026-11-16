// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_
#define CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_

#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace shuttle {

// Re-applies the renderer preferences (see ApplyAgentRendererPrefs) of open
// tabs: an agent's tabs when its proxy changes (WebRTC policy), and ALL tabs of
// the profile when the profile's fingerprint persona or its own proxy
// preference changes (WebRTC policy). A page already
// shown keeps the user agent, client hints and languages it was loaded with
// until its next navigation or reload: the renderer reads them per document.
// It lives in the UI layer because it walks the tab strips; agent_proxy and
// agent_persona must not depend on that. Observes the profile's
// ProfileProxyService and ProfilePersonaService; without them, it is inert.
class ProxyTabSync : public ProfileProxyService::Observer {
 public:
  explicit ProxyTabSync(Profile* profile);
  ProxyTabSync(const ProxyTabSync&) = delete;
  ProxyTabSync& operator=(const ProxyTabSync&) = delete;
  ~ProxyTabSync() override;

  // ProfileProxyService::Observer:
  void OnAgentProxyChanged(const std::string& id) override;

 private:
  // ProfilePersonaService change callback: the persona is profile-wide, so
  // every open tab of the profile is re-synced.
  void OnPersonaChanged();
  // The profile's own proxy preference changed: it decides the WebRTC policy of
  // every tab (webrtc_policy.h), so every open tab of the profile is re-synced.
  void OnProfileProxyPrefChanged();
  // Re-applies the renderer preferences of every open tab of agent
  // `agent_id`, or of every open tab of the profile if it is empty.
  void SyncTabs(const std::string& agent_id);

  raw_ptr<Profile> profile_;
  base::ScopedObservation<ProfileProxyService, ProfileProxyService::Observer>
      observation_{this};
  base::CallbackListSubscription persona_subscription_;
  PrefChangeRegistrar pref_registrar_;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/proxy_tab_sync.h"

#include <string>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_partition.h"
#include "chrome/browser/shuttle/profile_persona_service.h"
#include "chrome/browser/shuttle/profile_persona_service_factory.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "chrome/browser/shuttle/proxy_auth.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/prefs/pref_watcher.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/renderer_preferences_util.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

namespace shuttle {

ProxyTabSync::ProxyTabSync(Profile* profile) : profile_(profile) {
  if (ProfileProxyService* service =
          ProfileProxyServiceFactory::GetForProfile(profile)) {
    observation_.Observe(service);
  }
  if (ProfilePersonaService* personas =
          ProfilePersonaServiceFactory::GetForProfile(profile)) {
    persona_subscription_ = personas->AddChangeCallback(base::BindRepeating(
        &ProxyTabSync::OnPersonaChanged, base::Unretained(this)));
  }
  if (profile_->GetPrefs()) {
    pref_registrar_.Init(profile_->GetPrefs());
    pref_registrar_.Add(
        proxy_config::prefs::kProxy,
        base::BindRepeating(&ProxyTabSync::OnProfileProxyPrefChanged,
                            base::Unretained(this)));
  }
}

ProxyTabSync::~ProxyTabSync() = default;

void ProxyTabSync::OnAgentProxyChanged(const std::string& id) {
  if (id.empty()) {
    return;
  }
  SyncTabs(id);
}

void ProxyTabSync::OnPersonaChanged() {
  // The persona is profile-wide: every open tab.
  SyncTabs(std::string());
}

void ProxyTabSync::OnProfileProxyPrefChanged() {
  SyncTabs(std::string());
}

void ProxyTabSync::SyncTabs(const std::string& agent_id) {
  if (!profile_) {
    return;
  }
  for (BrowserWindowInterface* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile() != profile_) {
      continue;
    }
    TabStripModel* tab_strip = browser->GetTabStripModel();
    if (!tab_strip) {
      continue;
    }
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* contents = tab_strip->GetWebContentsAt(i);
      if (!agent_id.empty() && AgentIdForWebContents(contents) != agent_id) {
        continue;
      }
      blink::RendererPreferences* prefs = contents->GetMutableRendererPrefs();
      // Recomputes what the persona overrides (languages) from the profile
      // first: a cleared persona leaves no stale value behind.
      renderer_preferences_util::UpdateFromSystemSettings(prefs, profile_);
      ApplyAgentRendererPrefs(profile_, contents, prefs);
      contents->SyncRendererPrefs();
    }
  }
  // A profile-wide change also reaches the profile's shared and service
  // workers (their prefs come from the profile only, not from a tab; dedicated
  // workers follow their page above). An agent's own change does not: workers
  // cannot be attributed to an agent.
  if (agent_id.empty()) {
    if (PrefWatcher* watcher = PrefWatcherFactory::GetForProfile(profile_)) {
      watcher->NotifyRendererPreferenceWatchers();
    }
  }
}

}  // namespace shuttle

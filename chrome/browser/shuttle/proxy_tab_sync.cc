// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/proxy_tab_sync.h"

#include <string>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_partition.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "chrome/browser/shuttle/proxy_auth.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"

namespace shuttle {

ProxyTabSync::ProxyTabSync(Profile* profile) : profile_(profile) {
  if (ProfileProxyService* service =
          ProfileProxyServiceFactory::GetForProfile(profile)) {
    observation_.Observe(service);
  }
}

ProxyTabSync::~ProxyTabSync() = default;

void ProxyTabSync::OnAgentProxyChanged(const std::string& id) {
  if (!profile_ || id.empty()) {
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
      if (AgentIdForWebContents(contents) != id) {
        continue;
      }
      ApplyAgentRendererPrefs(profile_, contents,
                              contents->GetMutableRendererPrefs());
      contents->SyncRendererPrefs();
    }
  }
}

}  // namespace shuttle

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_
#define CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"

class Profile;

namespace shuttle {

// Re-applies the renderer preferences (WebRTC policy, see
// ApplyAgentRendererPrefs) of an agent's open tabs when the agent's proxy
// changes, so the change applies without a reload. It lives in the UI layer
// because it walks the tab strips; agent_proxy must not depend on that.
// Observes the profile's ProfileProxyService; without one, it is inert.
class ProxyTabSync : public ProfileProxyService::Observer {
 public:
  explicit ProxyTabSync(Profile* profile);
  ProxyTabSync(const ProxyTabSync&) = delete;
  ProxyTabSync& operator=(const ProxyTabSync&) = delete;
  ~ProxyTabSync() override;

  // ProfileProxyService::Observer:
  void OnAgentProxyChanged(const std::string& id) override;

 private:
  raw_ptr<Profile> profile_;
  base::ScopedObservation<ProfileProxyService, ProfileProxyService::Observer>
      observation_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROXY_TAB_SYNC_H_

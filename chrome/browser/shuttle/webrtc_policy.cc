// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/webrtc_policy.h"

#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/peerconnection/webrtc_ip_handling_policy.mojom-shared.h"

namespace shuttle {

bool ProfileHasExplicitProxy(const PrefService* prefs) {
  if (!prefs) {
    return false;
  }
  ProxyConfigDictionary config(
      prefs->GetDict(proxy_config::prefs::kProxy).Clone());
  ProxyPrefs::ProxyMode mode = ProxyPrefs::MODE_DIRECT;
  if (!config.GetMode(&mode)) {
    return false;
  }
  return mode == ProxyPrefs::MODE_FIXED_SERVERS ||
         mode == ProxyPrefs::MODE_PAC_SCRIPT;
}

bool ApplyProxiedWebRtcPolicy(const PrefService* profile_prefs,
                              bool agent_has_proxy,
                              blink::RendererPreferences* prefs) {
  if (!prefs) {
    return false;
  }
  if (!agent_has_proxy && !ProfileHasExplicitProxy(profile_prefs)) {
    return false;
  }
  prefs->webrtc_ip_handling_policy =
      blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
  prefs->webrtc_ip_handling_urls.clear();
  return true;
}

}  // namespace shuttle

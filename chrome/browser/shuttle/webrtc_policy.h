// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_WEBRTC_POLICY_H_
#define CHROME_BROWSER_SHUTTLE_WEBRTC_POLICY_H_

class PrefService;

namespace blink {
struct RendererPreferences;
}

namespace shuttle {

// Native WebRTC IP privacy, decided per profile and per agent. It only picks
// Chromium's own blink::mojom::WebRtcIpHandlingPolicy; it never rewrites
// getStats, SDP, codec capabilities or candidates, and injects nothing (no JS,
// no CDP).
//
// The strict policy is disable_non_proxied_udp: ICE only uses a route that
// goes through the proxy, so no host or server-reflexive candidate carries the
// real local or public address. It is the strictest policy Chromium has.
//
// Known limits (also in tools/shuttle/docs/WEBRTC.md):
//  - It only applies when the traffic IS proxied. It does not hide an IP behind
//    a proxy that cannot carry UDP: an HTTP/HTTPS proxy has no UDP, so WebRTC
//    media/data channels simply fail to connect (that is the point: no leak),
//    while a SOCKS5 proxy with UDP ASSOCIATE support lets them work through it.
//  - TURN: a TURN server a page names is reached only through the proxy under
//    this policy. TURN over TCP/TLS through an HTTP proxy works; TURN over UDP
//    needs a UDP-capable proxy. Chromium cannot make a direct TURN
//    relay private: a page that is not proxied exposes the address it uses.
//  - Not covered: workers of an agent (their agent cannot be attributed, see
//    ApplyProfileWebRtcPolicyToWorkerPrefs), a proxy chosen by the system
//    (mode "system" or "auto_detect": the profile cannot tell if one applies),
//    and browser-level traffic.

// True when the profile's own proxy preference names a proxy: fixed servers or
// a PAC script (set by the command line, a policy, an extension or settings).
// "direct", "auto_detect" and "system" are false: they do not tell whether a
// proxy applies. UI thread only.
bool ProfileHasExplicitProxy(const PrefService* prefs);

// Forces `prefs` to disable_non_proxied_udp and drops its per-URL rules
// (webrtc_ip_handling_urls: a matching rule would override the policy) when
// `agent_has_proxy` is true or ProfileHasExplicitProxy(`profile_prefs`). The
// result is the same for every tab of the profile and every agent. Returns
// true when it forced the policy; otherwise leaves `prefs` untouched (they keep
// the profile's own setting, kWebRTCIPHandlingPolicy, as set by
// renderer_preferences_util::UpdateFromSystemSettings). It never relaxes a
// policy. Null arguments: no change, false.
bool ApplyProxiedWebRtcPolicy(const PrefService* profile_prefs,
                              bool agent_has_proxy,
                              blink::RendererPreferences* prefs);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_WEBRTC_POLICY_H_

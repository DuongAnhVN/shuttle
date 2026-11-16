// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROXY_AUTH_H_
#define CHROME_BROWSER_SHUTTLE_PROXY_AUTH_H_

#include <memory>
#include <optional>
#include <string>

#include "content/public/browser/login_delegate.h"
#include "net/base/auth.h"

class Profile;

namespace blink {
struct RendererPreferences;
}

namespace content {
class WebContents;
}

namespace shuttle {

// Credentials for a proxy challenge of agent `agent_id`, or nullopt (the
// caller then cancels). `first_auth_attempt` is the per-request flag from
// content::ContentBrowserClient::CreateLoginDelegate: true for the first
// challenge of a request, false once the request has already been answered
// with credentials. Failures are recorded in ProfileProxyService
// (MarkProxyAuthFailed).
//  - Backoff active (ProxyAuthBackoffActive): nullopt, no credentials sent.
//  - The agent has no readable proxy, or the challenge is not from its proxy
//    (ProxyMatchesChallenger): nullopt.
//  - The proxy matches but the agent has no username: marked failed, nullopt.
//  - first_auth_attempt false: the credentials sent for this request were
//    rejected. Marked failed, nullopt (no 407 loop).
//  - Otherwise the credentials.
// Credential-free: this never shows UI, whatever do_not_prompt_for_login says.
std::optional<net::AuthCredentials> ProxyCredentialsForAgent(
    Profile* profile,
    const std::string& agent_id,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt);

// True once a rejection, or a matching challenge the agent has no credentials
// for, was seen since the agent's proxy last changed. Forwards to
// ProfileProxyService::ProxyAuthFailed; false without a profile.
bool ProxyAuthFailed(Profile* profile, const std::string& agent_id);

// Clears the auth failure state of `agent_id` (ProfileProxyService). Called
// when its proxy changes, and by the proxy check before a test.
void ResetProxyAuthState(Profile* profile, const std::string& agent_id);

// Login delegate for a proxy challenge from an agent tab. Answers with
// ProxyCredentialsForAgent (same arguments), asynchronously (posted, never
// inside the call). Returns nullptr to cancel (the request fails; no dialog).
std::unique_ptr<content::LoginDelegate> CreateAgentProxyLoginDelegate(
    Profile* profile,
    const std::string& agent_id,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback);

// Sets the WebRTC IP handling policy of `prefs` (the renderer preferences of
// `contents`). A tab whose agent has a proxy (ProfileProxyService::Lookup is
// not kNone), and EVERY tab of a profile whose own proxy preference names a
// proxy (webrtc_policy.h, ProfileHasExplicitProxy), gets
// disable_non_proxied_udp, so WebRTC cannot reveal the real IP; its per-URL
// rules (webrtc_ip_handling_urls) are cleared, since a matching rule would
// otherwise override that policy. Any other agent gets the profile's setting
// (kWebRTCIPHandlingPolicy); other tabs keep what UpdateFromSystemSettings set.
// Native policy only: no spoofing of getStats, SDP or candidates. Call it after
// renderer_preferences_util::UpdateFromSystemSettings.
//
// For EVERY tab of the profile (agent or not) it first applies the profile's
// fingerprint persona (ProfilePersonaService, ApplyProfilePersonaToWebContents
// in persona_tab_prefs.h): user agent and client hints, languages, platform,
// hardware concurrency, device memory, screen size and color depth,
// devicePixelRatio. A profile without a readable persona keeps its values; a
// persona applied earlier and since removed is cleared. The persona does not
// depend on the agent or its partition.
void ApplyAgentRendererPrefs(Profile* profile,
                             content::WebContents* contents,
                             blink::RendererPreferences* prefs);

// Worker counterpart of the WebRTC part of ApplyAgentRendererPrefs. The agent
// of a worker cannot be attributed, so only the profile's own explicit proxy
// is considered: with one, `prefs` get disable_non_proxied_udp; otherwise they
// are left untouched. An agent's own proxy does not reach its workers (known
// gap). Call it after UpdateFromSystemSettings; null arguments are ignored.
void ApplyProfileWebRtcPolicyToWorkerPrefs(Profile* profile,
                                           blink::RendererPreferences* prefs);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROXY_AUTH_H_

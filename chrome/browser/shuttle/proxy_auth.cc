// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/proxy_auth.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/agent_partition.h"
#include "chrome/browser/shuttle/agent_proxy.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "net/base/auth.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

namespace shuttle {

namespace {

// True when the agent's proxy is set, or its password is undecryptable.
// kCorrupt counts as proxied on purpose: it fails closed (the agent runs
// with a config that cannot connect, see agent_proxy.h), so non-proxied UDP
// stays disabled for it too. UI thread only.
bool AgentHasProxy(Profile* profile, const std::string& agent_id) {
  ProfileProxyService* service =
      ProfileProxyServiceFactory::GetForProfile(profile);
  if (!service) {
    return false;
  }
  switch (service->Lookup(agent_id).state) {
    case StoredProxy::State::kProxy:
    case StoredProxy::State::kCorrupt:
      return true;
    case StoredProxy::State::kNone:
      return false;
  }
  return false;
}

// Answers a proxy challenge with fixed credentials. The answer is posted: the
// callback must not run inside CreateLoginDelegate. If the delegate is
// destroyed first (the request was canceled), the answer is dropped.
class AgentProxyLoginDelegate : public content::LoginDelegate {
 public:
  AgentProxyLoginDelegate(
      net::AuthCredentials credentials,
      content::LoginDelegate::LoginAuthRequiredCallback callback)
      : credentials_(std::move(credentials)), callback_(std::move(callback)) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&AgentProxyLoginDelegate::Answer,
                                  weak_factory_.GetWeakPtr()));
  }

  ~AgentProxyLoginDelegate() override = default;

 private:
  void Answer() {
    // Moves the credentials out: they are not kept past the answer. Runs the
    // callback last: it may destroy this delegate.
    std::move(callback_).Run(std::optional<net::AuthCredentials>(
        std::move(credentials_)));
  }

  net::AuthCredentials credentials_;
  content::LoginDelegate::LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<AgentProxyLoginDelegate> weak_factory_{this};
};

}  // namespace

std::optional<net::AuthCredentials> ProxyCredentialsForAgent(
    Profile* profile,
    const std::string& agent_id,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt) {
  if (!profile || agent_id.empty() || !auth_info.is_proxy) {
    return std::nullopt;
  }
  ProfileProxyService* service =
      ProfileProxyServiceFactory::GetForProfile(profile);
  if (!service) {
    return std::nullopt;
  }
  if (service->ProxyAuthBackoffActive(agent_id)) {
    // A recent rejection: send nothing, so a hostile proxy cannot harvest the
    // credentials with one challenge per request.
    return std::nullopt;
  }
  std::optional<ProxySpec> spec = service->Get(agent_id);
  if (!spec || !ProxyMatchesChallenger(*spec, auth_info)) {
    return std::nullopt;
  }
  if (spec->username.empty()) {
    // The proxy asks for credentials this agent doesn't have.
    service->MarkProxyAuthFailed(agent_id);
    return std::nullopt;
  }
  if (!first_auth_attempt) {
    // The request was already answered with these credentials and the proxy
    // refused them again: a wrong password. Cancel instead of looping.
    service->MarkProxyAuthFailed(agent_id);
    return std::nullopt;
  }
  return net::AuthCredentials(base::UTF8ToUTF16(spec->username),
                              base::UTF8ToUTF16(spec->password));
}

bool ProxyAuthFailed(Profile* profile, const std::string& agent_id) {
  ProfileProxyService* service =
      profile ? ProfileProxyServiceFactory::GetForProfile(profile) : nullptr;
  return service && service->ProxyAuthFailed(agent_id);
}

void ResetProxyAuthState(Profile* profile, const std::string& agent_id) {
  ProfileProxyService* service =
      profile ? ProfileProxyServiceFactory::GetForProfile(profile) : nullptr;
  if (service) {
    service->ResetProxyAuthState(agent_id);
  }
}

std::unique_ptr<content::LoginDelegate> CreateAgentProxyLoginDelegate(
    Profile* profile,
    const std::string& agent_id,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  std::optional<net::AuthCredentials> credentials = ProxyCredentialsForAgent(
      profile, agent_id, auth_info, first_auth_attempt);
  if (!credentials) {
    // nullptr: the caller cancels the authentication. No dialog.
    return nullptr;
  }
  return std::make_unique<AgentProxyLoginDelegate>(
      std::move(*credentials), std::move(auth_required_callback));
}

void ApplyAgentRendererPrefs(Profile* profile,
                             content::WebContents* contents,
                             blink::RendererPreferences* prefs) {
  std::string agent_id = AgentIdForWebContents(contents);
  if (agent_id.empty() || !profile || !prefs) {
    return;
  }

  if (AgentHasProxy(profile, agent_id)) {
    prefs->webrtc_ip_handling_policy =
        blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
    // A profile-wide per-URL rule matching this page would override the
    // policy above: drop them for this tab.
    prefs->webrtc_ip_handling_urls.clear();
    return;
  }
  prefs->webrtc_ip_handling_policy = blink::ToWebRTCIPHandlingPolicy(
      profile->GetPrefs()->GetString(prefs::kWebRTCIPHandlingPolicy));
}

}  // namespace shuttle

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/proxy_check.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/shuttle/proxy_auth.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/auth.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/http/http_response_headers.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace shuttle {

namespace {

constexpr base::TimeDelta kTimeout = base::Seconds(15);
constexpr size_t kMaxBodyBytes = 4 * 1024;
constexpr size_t kMaxBodyChars = 256;  // UTF-16 code units.

// The test's own traffic, the one request a user asks for by hand. Its
// cookies are not sent (see ProxyCheckRequest::Start).
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("shuttle_agent_proxy_test", R"(
      semantics {
        sender: "Shuttle"
        description:
          "Checks an agent's proxy: loads one URL through it and reports the "
          "response (by default, the IP address the proxy exits with)."
        trigger: "The test-proxy MCP tool."
        data:
          "The URL given to the tool (by default an IP echo service), with no "
          "cookies. The proxy user name and password go only to the proxy "
          "that asks for them."
        destination: OTHER
        destination_other:
          "The URL given to the tool, through the agent's proxy."
      }
      policy {
        cookies_allowed: NO
        setting:
          "Not user-controllable: it runs only when an MCP client calls the "
          "test-proxy tool."
        policy_exception_justification: "Not implemented."
      })");

// Answers one request's proxy challenges with the agent's credentials. The
// network service sends a request's challenges to the observer of its
// factory, or of a clone of it (Clone), so both are served from receivers_.
// Nothing but a proxy challenge gets credentials: a server asking for a
// password is cancelled.
class AgentAuthObserver
    : public network::mojom::URLLoaderNetworkServiceObserver {
 public:
  AgentAuthObserver(Profile* profile, std::string agent_id)
      : profile_(profile), agent_id_(std::move(agent_id)) {}
  AgentAuthObserver(const AgentAuthObserver&) = delete;
  AgentAuthObserver& operator=(const AgentAuthObserver&) = delete;
  ~AgentAuthObserver() override = default;

  mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> Bind() {
    mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  // network::mojom::URLLoaderNetworkServiceObserver:
  void OnSSLCertificateError(const GURL& /*url*/,
                             int32_t net_error,
                             const net::SSLInfo& /*ssl_info*/,
                             bool /*fatal*/,
                             OnSSLCertificateErrorCallback response) override {
    // Certificate errors are never accepted: the request fails with them.
    std::move(response).Run(net_error);
  }

  void OnCertificateRequested(
      const std::optional<base::UnguessableToken>& /*window_id*/,
      const scoped_refptr<net::SSLCertRequestInfo>& /*cert_info*/,
      mojo::PendingRemote<network::mojom::ClientCertificateResponder>
          cert_responder) override {
    mojo::Remote<network::mojom::ClientCertificateResponder>(
        std::move(cert_responder))
        ->ContinueWithoutCertificate();
  }

  void OnAuthRequired(
      const std::optional<base::UnguessableToken>& /*window_id*/,
      int32_t /*request_id*/,
      const GURL& /*url*/,
      bool first_auth_attempt,
      bool /*do_not_prompt_for_login*/,
      const net::AuthChallengeInfo& auth_info,
      const scoped_refptr<net::HttpResponseHeaders>& /*head_headers*/,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder>
          auth_challenge_responder) override {
    std::optional<net::AuthCredentials> credentials;
    if (auth_info.is_proxy) {
      credentials = ProxyCredentialsForAgent(profile_, agent_id_, auth_info,
                                             first_auth_attempt);
    }
    mojo::Remote<network::mojom::AuthChallengeResponder>(
        std::move(auth_challenge_responder))
        ->OnAuthCredentials(credentials);
  }

  void OnLocalNetworkAccessPermissionRequired(
      network::mojom::TransportType /*transport_type*/,
      network::mojom::IPAddressSpace /*ip_address_space*/,
      OnLocalNetworkAccessPermissionRequiredCallback callback) override {
    // The test never reaches the local network on purpose: no prompt.
    std::move(callback).Run(network::mojom::LocalNetworkAccessResult::kDenied);
  }

  void OnPlatformLocalNetworkPermissionRequired(
      OnPlatformLocalNetworkPermissionRequiredCallback callback) override {
    std::move(callback).Run(/*granted=*/false);
  }

  void OnClearSiteData(
      const GURL& /*url*/,
      const std::optional<net::SchemefulSite>& /*top_level_site*/,
      const std::optional<base::UnguessableToken>& /*nonce*/,
      const std::string& /*header_value*/,
      int /*load_flags*/,
      const std::optional<net::CookiePartitionKey>& /*cookie_partition_key*/,
      bool /*partitioned_state_allowed_only*/,
      OnClearSiteDataCallback callback) override {
    std::move(callback).Run();
  }

  void OnLoadingStateUpdate(network::mojom::LoadInfoPtr /*info*/,
                            OnLoadingStateUpdateCallback callback) override {
    std::move(callback).Run();
  }

  void OnDataUseUpdate(int32_t /*network_traffic_annotation_id_hash*/,
                       base::ByteSize /*recv_bytes*/,
                       base::ByteSize /*sent_bytes*/) override {}

  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
          listener) override {
    receivers_.Add(this, std::move(listener));
  }

  void OnWebSocketConnectedToLocalNetwork(
      const GURL& /*request_url*/,
      network::mojom::IPAddressSpace /*ip_address_space*/) override {}

  void OnUrlLoaderConnectedToLocalNetwork(
      const GURL& /*request_url*/,
      network::mojom::IPAddressSpace /*response_address_space*/,
      network::mojom::IPAddressSpace /*client_address_space*/,
      network::mojom::IPAddressSpace /*target_address_space*/) override {}

 private:
  raw_ptr<Profile> profile_;
  std::string agent_id_;
  mojo::ReceiverSet<network::mojom::URLLoaderNetworkServiceObserver>
      receivers_;
};

// One test request. Owns its factory, loader and observer, and deletes itself
// once it has reported (Finish). It also watches its profile: if the profile
// is destroyed first, it reports profile_closed and deletes itself, so the
// raw profile pointer is never used after that.
class ProxyCheckRequest : public ProfileObserver {
 public:
  ProxyCheckRequest(Profile* profile,
                    const std::string& agent_id,
                    ProxyCheckCallback callback)
      : profile_(profile),
        agent_id_(agent_id),
        callback_(std::move(callback)),
        observer_(profile, agent_id) {
    profile_observation_.Observe(profile);
  }
  ProxyCheckRequest(const ProxyCheckRequest&) = delete;
  ProxyCheckRequest& operator=(const ProxyCheckRequest&) = delete;
  ~ProxyCheckRequest() override = default;

  void Start(network::mojom::NetworkContext* network_context, const GURL& url) {
    // The factory mirrors the browser-process factories of the partition
    // (StoragePartitionImpl::CreateURLLoaderFactoryParams), with this
    // observer added: it is what answers the proxy's 407.
    network::mojom::URLLoaderFactoryParamsPtr params =
        network::mojom::URLLoaderFactoryParams::New();
    params->process_id = network::OriginatingProcessId::browser();
    params->automatically_assign_isolation_info = true;
    params->is_orb_enabled = false;
    params->is_trusted = true;
    params->url_loader_network_observer = observer_.Bind();
    network_context->CreateURLLoaderFactory(
        factory_.BindNewPipeAndPassReceiver(), std::move(params));

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    // Only the agent's proxy is involved, not its cookies.
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    // A public URL must not redirect into a private one: redirects fail.
    request->redirect_mode = network::mojom::RedirectMode::kError;
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kTrafficAnnotation);
    loader_->SetTimeoutDuration(kTimeout);
    start_ = base::TimeTicks::Now();
    loader_->DownloadToString(
        factory_.get(),
        base::BindOnce(&ProxyCheckRequest::OnDownloaded,
                       base::Unretained(this)),
        kMaxBodyBytes);
  }

 private:
  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override {
    // Stop observing before the profile is gone; Finish deletes this.
    profile_observation_.Reset();
    ProxyCheckResult result;
    result.profile_closed = true;
    result.net_error = net::ERR_ABORTED;
    Finish(std::move(result));
  }

  void OnDownloaded(std::optional<std::string> body) {
    ProxyCheckResult result;
    result.net_error = loader_->NetError();
    result.ok = result.net_error == net::OK;
    const network::mojom::URLResponseHead* head = loader_->ResponseInfo();
    if (head && head->headers) {
      result.status = head->headers->response_code();
    }
    if (body) {
      std::u16string text = base::UTF8ToUTF16(*body);
      if (text.size() > kMaxBodyChars) {
        text.resize(kMaxBodyChars);
      }
      result.body = base::UTF16ToUTF8(text);
    }
    result.latency = base::TimeTicks::Now() - start_;
    result.proxy_auth_failed = ProxyAuthFailed(profile_, agent_id_);
    Finish(std::move(result));
  }

  // Reports once and deletes this. The callback is moved out first and runs
  // after the request is gone, so it may do anything (even start a new test).
  // Nothing in this object may be touched after `delete this`.
  void Finish(ProxyCheckResult result) {
    ProxyCheckCallback callback = std::move(callback_);
    delete this;
    std::move(callback).Run(std::move(result));
  }

  raw_ptr<Profile> profile_;
  std::string agent_id_;
  ProxyCheckCallback callback_;
  AgentAuthObserver observer_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::TimeTicks start_;
};

}  // namespace

std::string CheckProxyTestUrl(const GURL& url) {
  constexpr char kNotPublic[] =
      "url must be a public http or https address (no localhost, loopback, "
      "private, link-local or unique-local addresses)";
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return "url must be an http or https URL";
  }
  if (url.has_username() || url.has_password()) {
    return "url must not contain a username or password";
  }
  std::string host(url.HostNoBracketsPiece());
  if (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  if (host == "localhost" || base::EndsWith(host, ".localhost")) {
    return kNotPublic;
  }
  if (url.HostIsIPAddress()) {
    net::IPAddress ip;
    if (!ip.AssignFromIPLiteral(url.HostNoBracketsPiece())) {
      return kNotPublic;
    }
    // ::ffff:a.b.c.d is the IPv4 address a.b.c.d for these checks.
    if (ip.IsIPv4MappedIPv6()) {
      ip = net::ConvertIPv4MappedIPv6ToIPv4(ip);
    }
    if (!ip.IsPubliclyRoutable() || ip.IsLoopback() || ip.IsLinkLocal() ||
        ip.IsUniqueLocalIPv6() || ip.IsZero()) {
      return kNotPublic;
    }
  }
  return std::string();
}

void RunProxyCheck(network::mojom::NetworkContext* network_context,
                   Profile* profile,
                   const std::string& agent_id,
                   const GURL& url,
                   ProxyCheckCallback callback) {
  // A test is an explicit retry by the caller. A failed auth backs off for 60 s
  // (no credentials sent), which guards automatic loops only, so start clean.
  ResetProxyAuthState(profile, agent_id);
  // Owns itself until it reports (Finish).
  auto* request = new ProxyCheckRequest(profile, agent_id, std::move(callback));
  request->Start(network_context, url);
}

}  // namespace shuttle

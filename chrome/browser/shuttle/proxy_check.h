// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROXY_CHECK_H_
#define CHROME_BROWSER_SHUTTLE_PROXY_CHECK_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "base/time/time.h"
#include "url/gurl.h"

class Profile;

namespace network::mojom {
class NetworkContext;
}

namespace shuttle {

// The outcome of one test-proxy request.
struct ProxyCheckResult {
  // The request completed (net::OK). The HTTP status is reported as is, not
  // judged: a 403 from the target still proves the proxy path works.
  bool ok = false;
  base::TimeDelta latency;
  // Set once a response head arrived.
  std::optional<int> status;
  // The first 256 UTF-16 code units of the body, if the request succeeded.
  std::optional<std::string> body;
  // The request's net::Error (net::OK when ok). net::ERR_ABORTED when
  // `profile_closed`.
  int net_error = 0;
  // ProxyAuthFailed() for the agent, after the request.
  bool proxy_auth_failed = false;
  // The agent's profile was destroyed before the request finished.
  bool profile_closed = false;
};

using ProxyCheckCallback = base::OnceCallback<void(ProxyCheckResult)>;

// "" if `url` may be the target of a proxy test, else why not. Only public
// http or https targets: no localhost, loopback, private, link-local,
// unique-local or unspecified addresses (IPv4-mapped forms included), and no
// username or password in the URL. Checked before RunProxyCheck; names are
// not resolved here, so a public name that resolves privately is not caught.
std::string CheckProxyTestUrl(const GURL& url);

// Loads `url` (http or https) through `network_context`, the NetworkContext of
// the agent's own partition, so through the agent's proxy. The proxy's 407 is
// answered with the agent's credentials (ProxyCredentialsForAgent); nothing
// else gets credentials. No cookies are sent. Redirects are not followed (they
// fail the request). Times out after 15 s. A body over 4 KiB fails the request
// with net::ERR_INSUFFICIENT_RESOURCES (DownloadToString's cap); the body is
// then not reported.
//
// Resets the agent's proxy auth state first (ResetProxyAuthState): a failed
// auth backs off for 60 s, sending no credentials, which only guards automatic
// retry loops. A test is an explicit retry by the caller, so it starts clean.
//
// Requires CheckProxyTestUrl(url) == "". If the profile is destroyed first,
// the callback runs once with profile_closed set. Otherwise it runs once when
// the request finishes.
void RunProxyCheck(network::mojom::NetworkContext* network_context,
                   Profile* profile,
                   const std::string& agent_id,
                   const GURL& url,
                   ProxyCheckCallback callback);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROXY_CHECK_H_

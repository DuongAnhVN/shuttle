// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_SCOPED_ROUTING_H_
#define CHROME_BROWSER_SHUTTLE_SCOPED_ROUTING_H_

#include <string>

#include "content/public/browser/devtools_manager_delegate.h"

namespace shuttle {

// Glue between the DevTools server and the isolated Chromium Profiles of
// tools/shuttle/docs/PROFILES.md (--shuttle-isolated-profiles): the CDP
// connection decision, and the Profile drain delegate. Everything here is a
// no-op / denies when the router does not exist (switch off).
//
// Not yet compiled (see PROFILES.md, "Implementation status").

// Installs the drain delegate in the router and, only then, declares scoped
// routing ready (get-capabilities `scopedRouting`). The chain it announces:
// the HTTP layer's per-socket ConnectionDecision, MCP routing before session
// touching (ShuttleService::HandleHttpMcpRequest), the filters of
// session_cdp_filter, and this drain delegate. Call once when the DevTools
// delegate is created; harmless when called again or with the switch off.
void InstallScopedRouting();

// The decision for one new CDP socket with isolated profiles on (the router
// exists): browser sockets are admin-only; session and page sockets resolve
// their owner through the browser-wide indexes, re-resolve it NOW (stale
// epoch => 404), and need the admin credential or the session's bearer grant
// (otherwise 403). Never returns a scoped decision with a null filter.
content::DevToolsManagerDelegate::ConnectionDecision DecideScopedConnection(
    content::DevToolsManagerDelegate::ConnectionKind kind,
    const std::string& session_id,
    const std::string& target_id,
    const content::DevToolsManagerDelegate::RequestContext& context);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_SCOPED_ROUTING_H_

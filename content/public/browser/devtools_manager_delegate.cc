// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/devtools_manager_delegate.h"

#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/devtools_agent_host.h"

namespace content {

DevToolsManagerDelegate::DevToolsOptions::DevToolsOptions() = default;

DevToolsManagerDelegate::DevToolsOptions::DevToolsOptions(
    const DevToolsManagerDelegate::DevToolsOptions& other) = default;

DevToolsManagerDelegate::DevToolsOptions::DevToolsOptions(
    std::optional<std::string> panel_id)
    : panel_id(panel_id) {}

DevToolsManagerDelegate::DevToolsOptions::~DevToolsOptions() = default;

void DevToolsManagerDelegate::Inspect(DevToolsAgentHost* agent_host) {
}

scoped_refptr<DevToolsAgentHost> DevToolsManagerDelegate::GetDevToolsAgentHost(
    DevToolsAgentHost* agent_host) {
  return nullptr;
}

scoped_refptr<DevToolsAgentHost> DevToolsManagerDelegate::OpenDevTools(
    DevToolsAgentHost* agent_host,
    const DevToolsManagerDelegate::DevToolsOptions& devtools_options) {
  return nullptr;
}

void DevToolsManagerDelegate::Activate(DevToolsAgentHost* agent_host) {}

std::string DevToolsManagerDelegate::GetTargetType(WebContents* wc) {
  return std::string();
}

std::string DevToolsManagerDelegate::GetTargetTitle(WebContents* wc) {
  return std::string();
}

std::string DevToolsManagerDelegate::GetTargetDescription(WebContents* wc) {
  return std::string();
}

std::unique_ptr<base::DictValue> DevToolsManagerDelegate::GetTargetEmbedderData(
    DevToolsAgentHost* agent_host) {
  return nullptr;
}

bool DevToolsManagerDelegate::AllowInspectingRenderFrameHost(
    RenderFrameHost* rfh) {
  return true;
}

std::optional<bool> DevToolsManagerDelegate::ShouldReportAsTabTarget(
    WebContents* web_contents) {
  return std::nullopt;
}

DevToolsAgentHost::List DevToolsManagerDelegate::RemoteDebuggingTargets(
    DevToolsManagerDelegate::TargetType target_type) {
  return DevToolsAgentHost::GetOrCreateAll();
}

scoped_refptr<DevToolsAgentHost> DevToolsManagerDelegate::CreateNewTarget(
    const GURL& url,
    DevToolsManagerDelegate::TargetType target_type,
    bool new_window) {
  return nullptr;
}

std::vector<base::WeakPtr<BrowserContext>>
DevToolsManagerDelegate::GetBrowserContexts() {
  return std::vector<base::WeakPtr<BrowserContext>>();
}

BrowserContext* DevToolsManagerDelegate::GetDefaultBrowserContext() {
  return nullptr;
}

BrowserContext* DevToolsManagerDelegate::GetBrowserContext(
    const std::string& context_id) {
  return nullptr;
}

BrowserContext* DevToolsManagerDelegate::CreateBrowserContext() {
  return nullptr;
}

void DevToolsManagerDelegate::DisposeBrowserContext(BrowserContext*,
                                                    DisposeCallback callback) {
  std::move(callback).Run(false, "Browser Context disposal is not supported");
}

void DevToolsManagerDelegate::ClientAttached(
    DevToolsAgentHostClientChannel* channel) {}
void DevToolsManagerDelegate::ClientDetached(
    DevToolsAgentHostClientChannel* channel) {}

bool DevToolsManagerDelegate::AllowInspectingTarget(
    DevToolsAgentHost* agent_host) {
  return true;
}

void DevToolsManagerDelegate::HandleCommand(
    DevToolsAgentHostClientChannel* channel,
    base::span<const uint8_t> message,
    NotHandledCallback callback) {
  callback.Run(message);
}

std::string DevToolsManagerDelegate::GetDiscoveryPageHTML() {
  return std::string();
}

bool DevToolsManagerDelegate::HasBundledFrontendResources() {
  return false;
}

bool DevToolsManagerDelegate::IsBrowserTargetDiscoverable() {
  return false;
}

void DevToolsManagerDelegate::AcceptDebugging(AcceptCallback callback) {
  std::move(callback).Run(
      content::DevToolsManagerDelegate::AcceptConnectionResult::kDeny);
}

void DevToolsManagerDelegate::SetActiveWebSocketConnections(size_t count) {}

DevToolsManagerDelegate::RequestContext::RequestContext() = default;
DevToolsManagerDelegate::RequestContext::RequestContext(
    const RequestContext& other) = default;
DevToolsManagerDelegate::RequestContext&
DevToolsManagerDelegate::RequestContext::operator=(
    const RequestContext& other) = default;
DevToolsManagerDelegate::RequestContext::~RequestContext() = default;

DevToolsManagerDelegate::ConnectionDecision::ConnectionDecision() = default;
DevToolsManagerDelegate::ConnectionDecision::ConnectionDecision(
    ConnectionDecision&& other) = default;
DevToolsManagerDelegate::ConnectionDecision&
DevToolsManagerDelegate::ConnectionDecision::operator=(
    ConnectionDecision&& other) = default;
DevToolsManagerDelegate::ConnectionDecision::~ConnectionDecision() = default;

// static
DevToolsManagerDelegate::ConnectionDecision
DevToolsManagerDelegate::ConnectionDecision::AdminAllowed() {
  ConnectionDecision decision;
  decision.kind = Kind::kAdminAllowed;
  decision.http_status = 200;
  return decision;
}

// static
DevToolsManagerDelegate::ConnectionDecision
DevToolsManagerDelegate::ConnectionDecision::Scoped(
    std::unique_ptr<ConnectionFilter> filter) {
  ConnectionDecision decision;
  if (!filter) {
    // A scoped decision without a filter is a bug: fail closed.
    return Denied(403);
  }
  decision.kind = Kind::kScopedFilter;
  decision.filter = std::move(filter);
  decision.http_status = 200;
  return decision;
}

// static
DevToolsManagerDelegate::ConnectionDecision
DevToolsManagerDelegate::ConnectionDecision::Denied(int http_status) {
  ConnectionDecision decision;
  decision.kind = Kind::kDenied;
  decision.http_status = http_status == 404 ? 404 : 403;
  return decision;
}

bool DevToolsManagerDelegate::VerifyAdminToken(std::string_view presented) {
  return false;
}

bool DevToolsManagerDelegate::AllowBrowserGlobalHttp(
    const RequestContext& context) {
  return true;
}

std::unique_ptr<DevToolsManagerDelegate::ConnectionFilter>
DevToolsManagerDelegate::CreateConnectionFilter(
    const std::string& session_id,
    const std::string& target_id,
    const RequestContext& context) {
  return nullptr;
}

DevToolsManagerDelegate::ConnectionDecision
DevToolsManagerDelegate::DecideConnection(ConnectionKind kind,
                                          const std::string& session_id,
                                          const std::string& target_id,
                                          const RequestContext& context) {
  switch (kind) {
    case ConnectionKind::kBrowser:
      return ConnectionDecision::AdminAllowed();
    case ConnectionKind::kSession: {
      std::unique_ptr<ConnectionFilter> filter =
          CreateConnectionFilter(session_id, std::string(), context);
      return filter ? ConnectionDecision::Scoped(std::move(filter))
                    : ConnectionDecision::Denied(404);
    }
    case ConnectionKind::kPage: {
      std::unique_ptr<ConnectionFilter> filter =
          CreateConnectionFilter(std::string(), target_id, context);
      // Legacy semantics, spelled out: a page that is not a leased tab is
      // reachable raw. Embedders that isolate Profiles override this method.
      return filter ? ConnectionDecision::Scoped(std::move(filter))
                    : ConnectionDecision::AdminAllowed();
    }
  }
  return ConnectionDecision::Denied(403);
}

void DevToolsManagerDelegate::HandleMcpRequest(const std::string& host,
                                               const std::string& body,
                                               const RequestContext& context,
                                               HttpResponseCallback callback) {
  std::move(callback).Run(404, "text/plain", "Not found");
}

DevToolsManagerDelegate::~DevToolsManagerDelegate() = default;
}  // namespace content

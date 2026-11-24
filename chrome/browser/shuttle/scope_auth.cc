// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/scope_auth.h"

#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"
#include "crypto/random.h"
#include "crypto/secure_util.h"

namespace shuttle {

ParsedSelector ParseProfileSelector(const base::DictValue* params) {
  ParsedSelector parsed;
  if (!params) {
    return parsed;
  }
  const base::Value* meta = params->Find("_meta");
  if (!meta) {
    return parsed;
  }
  const base::DictValue* meta_dict = meta->GetIfDict();
  if (!meta_dict) {
    // `_meta` is an object by the MCP schema; anything else could hide a
    // selector, so it is malformed, never "absent".
    parsed.state = SelectorState::kMalformed;
    parsed.has_selector_key = true;
    return parsed;
  }
  const base::Value* value = meta_dict->Find(kProfileScopeMetaKey);
  if (!value) {
    return parsed;
  }
  parsed.has_selector_key = true;
  const std::string* key = value->GetIfString();
  if (!key || !IsValidBrowserProfileKey(*key)) {
    parsed.state = SelectorState::kMalformed;
    return parsed;
  }
  parsed.state = SelectorState::kPresent;
  parsed.key = *key;
  return parsed;
}

namespace {

ConnectionAuthResult Allow(ConnectionVerdict verdict) {
  ConnectionAuthResult result;
  result.verdict = verdict;
  result.http_status = 200;
  return result;
}

ConnectionAuthResult Deny(int http_status) {
  ConnectionAuthResult result;
  result.verdict = ConnectionVerdict::kDenied;
  result.http_status = http_status == 404 ? 404 : 403;
  return result;
}

}  // namespace

ConnectionAuthResult DecideConnectionAuth(const ConnectionAuthInput& input) {
  if (!input.isolated_enabled) {
    // Legacy semantics, explicit.
    switch (input.target) {
      case ConnectionTarget::kBrowser:
        return Allow(ConnectionVerdict::kAdminAllowed);
      case ConnectionTarget::kSession:
        return input.legacy_scoped_available
                   ? Allow(ConnectionVerdict::kScopedFilter)
                   : Deny(404);
      case ConnectionTarget::kPage:
        return input.legacy_scoped_available
                   ? Allow(ConnectionVerdict::kScopedFilter)
                   : Allow(ConnectionVerdict::kAdminAllowed);
    }
    return Deny(403);
  }

  switch (input.target) {
    case ConnectionTarget::kBrowser:
      return input.is_admin ? Allow(ConnectionVerdict::kAdminAllowed)
                            : Deny(403);

    case ConnectionTarget::kSession:
      if (!input.located) {
        return Deny(404);
      }
      // A stale handle (epoch bumped by a stop/restart, other browser
      // incarnation, Profile gone or not ready) is unknown, not forbidden.
      if (!input.current || !(*input.located == *input.current)) {
        return Deny(404);
      }
      if (input.is_admin || input.grant_valid) {
        return Allow(ConnectionVerdict::kScopedFilter);
      }
      return Deny(403);

    case ConnectionTarget::kPage:
      if (!input.located) {
        // Not a leased tab/popup/worker of any session.
        if (input.is_admin) {
          return Allow(ConnectionVerdict::kAdminAllowed);
        }
        return Deny(input.target_exists ? 403 : 404);
      }
      if (!input.current || !(*input.located == *input.current)) {
        return Deny(404);
      }
      if (input.is_admin) {
        return Allow(ConnectionVerdict::kAdminAllowed);
      }
      if (input.grant_valid) {
        return Allow(ConnectionVerdict::kScopedFilter);
      }
      return Deny(403);
  }
  return Deny(403);
}

namespace {

CdpCommandDecision DenyCommand(std::string message) {
  CdpCommandDecision decision;
  decision.verdict = CdpCommandVerdict::kDeny;
  decision.message = std::move(message);
  return decision;
}

bool IsFlattened(const base::DictValue* params) {
  return params && params->FindBool("flatten").value_or(false);
}

}  // namespace

CdpCommandDecision DecideScopedCdpCommand(const CdpCommandInput& input) {
  const std::string_view method = input.method;
  const base::DictValue* params = input.params;

  // Nested CDP: a command (or an event) tunnelled through Target.* would reach
  // the agent host without passing this policy.
  if (method == "Target.sendMessageToTarget" ||
      method == "Target.receivedMessageFromTarget") {
    return DenyCommand(
        "Shuttle: tunnelled protocol messages are not available to a scoped "
        "session (attach with flatten: true)");
  }
  if (method == "Target.attachToBrowserTarget") {
    return DenyCommand("Shuttle: an agent session can't attach to the browser");
  }
  if (method == "Target.createBrowserContext" ||
      method == "Target.disposeBrowserContext") {
    return DenyCommand(
        "Shuttle: an agent session has one browser context, its agent's "
        "storage (create agents over MCP instead)");
  }
  if ((method == "Target.attachToTarget" || method == "Target.setAutoAttach") &&
      !IsFlattened(params)) {
    return DenyCommand(
        "Shuttle: only flattened sessions are available (flatten: true)");
  }

  // A command may only name the connection's own browser context.
  if (params) {
    if (const base::Value* context = params->Find("browserContextId")) {
      const std::string* id = context->GetIfString();
      if (!id || *id != input.own_browser_context_id) {
        return DenyCommand(
            "Shuttle: a foreign browser context is not reachable from this "
            "session");
      }
    }
  }

  // Child/page handlers can resolve omitted context ids to the startup
  // Profile. Never let privileged domains reach those handlers.
  if (input.form != CdpForm::kBrowserRoot &&
      (method.starts_with("Browser.") || method.starts_with("Storage."))) {
    return DenyCommand("Shuttle: Browser/Storage commands require the session root");
  }
  if (method == "Storage.getCookies" || method == "Storage.setCookies" ||
      method == "Storage.clearCookies") {
    CdpCommandDecision decision;
    decision.verdict = CdpCommandVerdict::kRouteCookiesToAgent;
    return decision;
  }
  if (method.starts_with("Storage.")) {
    return DenyCommand(
        "Shuttle: storage commands other than cookies are not available to a "
        "scoped session");
  }
  if (method == "Browser.setWindowBounds" ||
      method == "Browser.getWindowBounds" ||
      method == "Browser.setContentsSize" ||
      method == "Browser.getBrowserCommandLine" ||
      method == "Browser.getHistograms" || method == "Browser.getHistogram") {
    return DenyCommand("Shuttle: not available to a scoped session");
  }
  // Without a browserContextId these act on the DevTools default context,
  // which is not a managed Profile's.
  if (input.managed &&
      (method == "Browser.setPermission" ||
       method == "Browser.grantPermissions" ||
       method == "Browser.resetPermissions" ||
       method == "Browser.setDownloadBehavior" ||
       method == "Browser.cancelDownload") &&
      !(params && params->contains("browserContextId"))) {
    return DenyCommand("Shuttle: name the session's own browserContextId");
  }
  return CdpCommandDecision();
}

std::string GenerateSessionGrant() {
  std::string encoded;
  base::Base64UrlEncode(crypto::RandBytesAsVector(kSessionGrantBytes),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &encoded);
  return encoded;
}

SessionGrants::SessionGrants() = default;
SessionGrants::~SessionGrants() = default;

// static
SessionGrants& SessionGrants::Get() {
  static base::NoDestructor<SessionGrants> instance;
  return *instance;
}

std::string SessionGrants::Issue(const std::string& session_id,
                                 const OwnerTuple& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Entry entry;
  entry.token = GenerateSessionGrant();
  entry.owner = owner;
  std::string token = entry.token;
  grants_[session_id] = std::move(entry);
  return token;
}

std::string SessionGrants::GrantFor(std::string_view session_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = grants_.find(session_id);
  return it == grants_.end() ? std::string() : it->second.token;
}

bool SessionGrants::Verify(std::string_view session_id,
                           std::string_view presented,
                           const OwnerTuple& owner) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = grants_.find(session_id);
  if (it == grants_.end() || presented.empty()) {
    return false;
  }
  const std::string& expected = it->second.token;
  if (presented.size() != expected.size() ||
      !crypto::SecureMemEqual(base::as_byte_span(presented),
                              base::as_byte_span(expected))) {
    return false;
  }
  return it->second.owner == owner;
}

void SessionGrants::Revoke(std::string_view session_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = grants_.find(session_id);
  if (it != grants_.end()) {
    grants_.erase(it);
  }
}

void SessionGrants::RevokeAllForProfile(std::string_view profile_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::erase_if(grants_, [&](const auto& entry) {
    return entry.second.owner.profile_key == profile_key;
  });
}

}  // namespace shuttle

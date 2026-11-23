// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_SCOPE_AUTH_H_
#define CHROME_BROWSER_SHUTTLE_SCOPE_AUTH_H_

#include <stddef.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "chrome/browser/shuttle/browser_owner_index.h"

namespace shuttle {

// Pure logic of tools/shuttle/docs/PROFILES.md sections 3 and 4 that decides
// who may reach what: the profile selector parser, the CDP connection
// decision and the per-session bearer grants. No Profile, no content, no
// router: unit tested alone (scope_auth_unittest.cc).
//
// Not yet compiled (see PROFILES.md, "Implementation status").

// --- Profile selector (params._meta["shuttle/profileId"]) -----------------------

inline constexpr char kProfileScopeMetaKey[] = "shuttle/profileId";

enum class SelectorState {
  // No `_meta`, or `_meta` without the selector key: the startup Profile.
  kAbsent,
  // A well-formed key (^bp_[0-9a-f]{32}$). It names a Profile; it grants no
  // authority.
  kPresent,
  // `_meta` is not an object, or the value is not a string / not a key.
  kMalformed,
};

struct ParsedSelector {
  SelectorState state = SelectorState::kAbsent;
  // kPresent only.
  std::string key;
  // `_meta` carries the selector key at all (well-formed or not): browser-level
  // tools reject any selector.
  bool has_selector_key = false;
};

// `params` is the `params` object of a tools/call request (may be null).
ParsedSelector ParseProfileSelector(const base::DictValue* params);

// --- Connection decision (pure) ------------------------------------------------------

enum class ConnectionTarget { kBrowser, kSession, kPage };
enum class ConnectionVerdict { kAdminAllowed, kScopedFilter, kDenied };

struct ConnectionAuthInput {
  // --shuttle-isolated-profiles (a router exists).
  bool isolated_enabled = false;
  // The request carried a verified admin credential.
  bool is_admin = false;
  ConnectionTarget target = ConnectionTarget::kBrowser;

  // kSession / kPage with isolated profiles: the owner the index holds for the
  // session id / the target id (nullopt: not indexed).
  std::optional<OwnerTuple> located;
  // The owner `located` resolves to NOW (profile ready, current lifecycle
  // epoch and browser epoch); nullopt when the Profile is not ready or gone.
  std::optional<OwnerTuple> current;
  // The presented session grant verified against `located`.
  bool grant_valid = false;
  // kPage: the target exists at all (distinguishes 404 from 403).
  bool target_exists = true;
  // Legacy mode only: the embedder has a scoped filter for the connection
  // (the session is known / the tab is leased).
  bool legacy_scoped_available = false;
};

struct ConnectionAuthResult {
  ConnectionVerdict verdict = ConnectionVerdict::kDenied;
  // kDenied only: 404 (unknown or stale owner) or 403 (not authorized).
  int http_status = 403;
};

// The decision table of PROFILES.md section 4. A scoped verdict means "build
// the mandatory non-null filter"; nothing here ever means "unfiltered" except
// kAdminAllowed.
//   isolated off: browser -> admin; session -> scoped | 404; page -> scoped |
//     admin (legacy: an unleased page is reachable raw).
//   isolated on: browser -> admin | 403; session/page: unknown or stale owner
//     -> 404; admin -> admin (page) / scoped (session); valid grant ->
//     scoped; otherwise 403.
ConnectionAuthResult DecideConnectionAuth(const ConnectionAuthInput& input);

// --- Scoped CDP command policy (pure) -------------------------------------------------

// How the command reached the filter. The policy is the same for every form:
// a command is judged by its method and parameters, never by how deeply it is
// nested in the transport.
enum class CdpForm {
  kBrowserRoot,   // the session socket (browser-level, no sessionId)
  kPageRoot,      // a page socket (/devtools/page/<id>), no sessionId
  kChildSession,  // any command carrying a flattened child sessionId
};

enum class CdpCommandVerdict {
  kAllow,
  // Storage.getCookies/setCookies/clearCookies: answered from the owner
  // Profile's agent partition by the filter, never by the default context.
  kRouteCookiesToAgent,
  kDeny,
};

struct CdpCommandInput {
  std::string_view method;
  CdpForm form = CdpForm::kBrowserRoot;
  // The owner is a managed (isolated) Profile, not the startup one.
  bool managed = false;
  // `Profile::UniqueId()` of the connection's own Profile.
  std::string_view own_browser_context_id;
  raw_ptr<const base::DictValue> params = nullptr;
};

struct CdpCommandDecision {
  CdpCommandVerdict verdict = CdpCommandVerdict::kAllow;
  // kDeny only: fixed text, never contains caller input.
  std::string message;
};

// The one policy of a scoped connection for Browser.*, Storage.* and Target.*:
//  - Target.sendMessageToTarget / receivedMessageFromTarget (nested CDP
//    tunnelling), Target.attachToBrowserTarget, Target.create/
//    disposeBrowserContext: denied;
//  - Target.attachToTarget and Target.setAutoAttach only with flatten:true
//    (a non-flattened session would carry commands the filter cannot see);
//  - any browserContextId other than the own one: denied;
//  - Browser.* and Storage.* on pages/children: denied;
//  - root Storage.getCookies/setCookies/clearCookies: routed to the agent
//    partition; every other Storage.* denied;
//  - window bounds, command line and histogram commands denied;
//  - permission/download commands of a managed Profile need the own
//    browserContextId.
CdpCommandDecision DecideScopedCdpCommand(const CdpCommandInput& input);

// --- Per-session bearer grants ----------------------------------------------------------

inline constexpr size_t kSessionGrantBytes = 32;

// A random 256-bit token (base64url, no padding).
std::string GenerateSessionGrant();

// Per-session bearer grants of scoped endpoints: random, bound to the owner
// tuple, never the admin token, revoked when the session or its Profile goes
// away. UI thread only.
class SessionGrants {
 public:
  SessionGrants();
  SessionGrants(const SessionGrants&) = delete;
  SessionGrants& operator=(const SessionGrants&) = delete;
  ~SessionGrants();

  // The process-wide instance (used by the session manager and the CDP
  // decision).
  static SessionGrants& Get();

  // Issues (or replaces) the grant of `session_id`, bound to `owner`; returns
  // the token.
  std::string Issue(const std::string& session_id, const OwnerTuple& owner);
  // The token to put into the session's endpoint URLs, or "" (none).
  std::string GrantFor(std::string_view session_id) const;
  // Constant-time check that `presented` is the session's grant and that the
  // grant is bound to exactly `owner`. False for empty/unknown/mismatch.
  bool Verify(std::string_view session_id,
              std::string_view presented,
              const OwnerTuple& owner) const;
  void Revoke(std::string_view session_id);
  void RevokeAllForProfile(std::string_view profile_key);
  size_t size() const { return grants_.size(); }

 private:
  struct Entry {
    std::string token;
    OwnerTuple owner;
  };

  SEQUENCE_CHECKER(sequence_checker_);
  std::map<std::string, Entry, std::less<>> grants_;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_SCOPE_AUTH_H_

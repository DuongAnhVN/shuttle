// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_SESSION_CDP_FILTER_H_
#define CHROME_BROWSER_SHUTTLE_SESSION_CDP_FILTER_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/shuttle/browser_owner_index.h"
#include "chrome/browser/shuttle/scope_auth.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace shuttle {

class SessionManager;

// With isolated Profiles on (--shuttle-isolated-profiles) a filter also
// verifies the session's owner tuple (Profile, workspace agent/partition,
// lifecycle and browser epoch) on every message, against the browser-wide
// indexes, and applies the stricter command/event rules of PROFILES.md
// section 4. Without it the filter behaves exactly as before.
struct FilterScope {
  // The owner the connection was authorized for.
  OwnerTuple owner;
  // The CDP browserContextId of the owner's Profile: the only one a command
  // may name.
  std::string browser_context_id;
};

// Scopes one CDP connection to an agent session: the connection only sees and
// reaches the session's tabs (their frames and workers), new targets become
// tabs leased to the session, cookies are the agent's, and Browser.close ends
// the session instead of quitting. Used for /devtools/session/<id> (a
// browser-level connection) and for page connections to leased tabs (the
// Target domain of a page session is browser-wide).
class SessionCdpFilter
    : public content::DevToolsManagerDelegate::ConnectionFilter {
 public:
  SessionCdpFilter(base::WeakPtr<SessionManager> manager,
                   std::string session_id,
                   bool page_connection,
                   std::string page_target_id = std::string(),
                   std::optional<FilterScope> scope = std::nullopt);
  SessionCdpFilter(const SessionCdpFilter&) = delete;
  SessionCdpFilter& operator=(const SessionCdpFilter&) = delete;
  ~SessionCdpFilter() override;

  // content::DevToolsManagerDelegate::ConnectionFilter:
  void Start(SendCallback to_agent, SendCallback to_client) override;
  void SetCloseCallback(base::RepeatingClosure close) override;
  void OnClientMessage(std::string message) override;
  void OnAgentMessage(std::string message) override;

 private:
  // Whether `target_id` is (or belongs to) a tab leased to the session, or
  // was opened by one (`opener_id`; popups are leased right after creation).
  bool Owns(const std::string& target_id, const std::string& opener_id);
  // Scoped mode: the connection's session, owner tuple and grant are still
  // current. When not, the connection is closed (once) and false is returned.
  bool StillAuthorized();
  void EndConnection();
  void OnSessionEnded(const std::string& session_id);
  // Scoped mode: a target the index knows for another owner/session/partition
  // is not ours.
  bool IndexAllows(const std::string& target_id) const;
  // Scoped mode: records a target attached under an own parent (a worker, a
  // frame) in the target index, so a raw page connection to it is attributed.
  void IndexChildTarget(const std::string& child_session,
                        const std::string& target_id);
  void UnindexChildTarget(const std::string& child_session);
  // Scoped mode: the shared Browser/Storage/Target policy
  // (DecideScopedCdpCommand), identical for the session socket, page sockets
  // and every flattened child session.
  CdpCommandDecision ScopedDecision(std::string_view method,
                                    const base::DictValue* params,
                                    const std::string* session) const;

  void HandleTargetEvent(const std::string& method,
                         base::DictValue message,
                         std::string raw);
  void FilterTargetInfos(base::DictValue message);

  void CreateTarget(const base::Value& id,
                    const std::string* session,
                    const base::DictValue* params);
  void CloseSession(const base::Value& id, const std::string* session);
  // Page.bringToFront / Target.activateTarget: success without activating
  // the window or the app (SessionManager::SelectTab).
  void BringToFront(const base::Value& id,
                    const std::string* session,
                    const std::string& target_id);
  void GetCookies(const base::Value& id, const std::string* session);
  void SetCookies(const base::Value& id,
                  const std::string* session,
                  const base::DictValue* params);
  void ClearCookies(const base::Value& id, const std::string* session);

  void SendResult(const base::Value& id,
                  const std::string* session,
                  base::DictValue result);
  void SendError(const base::Value& id,
                 const std::string* session,
                 const std::string& message);
  // Sends a command of our own (its response is dropped).
  void SendInternal(const std::string& method,
                    base::DictValue params,
                    const std::string* session);

  base::WeakPtr<SessionManager> manager_;
  const std::string session_id_;
  // Page connections: the root session is the leased page itself, so targets
  // auto-attached to it (its frames and workers) are its own.
  const bool page_connection_;
  // Page connections: the leased page's target id.
  const std::string page_target_id_;
  // Set with isolated Profiles on.
  const std::optional<FilterScope> scope_;
  SendCallback to_agent_;
  SendCallback to_client_;
  base::RepeatingClosure close_;
  bool ended_ = false;
  int end_callback_id_ = 0;
  // Scoped mode: child session -> target id of the attached children this
  // connection registered in the target index.
  std::map<std::string, std::string> indexed_children_;

  // Target ids announced to the client.
  std::set<std::string> visible_;
  // Child sessions attached to own top-level targets: session -> target id.
  std::map<std::string, std::string> session_targets_;
  // All owned flattened children, including nested workers and frames.
  std::set<std::string> own_sessions_;
  // Child sessions auto-attached to foreign targets, being detached.
  std::set<std::string> foreign_sessions_;
  // Client command ids whose responses list targets (Target.getTargets).
  std::set<int> filter_responses_;
  // Ids of our own commands.
  std::set<int> internal_ids_;
  int next_internal_id_ = 1'000'000'000;
  // Tabs this connection created (Target.createTarget): closed when it ends.
  std::set<std::string> created_;
  // Set while Target.createTarget allocates a tab.
  bool allocating_ = false;

  base::WeakPtrFactory<SessionCdpFilter> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_SESSION_CDP_FILTER_H_

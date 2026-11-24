// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/scoped_routing.h"

#include <optional>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/types/expected.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/browser_owner_index.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"
#include "chrome/browser/shuttle/browser_profile_router.h"
#include "chrome/browser/shuttle/scope_auth.h"
#include "chrome/browser/shuttle/shuttle_service.h"
#include "content/public/browser/devtools_agent_host.h"

namespace shuttle {

namespace {

using Decision = content::DevToolsManagerDelegate::ConnectionDecision;
using Kind = content::DevToolsManagerDelegate::ConnectionKind;

// Drains a Profile through its ShuttleService (BrowserProfileRouter's stop).
class ProfileDrainDelegate : public BrowserProfileDrainDelegate {
 public:
  void DrainProfile(Profile* profile,
                    const std::string& core_profile_key,
                    int64_t lifecycle_epoch,
                    base::OnceCallback<void(bool success)> done) override {
    // The router already revoked the owner indexes; the grants go too.
    SessionGrants::Get().RevokeAllForProfile(core_profile_key);
    ShuttleService* service = ShuttleService::Get(profile);
    if (!service) {
      std::move(done).Run(true);  // Nothing was ever started in it.
      return;
    }
    service->Drain(std::move(done));
  }

  bool HasDescendantHandles(Profile* profile) override {
    ShuttleService* service = ShuttleService::Get(profile);
    return service && service->HasDescendantHandles();
  }
};

// The owner `located` resolves to right now, with its Profile. nullopt when
// the Profile is not ready (stopped, draining, failed, deleted, startup not
// captured).
struct CurrentScope {
  OwnerTuple owner;
  raw_ptr<Profile> profile = nullptr;
};

std::optional<CurrentScope> ResolveCurrent(BrowserProfileRouter& router,
                                           const OwnerTuple& located) {
  std::optional<std::string> key;
  if (!located.profile_key.empty()) {
    key = located.profile_key;
  }
  base::expected<ResolvedProfile, BrowserProfileFailure> resolved =
      router.Resolve(key);
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  CurrentScope current;
  current.owner = router.MakeOwner(*resolved, located.workspace_agent);
  // The partition is the session's own agent, as when the session was opened.
  current.owner.workspace_agent = located.workspace_agent;
  current.profile = resolved->profile;
  return current;
}

}  // namespace

void InstallScopedRouting() {
  BrowserProfileRouter* router = BrowserProfileRouter::Get();
  if (!router) {
    return;
  }
  static base::NoDestructor<ProfileDrainDelegate> drain_delegate;
  router->SetDrainDelegate(drain_delegate.get());
  // Only a router that can serve (manifest and admin credential usable) is
  // announced as routing-ready.
  router->SetScopedRoutingReady(router->IsAvailable());
}

Decision DecideScopedConnection(
    Kind kind,
    const std::string& session_id,
    const std::string& target_id,
    const content::DevToolsManagerDelegate::RequestContext& context) {
  BrowserProfileRouter* router = BrowserProfileRouter::Get();
  if (!router) {
    return Decision::Denied(403);
  }

  ConnectionAuthInput input;
  input.isolated_enabled = true;
  input.is_admin = context.admin;
  std::string owning_session;
  switch (kind) {
    case Kind::kBrowser:
      input.target = ConnectionTarget::kBrowser;
      break;
    case Kind::kSession:
      input.target = ConnectionTarget::kSession;
      input.located = router->session_owners().Lookup(session_id);
      owning_session = session_id;
      break;
    case Kind::kPage: {
      input.target = ConnectionTarget::kPage;
      input.target_exists =
          content::DevToolsAgentHost::GetForId(target_id) != nullptr;
      if (std::optional<TargetOwner> target =
              router->target_owners().Lookup(target_id)) {
        input.located = target->owner;
        owning_session = target->session_id;
      }
      break;
    }
  }

  std::optional<CurrentScope> current;
  if (input.located) {
    current = ResolveCurrent(*router, *input.located);
    if (current) {
      input.current = current->owner;
    }
    input.grant_valid = SessionGrants::Get().Verify(
        owning_session, context.session_grant, *input.located);
  }

  const ConnectionAuthResult result = DecideConnectionAuth(input);
  switch (result.verdict) {
    case ConnectionVerdict::kDenied:
      return Decision::Denied(result.http_status);
    case ConnectionVerdict::kAdminAllowed:
      return Decision::AdminAllowed();
    case ConnectionVerdict::kScopedFilter:
      break;
  }

  // The mandatory filter: from the service of the Profile the owner resolves
  // to (never a last-used or startup service chosen by the caller).
  ShuttleService* service =
      current && current->profile
          ? ShuttleService::Get(current->profile->GetOriginalProfile())
          : nullptr;
  if (!service || !input.located) {
    return Decision::Denied(404);
  }
  std::unique_ptr<content::DevToolsManagerDelegate::ConnectionFilter> filter =
      service->CreateScopedConnectionFilter(
          owning_session, kind == Kind::kPage ? target_id : std::string(),
          *input.located);
  if (!filter) {
    return Decision::Denied(404);
  }
  return Decision::Scoped(std::move(filter));
}

}  // namespace shuttle

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/scope_auth.h"

#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/test/gtest_util.h"
#include "base/values.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace shuttle {
namespace {

OwnerTuple Owner(const std::string& key,
                 const std::string& agent = "agent",
                 int64_t epoch = 1,
                 const std::string& browser = "b1") {
  OwnerTuple owner;
  owner.profile_key = key;
  owner.workspace_agent = agent;
  owner.lifecycle_epoch = epoch;
  owner.browser_epoch = browser;
  return owner;
}

base::DictValue ParseDict(const char* json) {
  std::optional<base::Value> value = base::JSONReader::Read(json);
  CHECK(value && value->is_dict());
  return std::move(value->GetDict());
}

const char kKeyA[] = "bp_0123456789abcdef0123456789abcdef";

// --- ParseProfileSelector ------------------------------------------------------

TEST(ScopeAuthSelectorTest, NullParamsIsAbsent) {
  ParsedSelector parsed = ParseProfileSelector(nullptr);
  EXPECT_EQ(parsed.state, SelectorState::kAbsent);
  EXPECT_FALSE(parsed.has_selector_key);
}

TEST(ScopeAuthSelectorTest, NoMetaOrNoKeyIsAbsent) {
  base::DictValue a = ParseDict(R"({"name":"x"})");
  EXPECT_EQ(ParseProfileSelector(&a).state, SelectorState::kAbsent);
  base::DictValue b = ParseDict(R"({"_meta":{"other":1}})");
  ParsedSelector parsed = ParseProfileSelector(&b);
  EXPECT_EQ(parsed.state, SelectorState::kAbsent);
  EXPECT_FALSE(parsed.has_selector_key);
}

TEST(ScopeAuthSelectorTest, WellFormedKeyIsPresent) {
  base::DictValue params = ParseDict(
      R"({"_meta":{"shuttle/profileId":"bp_0123456789abcdef0123456789abcdef"}})");
  ParsedSelector parsed = ParseProfileSelector(&params);
  EXPECT_EQ(parsed.state, SelectorState::kPresent);
  EXPECT_TRUE(parsed.has_selector_key);
  EXPECT_EQ(parsed.key, kKeyA);
}

TEST(ScopeAuthSelectorTest, MalformedValuesNeverFallBackToAbsent) {
  const char* cases[] = {
      R"({"_meta":{"shuttle/profileId":"bp_short"}})",
      R"({"_meta":{"shuttle/profileId":"BP_0123456789ABCDEF0123456789ABCDEF"}})",
      R"({"_meta":{"shuttle/profileId":""}})",
      R"({"_meta":{"shuttle/profileId":null}})",
      R"({"_meta":{"shuttle/profileId":7}})",
      R"({"_meta":{"shuttle/profileId":{"a":1}}})",
      R"({"_meta":"shuttle/profileId"})",
      R"({"_meta":null})",
      R"({"_meta":[]})",
  };
  for (const char* json : cases) {
    base::DictValue params = ParseDict(json);
    ParsedSelector parsed = ParseProfileSelector(&params);
    EXPECT_EQ(parsed.state, SelectorState::kMalformed) << json;
    EXPECT_TRUE(parsed.has_selector_key) << json;
    EXPECT_TRUE(parsed.key.empty()) << json;
  }
}

// --- DecideConnectionAuth ------------------------------------------------------------

ConnectionAuthInput Isolated(ConnectionTarget target) {
  ConnectionAuthInput input;
  input.isolated_enabled = true;
  input.target = target;
  return input;
}

TEST(ScopeAuthDecisionTest, LegacyKeepsPreIsolationSemantics) {
  ConnectionAuthInput input;
  input.target = ConnectionTarget::kBrowser;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kAdminAllowed);

  input.target = ConnectionTarget::kSession;
  ConnectionAuthResult result = DecideConnectionAuth(input);
  EXPECT_EQ(result.verdict, ConnectionVerdict::kDenied);
  EXPECT_EQ(result.http_status, 404);
  input.legacy_scoped_available = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kScopedFilter);

  input.target = ConnectionTarget::kPage;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kScopedFilter);
  input.legacy_scoped_available = false;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kAdminAllowed);
}

TEST(ScopeAuthDecisionTest, IsolatedBrowserSocketNeedsAdmin) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kBrowser);
  ConnectionAuthResult result = DecideConnectionAuth(input);
  EXPECT_EQ(result.verdict, ConnectionVerdict::kDenied);
  EXPECT_EQ(result.http_status, 403);
  // A valid session grant never opens the browser socket.
  input.grant_valid = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict, ConnectionVerdict::kDenied);
  input.is_admin = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kAdminAllowed);
}

TEST(ScopeAuthDecisionTest, IsolatedSessionUnknownIs404) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kSession);
  input.is_admin = true;
  input.grant_valid = true;
  ConnectionAuthResult result = DecideConnectionAuth(input);
  EXPECT_EQ(result.verdict, ConnectionVerdict::kDenied);
  EXPECT_EQ(result.http_status, 404);
}

TEST(ScopeAuthDecisionTest, IsolatedSessionStaleOwnerIs404) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kSession);
  input.grant_valid = true;
  input.located = Owner(kKeyA, "agent", 1);
  // Not ready / gone.
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);
  // Epoch bumped, other agent, other browser incarnation.
  input.current = Owner(kKeyA, "agent", 2);
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);
  input.current = Owner(kKeyA, "other", 1);
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);
  input.current = Owner(kKeyA, "agent", 1, "b2");
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);
}

TEST(ScopeAuthDecisionTest, IsolatedSessionNeedsGrantOrAdminAndIsAlwaysScoped) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kSession);
  input.located = Owner(kKeyA);
  input.current = Owner(kKeyA);
  ConnectionAuthResult result = DecideConnectionAuth(input);
  EXPECT_EQ(result.verdict, ConnectionVerdict::kDenied);
  EXPECT_EQ(result.http_status, 403);

  input.grant_valid = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kScopedFilter);

  input.grant_valid = false;
  input.is_admin = true;
  // Admin on a session socket still gets the scoped filter, never raw CDP.
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kScopedFilter);
}

TEST(ScopeAuthDecisionTest, IsolatedPageUnindexed) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kPage);
  input.grant_valid = true;  // A grant alone proves nothing for an unknown id.
  ConnectionAuthResult result = DecideConnectionAuth(input);
  EXPECT_EQ(result.verdict, ConnectionVerdict::kDenied);
  EXPECT_EQ(result.http_status, 403);
  input.target_exists = false;
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);
  input.is_admin = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kAdminAllowed);
}

TEST(ScopeAuthDecisionTest, IsolatedPageIndexed) {
  ConnectionAuthInput input = Isolated(ConnectionTarget::kPage);
  input.located = Owner(kKeyA);
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);  // not ready
  input.current = Owner(kKeyA, "agent", 9);
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 404);  // stale
  input.current = Owner(kKeyA);
  EXPECT_EQ(DecideConnectionAuth(input).http_status, 403);  // no credential
  input.grant_valid = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kScopedFilter);
  input.grant_valid = false;
  input.is_admin = true;
  EXPECT_EQ(DecideConnectionAuth(input).verdict,
            ConnectionVerdict::kAdminAllowed);
}

// --- DecideScopedCdpCommand -------------------------------------------------------------

constexpr char kOwnContext[] = "ABCDEF0123456789ABCDEF0123456789";

CdpCommandDecision Decide(std::string_view method,
                          CdpForm form,
                          const char* params_json = nullptr,
                          bool managed = true) {
  base::DictValue params;
  CdpCommandInput input;
  input.method = method;
  input.form = form;
  input.managed = managed;
  input.own_browser_context_id = kOwnContext;
  if (params_json) {
    params = ParseDict(params_json);
    input.params = &params;
  }
  return DecideScopedCdpCommand(input);
}

constexpr CdpForm kForms[] = {CdpForm::kBrowserRoot, CdpForm::kPageRoot,
                              CdpForm::kChildSession};

TEST(ScopeAuthCdpPolicyTest, NestedProtocolTunnellingIsDeniedOnEveryForm) {
  for (CdpForm form : kForms) {
    EXPECT_EQ(CdpCommandVerdict::kDeny,
              Decide("Target.sendMessageToTarget", form,
                     R"({"sessionId":"s","message":"{\"id\":1,\"method\":\"Browser.close\"}"})")
                  .verdict);
    EXPECT_EQ(CdpCommandVerdict::kDeny,
              Decide("Target.receivedMessageFromTarget", form).verdict);
    EXPECT_EQ(CdpCommandVerdict::kDeny,
              Decide("Target.attachToBrowserTarget", form).verdict);
  }
}

TEST(ScopeAuthCdpPolicyTest, OnlyFlattenedAttachmentsAreAllowed) {
  for (CdpForm form : kForms) {
    for (const char* method : {"Target.attachToTarget", "Target.setAutoAttach"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide(method, form, R"({"targetId":"T"})").verdict)
          << method;
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide(method, form, R"({"flatten":false})").verdict);
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide(method, form, R"({"flatten":"true"})").verdict);
      EXPECT_EQ(CdpCommandVerdict::kDeny, Decide(method, form).verdict);
      EXPECT_EQ(CdpCommandVerdict::kAllow,
                Decide(method, form, R"({"flatten":true})").verdict);
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, ContextCreationIsDeniedOnEveryForm) {
  for (CdpForm form : kForms) {
    EXPECT_EQ(CdpCommandVerdict::kDeny,
              Decide("Target.createBrowserContext", form).verdict);
    EXPECT_EQ(CdpCommandVerdict::kDeny,
              Decide("Target.disposeBrowserContext", form).verdict);
  }
}

TEST(ScopeAuthCdpPolicyTest, CookiesAreRoutedOnlyAtTheSessionRoot) {
  for (CdpForm form : kForms) {
    const auto expected = form == CdpForm::kBrowserRoot
                              ? CdpCommandVerdict::kRouteCookiesToAgent
                              : CdpCommandVerdict::kDeny;
    for (const char* method :
         {"Storage.getCookies", "Storage.setCookies", "Storage.clearCookies"}) {
      EXPECT_EQ(expected,
                Decide(method, form).verdict)
          << method;
      EXPECT_EQ(expected,
                Decide(method, form,
                       R"({"browserContextId":"ABCDEF0123456789ABCDEF0123456789"})")
                    .verdict);
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide(method, form, R"({"browserContextId":"OTHER"})").verdict);
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, OtherStorageCommandsAreDeniedOnEveryForm) {
  for (CdpForm form : kForms) {
    for (const char* method :
         {"Storage.clearDataForOrigin", "Storage.getUsageAndQuota",
          "Storage.getStorageKeyForFrame", "Storage.overrideQuotaForOrigin"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny, Decide(method, form).verdict) << method;
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, ForeignOrMalformedContextIsDeniedEverywhere) {
  for (CdpForm form : kForms) {
    for (const char* json :
         {R"({"browserContextId":"OTHER"})", R"({"browserContextId":""})",
          R"({"browserContextId":7})", R"({"browserContextId":null})"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide("Target.createTarget", form, json).verdict)
          << json;
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide("Browser.setPermission", form, json).verdict);
    }
    EXPECT_EQ(
        CdpCommandVerdict::kAllow,
        Decide("Target.createTarget", form,
               R"({"browserContextId":"ABCDEF0123456789ABCDEF0123456789"})")
            .verdict);
  }
}

TEST(ScopeAuthCdpPolicyTest, WindowAndProcessIntrospectionIsDenied) {
  for (CdpForm form : kForms) {
    for (const char* method :
         {"Browser.setWindowBounds", "Browser.getWindowBounds",
          "Browser.setContentsSize", "Browser.getBrowserCommandLine",
          "Browser.getHistograms", "Browser.getHistogram"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny, Decide(method, form).verdict) << method;
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, ManagedPermissionCommandsNeedTheOwnContext) {
  for (CdpForm form : kForms) {
    const auto expected = form == CdpForm::kBrowserRoot
                              ? CdpCommandVerdict::kAllow
                              : CdpCommandVerdict::kDeny;
    for (const char* method :
         {"Browser.setPermission", "Browser.grantPermissions",
          "Browser.resetPermissions", "Browser.setDownloadBehavior",
          "Browser.cancelDownload"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny, Decide(method, form).verdict) << method;
      EXPECT_EQ(expected,
                Decide(method, form,
                       R"({"browserContextId":"ABCDEF0123456789ABCDEF0123456789"})")
                    .verdict);
      // The startup Profile's owner is not managed: its default context is its own.
      EXPECT_EQ(expected,
                Decide(method, form, nullptr, /*managed=*/false).verdict);
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, EveryBrowserCommandIsDeniedOnPagesAndChildren) {
  for (CdpForm form : {CdpForm::kPageRoot, CdpForm::kChildSession}) {
    for (const char* method : {"Browser.close", "Browser.crash",
                               "Browser.setPermission", "Browser.getVersion"}) {
      EXPECT_EQ(CdpCommandVerdict::kDeny, Decide(method, form).verdict);
      EXPECT_EQ(CdpCommandVerdict::kDeny,
                Decide(method, form, nullptr, /*managed=*/false).verdict);
    }
  }
}

TEST(ScopeAuthCdpPolicyTest, OrdinaryCommandsPass) {
  for (CdpForm form : kForms) {
    EXPECT_EQ(CdpCommandVerdict::kAllow,
              Decide("Runtime.evaluate", form, R"({"expression":"1"})").verdict);
    EXPECT_EQ(CdpCommandVerdict::kAllow,
              Decide("Network.getAllCookies", form).verdict);
    EXPECT_EQ(CdpCommandVerdict::kAllow,
              Decide("Target.getTargets", form).verdict);
  }
}

// --- SessionGrants -------------------------------------------------------------------

TEST(ScopeAuthGrantTest, GeneratedGrantsAreUniqueAndUnpadded) {
  std::string a = GenerateSessionGrant();
  std::string b = GenerateSessionGrant();
  EXPECT_NE(a, b);
  EXPECT_EQ(a.find('='), std::string::npos);
  EXPECT_GE(a.size(), 43u);
}

TEST(ScopeAuthGrantTest, IssueVerifyRevoke) {
  SessionGrants grants;
  OwnerTuple owner = Owner(kKeyA);
  std::string token = grants.Issue("s1", owner);
  EXPECT_FALSE(token.empty());
  EXPECT_EQ(grants.GrantFor("s1"), token);
  EXPECT_TRUE(grants.Verify("s1", token, owner));

  EXPECT_FALSE(grants.Verify("s1", "", owner));
  EXPECT_FALSE(grants.Verify("s1", token + "x", owner));
  EXPECT_FALSE(grants.Verify("s1", token.substr(1), owner));
  EXPECT_FALSE(grants.Verify("s2", token, owner));

  grants.Revoke("s1");
  EXPECT_FALSE(grants.Verify("s1", token, owner));
  EXPECT_EQ(grants.GrantFor("s1"), "");
  EXPECT_EQ(grants.size(), 0u);
}

TEST(ScopeAuthGrantTest, BoundToExactOwnerTuple) {
  SessionGrants grants;
  OwnerTuple owner = Owner(kKeyA, "agent", 3, "b1");
  std::string token = grants.Issue("s1", owner);
  EXPECT_FALSE(grants.Verify("s1", token, Owner(kKeyA, "agent", 4, "b1")));
  EXPECT_FALSE(grants.Verify("s1", token, Owner(kKeyA, "other", 3, "b1")));
  EXPECT_FALSE(grants.Verify("s1", token, Owner(kKeyA, "agent", 3, "b2")));
  EXPECT_FALSE(grants.Verify(
      "s1", token, Owner("bp_ffffffffffffffffffffffffffffffff", "agent", 3)));
  EXPECT_TRUE(grants.Verify("s1", token, owner));
}

TEST(ScopeAuthGrantTest, GrantOfOneSessionNeverOpensAnother) {
  SessionGrants grants;
  OwnerTuple owner = Owner(kKeyA);
  std::string t1 = grants.Issue("s1", owner);
  std::string t2 = grants.Issue("s2", owner);
  EXPECT_NE(t1, t2);
  EXPECT_FALSE(grants.Verify("s1", t2, owner));
  EXPECT_FALSE(grants.Verify("s2", t1, owner));
}

TEST(ScopeAuthGrantTest, ReissueReplacesTheOldToken) {
  SessionGrants grants;
  OwnerTuple owner = Owner(kKeyA);
  std::string old_token = grants.Issue("s1", owner);
  std::string new_token = grants.Issue("s1", owner);
  EXPECT_FALSE(grants.Verify("s1", old_token, owner));
  EXPECT_TRUE(grants.Verify("s1", new_token, owner));
  EXPECT_EQ(grants.size(), 1u);
}

TEST(ScopeAuthGrantTest, RevokeAllForProfileOnlyTouchesThatProfile) {
  SessionGrants grants;
  const char kKeyB[] = "bp_ffffffffffffffffffffffffffffffff";
  OwnerTuple a = Owner(kKeyA);
  OwnerTuple b = Owner(kKeyB);
  std::string ta1 = grants.Issue("a1", a);
  std::string ta2 = grants.Issue("a2", a);
  std::string tb = grants.Issue("b1", b);

  grants.RevokeAllForProfile(kKeyA);
  EXPECT_FALSE(grants.Verify("a1", ta1, a));
  EXPECT_FALSE(grants.Verify("a2", ta2, a));
  EXPECT_TRUE(grants.Verify("b1", tb, b));
  EXPECT_EQ(grants.size(), 1u);
}

}  // namespace
}  // namespace shuttle

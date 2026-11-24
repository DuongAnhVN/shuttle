// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOT YET COMPILED nor run (no native build on the authoring machine). See
// tools/shuttle/docs/PROFILES.md, "Implementation status".

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/types/expected.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "chrome/browser/shuttle/browser_admin_auth.h"
#include "chrome/browser/shuttle/browser_owner_index.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace shuttle {

namespace {

using Result = base::expected<BrowserProfileEntry, BrowserProfileFailure>;
using State = BrowserProfileState;

constexpr char kKeyA[] = "bp_0123456789abcdef0123456789abcdef";
constexpr char kKeyB[] = "bp_fedcba9876543210fedcba9876543210";
constexpr char kRequestA[] = "6b2c1a3e-8f4d-4c1b-9a77-0e5d2f3a4b51";
constexpr char kRequestB[] = "0d9a7c52-3b1e-4f6a-8c20-7a4b5e6f1d90";
constexpr char kPersona[] = R"({"preset":"windows-chrome"})";

// In-memory manifest writer: records what would be on disk.
class FakeWriter : public BrowserProfileManifestWriter {
 public:
  void Write(std::string data,
             base::OnceCallback<void(bool)> done) override {
    ++write_count;
    if (succeed) {
      last_data = std::move(data);
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(done), succeed));
  }

  bool succeed = true;
  int write_count = 0;
  std::string last_data;
};

BrowserProfileRegistry::CreateRequest MakeRequest(const std::string& request_id,
                                                  const std::string& key,
                                                  const std::string& directory,
                                                  const std::string& agent) {
  BrowserProfileRegistry::CreateRequest request;
  request.request_id = request_id;
  request.key = key;
  request.directory = directory;
  request.workspace_agent = agent;
  request.persona_input_json = kPersona;
  request.request_fingerprint =
      ComputeCreateRequestFingerprint(agent, kPersona, "");
  return request;
}

class BrowserProfileRegistryTest : public testing::Test {
 protected:
  void MakeRegistry(BrowserProfileManifest manifest = BrowserProfileManifest()) {
    auto writer = std::make_unique<FakeWriter>();
    writer_ = writer.get();
    registry_ = std::make_unique<BrowserProfileRegistry>(std::move(manifest),
                                                         std::move(writer));
  }

  template <typename F>
  Result Run(F&& start) {
    base::RunLoop loop;
    std::optional<Result> result;
    start(base::BindLambdaForTesting([&](Result r) {
      result = std::move(r);
      loop.Quit();
    }));
    loop.Run();
    return std::move(*result);
  }

  Result Create(const BrowserProfileRegistry::CreateRequest& request) {
    return Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->Create(request, std::move(cb));
    });
  }

  Result Transition(const std::string& key, State to) {
    return Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->Transition(key, to, BrowserProfileError::kNone, std::move(cb));
    });
  }

  base::test::TaskEnvironment task_environment_;
  raw_ptr<FakeWriter> writer_ = nullptr;
  std::unique_ptr<BrowserProfileRegistry> registry_;
};

BrowserProfileEntry MakeEntry(const std::string& key,
                              const std::string& directory,
                              State state,
                              int epoch) {
  BrowserProfileEntry entry;
  entry.key = key;
  entry.directory = directory;
  entry.workspace_agent = "workspace";
  entry.lifecycle_epoch = epoch;
  entry.state = state;
  entry.persona_input_json = kPersona;
  return entry;
}

}  // namespace

// --- Pure validation ------------------------------------------------------------

TEST(BrowserProfileValidationTest, KeyRegex) {
  EXPECT_TRUE(IsValidBrowserProfileKey(kKeyA));
  EXPECT_FALSE(IsValidBrowserProfileKey(""));
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_"));
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_0123"));
  // Upper case hex, wrong prefix, too long, path-like and non-hex all fail.
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_0123456789ABCDEF0123456789ABCDEF"));
  EXPECT_FALSE(IsValidBrowserProfileKey("xp_0123456789abcdef0123456789abcdef"));
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_0123456789abcdef0123456789abcdef0"));
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_../../0123456789abcdef01234567"));
  EXPECT_FALSE(IsValidBrowserProfileKey("bp_0123456789abcdef0123456789abcdeg"));
  EXPECT_FALSE(IsValidBrowserProfileKey(" bp_0123456789abcdef0123456789abcdef"));
}

TEST(BrowserProfileValidationTest, GeneratedKeysAreValidAndDistinct) {
  std::string first = GenerateBrowserProfileKey();
  std::string second = GenerateBrowserProfileKey();
  EXPECT_TRUE(IsValidBrowserProfileKey(first));
  EXPECT_TRUE(IsValidBrowserProfileKey(second));
  EXPECT_NE(first, second);
}

TEST(BrowserProfileValidationTest, WorkspaceSlug) {
  EXPECT_TRUE(IsValidWorkspaceAgentSlug("workspace"));
  EXPECT_TRUE(IsValidWorkspaceAgentSlug("a1-b2-c3"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug(""));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("Workspace"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("-a"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("a-"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("a--b"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("a/b"));
  EXPECT_FALSE(IsValidWorkspaceAgentSlug("tmp-1234"));  // throwaway prefix
  EXPECT_FALSE(IsValidWorkspaceAgentSlug(std::string(65, 'a')));
  EXPECT_TRUE(IsValidWorkspaceAgentSlug(std::string(64, 'a')));
}

TEST(BrowserProfileValidationTest, DirectoryNamesRejectTraversal) {
  EXPECT_TRUE(IsSafeProfileDirectoryName("Profile 1"));
  EXPECT_TRUE(IsSafeProfileDirectoryName("Profile 12"));
  for (const char* bad :
       {"", ".", "..", "../Profile 1", "..\\Profile 1", "Profile 1/", "a/b",
        "a\\b", "C:", "C:\\Windows", "/etc", "Profile.1", "Profile 1 ", " Profile 1",
        "Default", "default", "System Profile", "Guest Profile", "CON", "nul",
        "com1", "LPT9", "Profile\n1", "Profile%2e1"}) {
    EXPECT_FALSE(IsSafeProfileDirectoryName(bad)) << bad;
  }
  EXPECT_FALSE(IsSafeProfileDirectoryName(std::string(65, 'a')));
  // A key is never a directory name derivation, but if one is given it still
  // has to be a safe name.
  EXPECT_TRUE(IsSafeProfileDirectoryName(kKeyA));
  std::string embedded_nul("Profile 1", 9);
  embedded_nul.push_back('\0');
  EXPECT_FALSE(IsSafeProfileDirectoryName(embedded_nul));
}

TEST(BrowserProfileStateMachineTest, EdgesAndEpochBumps) {
  const State all[] = {State::kStopped,  State::kLoading, State::kInitializing,
                       State::kReady,    State::kDraining, State::kFailed,
                       State::kDeleted};
  struct Edge {
    State from;
    State to;
  };
  const Edge valid[] = {
      {State::kStopped, State::kLoading},
      {State::kStopped, State::kDeleted},
      {State::kLoading, State::kInitializing},
      {State::kLoading, State::kFailed},
      {State::kInitializing, State::kReady},
      {State::kInitializing, State::kFailed},
      {State::kReady, State::kDraining},
      {State::kReady, State::kFailed},
      {State::kDraining, State::kStopped},
      {State::kDraining, State::kFailed},
      {State::kFailed, State::kLoading},
      {State::kFailed, State::kStopped},
  };
  size_t valid_count = 0;
  for (State from : all) {
    for (State to : all) {
      bool expected = false;
      for (const Edge& edge : valid) {
        expected |= edge.from == from && edge.to == to;
      }
      EXPECT_EQ(expected, IsValidBrowserProfileTransition(from, to))
          << BrowserProfileStateToString(from) << " -> "
          << BrowserProfileStateToString(to);
      valid_count += expected;
    }
  }
  EXPECT_EQ(std::size(valid), valid_count);
  // Epoch: load, drain and deletion only.
  EXPECT_TRUE(TransitionBumpsLifecycleEpoch(State::kStopped, State::kLoading));
  EXPECT_TRUE(TransitionBumpsLifecycleEpoch(State::kFailed, State::kLoading));
  EXPECT_TRUE(TransitionBumpsLifecycleEpoch(State::kReady, State::kDraining));
  EXPECT_TRUE(TransitionBumpsLifecycleEpoch(State::kStopped, State::kDeleted));
  EXPECT_FALSE(TransitionBumpsLifecycleEpoch(State::kLoading, State::kInitializing));
  EXPECT_FALSE(TransitionBumpsLifecycleEpoch(State::kInitializing, State::kReady));
  EXPECT_FALSE(TransitionBumpsLifecycleEpoch(State::kDraining, State::kStopped));
  EXPECT_FALSE(TransitionBumpsLifecycleEpoch(State::kFailed, State::kStopped));
  // Nothing leaves `deleted`.
  for (State to : all) {
    EXPECT_FALSE(IsValidBrowserProfileTransition(State::kDeleted, to));
  }
}

TEST(BrowserProfileStateMachineTest, StringsRoundTrip) {
  for (std::string_view name : {"stopped", "loading", "initializing", "ready",
                                "draining", "failed", "deleted"}) {
    std::optional<State> state = BrowserProfileStateFromString(name);
    ASSERT_TRUE(state) << name;
    EXPECT_EQ(name, BrowserProfileStateToString(*state));
  }
  EXPECT_FALSE(BrowserProfileStateFromString("Ready"));
  EXPECT_FALSE(BrowserProfileStateFromString(""));
}

TEST(BrowserProfileErrorTest, CodesMatchContract) {
  for (std::string_view code :
       {"forbidden", "unsupported", "invalid_profile_key", "profile_not_found",
        "profile_not_ready", "profile_busy", "profile_failed",
        "persona_invalid", "proxy_not_ready", "storage_error", "drain_timeout",
        "request_conflict"}) {
    std::optional<BrowserProfileError> error = BrowserProfileErrorFromCode(code);
    ASSERT_TRUE(error) << code;
    EXPECT_EQ(code, BrowserProfileErrorCode(*error));
    BrowserProfileFailure failure(*error);
    EXPECT_FALSE(failure.message.empty());
  }
  EXPECT_FALSE(BrowserProfileErrorFromCode("nope"));
}

// --- Manifest -----------------------------------------------------------------------

TEST(BrowserProfileManifestTest, RoundTrip) {
  BrowserProfileManifest manifest;
  manifest.startup_profile_dir = "Default";
  BrowserProfileEntry entry = MakeEntry(kKeyA, "Profile 1", State::kStopped, 3);
  entry.request_id = kRequestA;
  entry.request_fingerprint = "abc";
  entry.provisioned = true;
  entry.persona_summary_json = R"({"preset":"windows-chrome"})";
  entry.proxy_pref_json = R"({"host":"h","port":1,"scheme":"http"})";
  entry.last_error = BrowserProfileError::kProxyNotReady;
  manifest.entries.push_back(entry);
  manifest.entries.push_back(MakeEntry(kKeyB, "Profile 2", State::kDeleted, 2));
  manifest.entries.back().deletion_pending = true;

  std::string json = SerializeBrowserProfileManifest(manifest);
  BrowserProfileParseResult parsed = ParseBrowserProfileManifest(json);
  ASSERT_EQ(BrowserProfileParseError::kNone, parsed.error);
  EXPECT_EQ("Default", parsed.manifest.startup_profile_dir);
  ASSERT_EQ(2u, parsed.manifest.entries.size());
  const BrowserProfileEntry& a = parsed.manifest.entries[0];
  EXPECT_EQ(kKeyA, a.key);
  EXPECT_EQ("Profile 1", a.directory);
  EXPECT_EQ(3, a.lifecycle_epoch);
  EXPECT_EQ(kRequestA, a.request_id);
  EXPECT_TRUE(a.provisioned);
  EXPECT_EQ(BrowserProfileError::kProxyNotReady, a.last_error);
  EXPECT_FALSE(a.proxy_pref_json.empty());
  const BrowserProfileEntry& b = parsed.manifest.entries[1];
  EXPECT_EQ(State::kDeleted, b.state);
  EXPECT_TRUE(b.deletion_pending);
  // No endpoint, path or secret field exists in the format.
  EXPECT_EQ(std::string::npos, json.find("endpoint"));
  EXPECT_EQ(std::string::npos, json.find("seed"));
  EXPECT_EQ(std::string::npos, json.find("token"));
}

TEST(BrowserProfileManifestTest, RejectsInvalidManifests) {
  auto valid_entry = [](const std::string& key, const std::string& dir) {
    return R"({"key":")" + key + R"(","directory":")" + dir +
           R"(","workspaceAgent":"workspace","lifecycleEpoch":1,"state":"stopped"})";
  };
  auto manifest_of = [](const std::string& entries) {
    return R"({"version":1,"profiles":[)" + entries + "]}";
  };
  // Baseline is accepted.
  EXPECT_EQ(BrowserProfileParseError::kNone,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry(kKeyA, "Profile 1"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest("not json").error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest("[]").error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(R"({"profiles":[]})").error);
  // A manifest of another version is never interpreted.
  EXPECT_EQ(BrowserProfileParseError::kUnknownVersion,
            ParseBrowserProfileManifest(R"({"version":2,"profiles":[]})").error);
  // Bad key, traversal directory, duplicate key/directory (case-insensitive),
  // unknown state, epoch below 1, bad slug: the whole manifest is refused.
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry("bp_nothex", "Profile 1"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry(kKeyA, "../Profile 1"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry(kKeyA, "C:\\\\x"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry(kKeyA, "Profile 1") + "," +
                            valid_entry(kKeyA, "Profile 2"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                manifest_of(valid_entry(kKeyA, "Profile 1") + "," +
                            valid_entry(kKeyB, "profile 1"))).error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                R"({"version":1,"profiles":[{"key":")" + std::string(kKeyA) +
                R"(","directory":"Profile 1","workspaceAgent":"workspace","lifecycleEpoch":1,"state":"running"}]})").error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                R"({"version":1,"profiles":[{"key":")" + std::string(kKeyA) +
                R"(","directory":"Profile 1","workspaceAgent":"workspace","lifecycleEpoch":0,"state":"stopped"}]})").error);
  EXPECT_EQ(BrowserProfileParseError::kMalformed,
            ParseBrowserProfileManifest(
                R"({"version":1,"profiles":[{"key":")" + std::string(kKeyA) +
                R"(","directory":"Profile 1","workspaceAgent":"Bad Slug","lifecycleEpoch":1,"state":"stopped"}]})").error);
}

// --- Registry ---------------------------------------------------------------------------

TEST_F(BrowserProfileRegistryTest, CreateIsIdempotentByRequestId) {
  MakeRegistry();
  Result first = Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(kKeyA, first->key);
  EXPECT_EQ(State::kStopped, first->state);
  EXPECT_EQ(1, first->lifecycle_epoch);
  EXPECT_EQ(1, writer_->write_count);

  // The same requestId and arguments: the same entry, nothing written, the
  // epoch is not touched, and a different key proposal is irrelevant.
  Result replay = Create(MakeRequest(kRequestA, kKeyB, "Profile 2", "workspace"));
  ASSERT_TRUE(replay.has_value());
  EXPECT_EQ(kKeyA, replay->key);
  EXPECT_EQ(1, replay->lifecycle_epoch);
  EXPECT_EQ(1, writer_->write_count);
  EXPECT_EQ(1u, registry_->List().size());

  // The same requestId with other arguments diverges.
  Result conflict =
      Create(MakeRequest(kRequestA, kKeyB, "Profile 2", "other-workspace"));
  ASSERT_FALSE(conflict.has_value());
  EXPECT_EQ(BrowserProfileError::kRequestConflict, conflict.error().code);
  EXPECT_EQ(1u, registry_->List().size());

  // Another requestId makes another entry.
  Result second = Create(MakeRequest(kRequestB, kKeyB, "Profile 2", "workspace"));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(kKeyB, second->key);
  EXPECT_EQ(2u, registry_->List().size());
}

TEST_F(BrowserProfileRegistryTest, CreateRefusesUnsafeOrDuplicateInput) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  int writes = writer_->write_count;
  for (const BrowserProfileRegistry::CreateRequest& request : {
           MakeRequest(kRequestB, "bp_bad", "Profile 2", "workspace"),
           MakeRequest(kRequestB, kKeyB, "../Profile 2", "workspace"),
           MakeRequest(kRequestB, kKeyB, "Profile 2", "Bad Slug"),
           MakeRequest(kRequestB, kKeyA, "Profile 2", "workspace"),   // dup key
           MakeRequest(kRequestB, kKeyB, "profile 1", "workspace"),   // dup dir
           MakeRequest("not-a-uuid", kKeyB, "Profile 2", "workspace"),
       }) {
    Result result = Create(request);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(BrowserProfileError::kProfileFailed, result.error().code);
  }
  EXPECT_EQ(writes, writer_->write_count);
  EXPECT_EQ(1u, registry_->List().size());
}

TEST_F(BrowserProfileRegistryTest, LifecycleEpochIsPersistedBeforeEachGeneration) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());

  Result loading = Transition(kKeyA, State::kLoading);
  ASSERT_TRUE(loading.has_value());
  EXPECT_EQ(2, loading->lifecycle_epoch);
  // The durable manifest already carries the incremented epoch when the
  // callback runs.
  BrowserProfileParseResult durable = ParseBrowserProfileManifest(writer_->last_data);
  ASSERT_EQ(BrowserProfileParseError::kNone, durable.error);
  EXPECT_EQ(2, durable.manifest.entries[0].lifecycle_epoch);
  EXPECT_EQ(State::kLoading, durable.manifest.entries[0].state);

  EXPECT_EQ(2, Transition(kKeyA, State::kInitializing)->lifecycle_epoch);
  EXPECT_EQ(2, Transition(kKeyA, State::kReady)->lifecycle_epoch);
  EXPECT_EQ(3, Transition(kKeyA, State::kDraining)->lifecycle_epoch);
  EXPECT_EQ(3, Transition(kKeyA, State::kStopped)->lifecycle_epoch);
  Result deleted = Transition(kKeyA, State::kDeleted);
  ASSERT_TRUE(deleted.has_value());
  EXPECT_EQ(4, deleted->lifecycle_epoch);
  EXPECT_TRUE(deleted->deletion_pending);
  EXPECT_EQ(State::kDeleted, deleted->state);

  // A tombstone never comes back.
  Result revive = Transition(kKeyA, State::kLoading);
  ASSERT_FALSE(revive.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileBusy, revive.error().code);
  EXPECT_EQ(4, registry_->Find(kKeyA)->lifecycle_epoch);
}

TEST_F(BrowserProfileRegistryTest, InvalidTransitionChangesNothing) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  int writes = writer_->write_count;
  // stopped -> ready skips loading; stopped -> draining is not an edge.
  for (State to : {State::kReady, State::kDraining, State::kInitializing,
                   State::kStopped}) {
    Result result = Transition(kKeyA, to);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(BrowserProfileError::kProfileBusy, result.error().code);
  }
  EXPECT_EQ(writes, writer_->write_count);
  EXPECT_EQ(State::kStopped, registry_->Find(kKeyA)->state);
  EXPECT_EQ(1, registry_->Find(kKeyA)->lifecycle_epoch);

  Result unknown = Transition(kKeyB, State::kLoading);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotFound, unknown.error().code);
}

TEST_F(BrowserProfileRegistryTest, ConcurrentLoadsSeeTheFirstTransition) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  // Two loads in the same turn: the first applies stopped -> loading in memory
  // at once, the second finds `loading` and is told it raced (the router joins
  // it to the running load instead of starting another).
  std::optional<Result> first;
  std::optional<Result> second;
  registry_->Transition(kKeyA, State::kLoading, BrowserProfileError::kNone,
                        base::BindLambdaForTesting([&](Result r) { first = std::move(r); }));
  EXPECT_EQ(State::kLoading, registry_->Find(kKeyA)->state);
  registry_->Transition(kKeyA, State::kLoading, BrowserProfileError::kNone,
                        base::BindLambdaForTesting([&](Result r) { second = std::move(r); }));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(first && first->has_value());
  ASSERT_TRUE(second && !second->has_value());
  EXPECT_EQ(BrowserProfileError::kProfileBusy, second->error().code);
  EXPECT_EQ(2, registry_->Find(kKeyA)->lifecycle_epoch);
}

TEST_F(BrowserProfileRegistryTest, FailedWriteRollsBackEveryPendingChange) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  writer_->succeed = false;
  Result result = Transition(kKeyA, State::kLoading);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(BrowserProfileError::kStorageError, result.error().code);
  // Memory never runs ahead of disk: still stopped at epoch 1.
  EXPECT_EQ(State::kStopped, registry_->Find(kKeyA)->state);
  EXPECT_EQ(1, registry_->Find(kKeyA)->lifecycle_epoch);

  // A failed create leaves no entry.
  Result create = Create(MakeRequest(kRequestB, kKeyB, "Profile 2", "workspace"));
  ASSERT_FALSE(create.has_value());
  EXPECT_EQ(BrowserProfileError::kStorageError, create.error().code);
  EXPECT_EQ(nullptr, registry_->Find(kKeyB));

  // The disk recovers: the same transition now works from the same state.
  writer_->succeed = true;
  Result retry = Transition(kKeyA, State::kLoading);
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(2, retry->lifecycle_epoch);
}

TEST_F(BrowserProfileRegistryTest, RestartRecoversEntriesAsStoppedKeepingEpochs) {
  BrowserProfileManifest manifest;
  manifest.startup_profile_dir = "Default";
  manifest.entries.push_back(MakeEntry(kKeyA, "Profile 1", State::kReady, 5));
  manifest.entries.back().loaded = true;
  manifest.entries.push_back(MakeEntry(kKeyB, "Profile 2", State::kDeleted, 2));
  manifest.entries.back().deletion_pending = true;
  BrowserProfileEntry draining = MakeEntry(
      "bp_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Profile 3", State::kDraining, 7);
  manifest.entries.push_back(draining);
  MakeRegistry(std::move(manifest));

  const BrowserProfileEntry* a = registry_->Find(kKeyA);
  ASSERT_TRUE(a);
  EXPECT_EQ(State::kStopped, a->state);
  EXPECT_FALSE(a->loaded);
  EXPECT_EQ(5, a->lifecycle_epoch);
  EXPECT_EQ(State::kStopped,
            registry_->Find("bp_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")->state);
  // Tombstones stay tombstones (and still hold their key and directory).
  const BrowserProfileEntry* b = registry_->Find(kKeyB);
  ASSERT_TRUE(b);
  EXPECT_EQ(State::kDeleted, b->state);
  EXPECT_TRUE(b->deletion_pending);
  EXPECT_EQ(registry_->Find(kKeyB), registry_->FindByDirectory("PROFILE 2"));
  EXPECT_EQ("Default", registry_->startup_profile_dir());

  // The next load increments from the persisted epoch.
  EXPECT_EQ(6, Transition(kKeyA, State::kLoading)->lifecycle_epoch);
}

TEST_F(BrowserProfileRegistryTest, MarkProvisionedAndConfirmDeletion) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  Result provisioned = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->MarkProvisioned(kKeyA, 1, kPersona,
                              R"({"preset":"windows-chrome"})", std::move(cb));
  });
  ASSERT_TRUE(provisioned.has_value());
  EXPECT_TRUE(provisioned->provisioned);

  // Only a tombstone can be confirmed.
  Result early = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ConfirmDeletion(kKeyA, std::move(cb));
  });
  EXPECT_FALSE(early.has_value());
  ASSERT_TRUE(Transition(kKeyA, State::kDeleted).has_value());
  Result confirmed = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ConfirmDeletion(kKeyA, std::move(cb));
  });
  ASSERT_TRUE(confirmed.has_value());
  EXPECT_FALSE(confirmed->deletion_pending);
  // The key and directory stay reserved forever.
  EXPECT_TRUE(registry_->Find(kKeyA));
  Result reuse = Create(MakeRequest(kRequestB, kKeyB, "Profile 1", "workspace"));
  EXPECT_FALSE(reuse.has_value());
}

// --- Persona replacement (contract section 7) -------------------------------------------------

TEST_F(BrowserProfileRegistryTest, StaleProvisioningCannotClearANewerPersonaIntent) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  ASSERT_TRUE(Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ReplacePersonaIntent(kKeyA, R"({"preset":"linux-chrome"})",
                                    "{}", std::move(cb));
  }).has_value());
  const BrowserProfileEntry current = *registry_->Find(kKeyA);
  const int writes = writer_->write_count;
  for (bool stale_epoch : {true, false}) {
    Result stale = Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->MarkProvisioned(
          kKeyA, stale_epoch ? 1 : current.lifecycle_epoch,
          stale_epoch ? current.persona_input_json : kPersona, "{}",
          std::move(cb));
    });
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(BrowserProfileError::kProfileBusy, stale.error().code);
    EXPECT_TRUE(registry_->Find(kKeyA)->persona_replacement_pending);
    EXPECT_FALSE(registry_->Find(kKeyA)->provisioned);
    EXPECT_EQ(writes, writer_->write_count);
  }
}

constexpr char kPersonaNew[] =
    R"({"preset":"linux-chrome","locale":"fr-FR","timezone":"Asia/Tokyo"})";
constexpr char kSummaryNew[] = R"({"preset":"linux-chrome","locale":"fr-FR"})";

TEST(BrowserProfilePersonaReplacementTest, OnlyStoppedIsAccepted) {
  EXPECT_EQ(BrowserProfileError::kNone,
            PersonaReplacementStateError(State::kStopped));
  for (State busy :
       {State::kReady, State::kLoading, State::kInitializing, State::kDraining}) {
    EXPECT_EQ(BrowserProfileError::kProfileBusy,
              PersonaReplacementStateError(busy));
  }
  EXPECT_EQ(BrowserProfileError::kProfileNotReady,
            PersonaReplacementStateError(State::kFailed));
  EXPECT_EQ(BrowserProfileError::kProfileNotFound,
            PersonaReplacementStateError(State::kDeleted));
}

TEST_F(BrowserProfileRegistryTest, ReplacePersonaOnStoppedPersistsIntentAndEpochTogether) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  const int writes = writer_->write_count;
  Result replaced = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ReplacePersonaIntent(kKeyA, kPersonaNew, kSummaryNew, std::move(cb));
  });
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(State::kStopped, replaced->state);
  EXPECT_EQ(2, replaced->lifecycle_epoch);
  EXPECT_TRUE(replaced->persona_replacement_pending);
  EXPECT_EQ(kPersonaNew, replaced->persona_input_json);
  EXPECT_EQ(kSummaryNew, replaced->persona_summary_json);
  // One atomic write carries the intent and the epoch; it is what is on disk.
  EXPECT_EQ(writes + 1, writer_->write_count);
  BrowserProfileParseResult parsed = ParseBrowserProfileManifest(writer_->last_data);
  ASSERT_EQ(BrowserProfileParseError::kNone, parsed.error);
  ASSERT_EQ(1u, parsed.manifest.entries.size());
  EXPECT_EQ(2, parsed.manifest.entries[0].lifecycle_epoch);
  EXPECT_TRUE(parsed.manifest.entries[0].persona_replacement_pending);
  EXPECT_EQ(kPersonaNew, parsed.manifest.entries[0].persona_input_json);

  // The next load still bumps the epoch from the replaced one.
  EXPECT_EQ(3, Transition(kKeyA, State::kLoading)->lifecycle_epoch);
}

TEST_F(BrowserProfileRegistryTest, ReplacePersonaTwiceBumpsTheEpochEachTime) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  for (int expected : {2, 3}) {
    Result result = Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->ReplacePersonaIntent(kKeyA, kPersonaNew, kSummaryNew, std::move(cb));
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(expected, result->lifecycle_epoch);
  }
}

TEST_F(BrowserProfileRegistryTest, ReplacePersonaRefusesEveryOtherStateWithoutChange) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  auto replace = [&]() {
    return Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->ReplacePersonaIntent(kKeyA, kPersonaNew, kSummaryNew, std::move(cb));
    });
  };
  auto expect_refused = [&](BrowserProfileError code) {
    const BrowserProfileEntry before = *registry_->Find(kKeyA);
    const int writes = writer_->write_count;
    Result result = replace();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(code, result.error().code);
    const BrowserProfileEntry* after = registry_->Find(kKeyA);
    EXPECT_EQ(before.lifecycle_epoch, after->lifecycle_epoch);
    EXPECT_EQ(before.persona_input_json, after->persona_input_json);
    EXPECT_FALSE(after->persona_replacement_pending);
    EXPECT_EQ(writes, writer_->write_count);  // nothing written
  };

  ASSERT_TRUE(Transition(kKeyA, State::kLoading).has_value());
  expect_refused(BrowserProfileError::kProfileBusy);
  ASSERT_TRUE(Transition(kKeyA, State::kInitializing).has_value());
  expect_refused(BrowserProfileError::kProfileBusy);
  ASSERT_TRUE(Transition(kKeyA, State::kReady).has_value());
  expect_refused(BrowserProfileError::kProfileBusy);
  ASSERT_TRUE(Transition(kKeyA, State::kDraining).has_value());
  expect_refused(BrowserProfileError::kProfileBusy);
  ASSERT_TRUE(Transition(kKeyA, State::kFailed).has_value());
  expect_refused(BrowserProfileError::kProfileNotReady);
  ASSERT_TRUE(Transition(kKeyA, State::kStopped).has_value());
  ASSERT_TRUE(Transition(kKeyA, State::kDeleted).has_value());
  expect_refused(BrowserProfileError::kProfileNotFound);

  Result unknown = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ReplacePersonaIntent(kKeyB, kPersonaNew, kSummaryNew, std::move(cb));
  });
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotFound, unknown.error().code);
}

TEST_F(BrowserProfileRegistryTest, ReplacePersonaRefusesAnEmptyIntent) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  Result result = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ReplacePersonaIntent(kKeyA, "", "", std::move(cb));
  });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(BrowserProfileError::kPersonaInvalid, result.error().code);
  EXPECT_EQ(1, registry_->Find(kKeyA)->lifecycle_epoch);
  EXPECT_EQ(kPersona, registry_->Find(kKeyA)->persona_input_json);
}

TEST_F(BrowserProfileRegistryTest, FailedWriteRollsBackThePersonaReplacement) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  writer_->succeed = false;
  Result result = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->ReplacePersonaIntent(kKeyA, kPersonaNew, kSummaryNew, std::move(cb));
  });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(BrowserProfileError::kStorageError, result.error().code);
  // Intent, flag and epoch all went back together.
  const BrowserProfileEntry* entry = registry_->Find(kKeyA);
  EXPECT_EQ(1, entry->lifecycle_epoch);
  EXPECT_FALSE(entry->persona_replacement_pending);
  EXPECT_EQ(kPersona, entry->persona_input_json);
  EXPECT_TRUE(entry->persona_summary_json.empty());
}

TEST_F(BrowserProfileRegistryTest, PendingReplacementSurvivesRestartAndIsClearedByTheCommit) {
  BrowserProfileManifest manifest;
  BrowserProfileEntry entry = MakeEntry(kKeyA, "Profile 1", State::kStopped, 4);
  entry.provisioned = true;
  entry.persona_replacement_pending = true;
  entry.persona_input_json = kPersonaNew;
  manifest.entries.push_back(entry);
  // The manifest round trip keeps the flag.
  BrowserProfileParseResult parsed =
      ParseBrowserProfileManifest(SerializeBrowserProfileManifest(manifest));
  ASSERT_EQ(BrowserProfileParseError::kNone, parsed.error);
  ASSERT_EQ(1u, parsed.manifest.entries.size());
  EXPECT_TRUE(parsed.manifest.entries[0].persona_replacement_pending);

  MakeRegistry(std::move(parsed.manifest));
  EXPECT_TRUE(registry_->Find(kKeyA)->persona_replacement_pending);
  EXPECT_EQ(4, registry_->Find(kKeyA)->lifecycle_epoch);
  // The commit (bootstrap / router) clears it and refreshes the summary.
  Result committed = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->MarkProvisioned(kKeyA, 4, kPersonaNew, kSummaryNew, std::move(cb));
  });
  ASSERT_TRUE(committed.has_value());
  EXPECT_FALSE(committed->persona_replacement_pending);
  EXPECT_EQ(kSummaryNew, committed->persona_summary_json);
  EXPECT_EQ(4, committed->lifecycle_epoch);  // committing never bumps
}

TEST_F(BrowserProfileRegistryTest, MarkPersonaCommittedOnlyClearsTheGenerationThatWasApplied) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  auto replace = [&](const char* persona) {
    return Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->ReplacePersonaIntent(kKeyA, persona, kSummaryNew, std::move(cb));
    });
  };
  auto commit = [&](int64_t epoch, const char* intent) {
    return Run([&](BrowserProfileRegistry::CommitCallback cb) {
      registry_->MarkPersonaCommitted(kKeyA, epoch, intent, kSummaryNew,
                                      std::move(cb));
    });
  };
  Result first = replace(kPersonaNew);
  ASSERT_TRUE(first.has_value());
  const int64_t epoch = first->lifecycle_epoch;

  // A stale epoch, a different intent: nothing changes.
  Result stale = commit(epoch - 1, kPersonaNew);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileBusy, stale.error().code);
  Result other = commit(epoch, R"({"preset":"windows-chrome"})");
  ASSERT_FALSE(other.has_value());
  EXPECT_TRUE(registry_->Find(kKeyA)->persona_replacement_pending);

  // A second replacement supersedes the first generation: the first's commit
  // can no longer clear the flag.
  Result second = replace(R"({"preset":"macos-chrome"})");
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(commit(epoch, kPersonaNew).has_value());
  EXPECT_TRUE(registry_->Find(kKeyA)->persona_replacement_pending);

  // The matching generation clears it, without touching state or epoch.
  Result ok = commit(second->lifecycle_epoch, R"({"preset":"macos-chrome"})");
  ASSERT_TRUE(ok.has_value());
  EXPECT_FALSE(ok->persona_replacement_pending);
  EXPECT_EQ(second->lifecycle_epoch, ok->lifecycle_epoch);
  EXPECT_EQ(State::kStopped, ok->state);
  // Nothing is pending any more: a repeated commit is refused.
  EXPECT_FALSE(commit(second->lifecycle_epoch, R"({"preset":"macos-chrome"})")
                   .has_value());
}

TEST_F(BrowserProfileRegistryTest, LostRestoreGenerationCompensatesCommittedIntent) {
  MakeRegistry();
  ASSERT_TRUE(Create(MakeRequest(kRequestA, kKeyA, "Profile 1", "workspace"))
                  .has_value());
  auto replacement = Run([&](auto cb) {
    registry_->ReplacePersonaIntent(kKeyA, kPersonaNew, kSummaryNew, std::move(cb));
  });
  ASSERT_TRUE(replacement.has_value());
  const auto epoch = replacement->lifecycle_epoch;
  ASSERT_TRUE(Run([&](auto cb) {
    registry_->MarkPersonaCommitted(kKeyA, epoch, kPersonaNew, kSummaryNew,
                                    std::move(cb));
  }).has_value());
  // Models generation loss while the asynchronous commit was in flight. The
  // compensating write keeps the same epoch/intent, not a new replacement.
  auto compensated = Run([&](auto cb) {
    registry_->ReinstatePersonaPending(kKeyA, epoch, kPersonaNew, std::move(cb));
  });
  ASSERT_TRUE(compensated.has_value());
  EXPECT_TRUE(compensated->persona_replacement_pending);
  EXPECT_EQ(epoch, compensated->lifecycle_epoch);
  EXPECT_EQ(kPersonaNew, compensated->persona_input_json);
  EXPECT_FALSE(Run([&](auto cb) {
    registry_->ReinstatePersonaPending(kKeyA, epoch - 1, kPersonaNew,
                                      std::move(cb));
  }).has_value());
}

TEST_F(BrowserProfileRegistryTest, MarkPersonaCommittedOfAnUnknownKeyIsNotFound) {
  MakeRegistry();
  Result result = Run([&](BrowserProfileRegistry::CommitCallback cb) {
    registry_->MarkPersonaCommitted(kKeyB, 1, kPersonaNew, kSummaryNew,
                                    std::move(cb));
  });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotFound, result.error().code);
}

// --- Owner indexes ----------------------------------------------------------------------------

TEST(BrowserOwnerIndexTest, SessionIdsAreUniqueAndOwnerChecked) {
  SessionOwnerIndex index;
  OwnerTuple a{kKeyA, "workspace", 2, "boot-1"};
  OwnerTuple b{kKeyB, "workspace", 2, "boot-1"};  // same slug, another Profile
  std::string id_a = index.RegisterNew(a);
  std::string id_b = index.RegisterNew(b);
  EXPECT_NE(id_a, id_b);
  EXPECT_TRUE(base::Uuid::ParseLowercase(id_a).is_valid());

  EXPECT_EQ(OwnerCheck::kOk, index.Check(id_a, a));
  EXPECT_EQ(OwnerCheck::kForbidden, index.Check(id_a, b));
  EXPECT_EQ(OwnerCheck::kUnknown, index.Check("00000000-0000-4000-8000-000000000000", a));
  OwnerTuple stale = a;
  stale.lifecycle_epoch = 1;
  EXPECT_EQ(OwnerCheck::kForbidden, index.Check(id_a, stale));
  OwnerTuple other_boot = a;
  other_boot.browser_epoch = "boot-2";
  EXPECT_EQ(OwnerCheck::kForbidden, index.Check(id_a, other_boot));
  OwnerTuple other_agent = a;
  other_agent.workspace_agent = "other";
  EXPECT_EQ(OwnerCheck::kForbidden, index.Check(id_a, other_agent));

  EXPECT_FALSE(index.Register(id_a, b));  // collision refused
}

TEST(BrowserOwnerIndexTest, RevokingAProfileRetiresItsIds) {
  SessionOwnerIndex index;
  OwnerTuple a{kKeyA, "workspace", 2, "boot-1"};
  OwnerTuple b{kKeyB, "workspace", 2, "boot-1"};
  std::string id_a = index.RegisterNew(a);
  std::string id_b = index.RegisterNew(b);
  EXPECT_EQ(std::vector<std::string>{id_a}, index.SessionsForProfile(kKeyA));
  EXPECT_EQ(std::vector<std::string>{id_a}, index.RemoveAllForProfile(kKeyA));
  EXPECT_FALSE(index.Lookup(id_a));
  EXPECT_TRUE(index.Lookup(id_b));
  // A revoked id is never handed out again nor re-registrable.
  EXPECT_FALSE(index.Register(id_a, a));

  TargetOwnerIndex targets;
  targets.Register("t1", {a, id_a, "workspace"});
  targets.Register("t2", {b, id_b, "workspace"});
  EXPECT_EQ(OwnerCheck::kForbidden, targets.Check("t1", b));
  EXPECT_EQ(OwnerCheck::kOk, targets.Check("t2", b));
  EXPECT_EQ(std::vector<std::string>{"t1"}, targets.RemoveAllForProfile(kKeyA));
  EXPECT_EQ(OwnerCheck::kUnknown, targets.Check("t1", a));
  EXPECT_EQ(std::vector<std::string>{"t2"}, targets.RemoveAllForSession(id_b));
  EXPECT_EQ(0u, targets.size());
}

// --- Admin token ---------------------------------------------------------------------------------

TEST(BrowserAdminAuthTest, GeneratedTokenFormat) {
  std::string token = GenerateAdminToken();
  EXPECT_TRUE(IsValidAdminTokenFormat(token));
  EXPECT_EQ(43u, token.size());  // 256 bits, base64url, no padding
  EXPECT_NE(token, GenerateAdminToken());
  EXPECT_FALSE(IsValidAdminTokenFormat("short"));
  EXPECT_FALSE(IsValidAdminTokenFormat(std::string(43, '+')));
}

TEST(BrowserAdminAuthTest, VerifyIsExactAndFailsClosed) {
  SetBrowserAdminTokenForTesting("");
  EXPECT_FALSE(IsBrowserAdminAuthAvailable());
  EXPECT_FALSE(VerifyBrowserAdminToken(""));
  EXPECT_FALSE(VerifyBrowserAdminToken("anything"));

  std::string token = GenerateAdminToken();
  SetBrowserAdminTokenForTesting(token);
  EXPECT_TRUE(IsBrowserAdminAuthAvailable());
  EXPECT_TRUE(VerifyBrowserAdminToken(token));
  EXPECT_FALSE(VerifyBrowserAdminToken(""));
  EXPECT_FALSE(VerifyBrowserAdminToken(token.substr(1)));
  EXPECT_FALSE(VerifyBrowserAdminToken(token + "x"));
  std::string flipped = token;
  flipped[0] = flipped[0] == 'a' ? 'b' : 'a';
  EXPECT_FALSE(VerifyBrowserAdminToken(flipped));
  SetBrowserAdminTokenForTesting("");
}

TEST(BrowserAdminAuthTest, TokenFileIsCreatedOnceAndReloaded) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ASSERT_TRUE(InitBrowserAdminAuth(dir.GetPath()));
  base::FilePath path = dir.GetPath().AppendASCII(kAdminTokenFileName);
  std::string stored;
  ASSERT_TRUE(base::ReadFileToString(path, &stored));
  std::string token(base::TrimWhitespaceASCII(stored, base::TRIM_ALL));
  EXPECT_TRUE(IsValidAdminTokenFormat(token));
  EXPECT_TRUE(VerifyBrowserAdminToken(token));

  // A second start loads the same token (not a new one).
  SetBrowserAdminTokenForTesting("");
  ASSERT_TRUE(InitBrowserAdminAuth(dir.GetPath()));
  EXPECT_TRUE(VerifyBrowserAdminToken(token));

#if BUILDFLAG(IS_POSIX)
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(path, &mode));
  EXPECT_EQ(0, mode & (base::FILE_PERMISSION_GROUP_MASK |
                       base::FILE_PERMISSION_OTHERS_MASK));
#endif

  // An unusable file is replaced, never trusted.
  ASSERT_TRUE(base::WriteFile(path, "short\n"));
  SetBrowserAdminTokenForTesting("");
  ASSERT_TRUE(InitBrowserAdminAuth(dir.GetPath()));
  EXPECT_FALSE(VerifyBrowserAdminToken("short"));
  EXPECT_FALSE(VerifyBrowserAdminToken(token));
  SetBrowserAdminTokenForTesting("");
}

}  // namespace shuttle

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOT YET COMPILED nor run (no native build on the authoring machine). It is
// the Gate of tools/shuttle/docs/PROFILES.md section 5 "Lifecycle": load two
// regular profiles in one headless browser; stop/delete A does not affect B;
// restart persists the mapping; the bootstrap runs before the keyed services.
// Source-only: //chrome/browser/shuttle:browser_tests is an opt-in source_set,
// NOT an executable, and the required Chrome test harness is pruned.

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/callback.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/shuttle/browser_profile_router.h"
#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "chrome/browser/shuttle/managed_profile_bootstrap.h"
#include "chrome/browser/shuttle/managed_network_admission.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "chrome/browser/shuttle/profile_persona_service.h"
#include "chrome/browser/shuttle/profile_persona_service_factory.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "chrome/browser/shuttle/profile_proxy_service_factory.h"
#include "chrome/browser/shuttle/switches.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/test/test_browser_context.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace shuttle {

namespace {

using RecordResult = base::expected<ProfileRecord, BrowserProfileFailure>;

constexpr char kSideFileName[] = "ShuttleLifecycleTestState.txt";

// A drain delegate that completes at once, or only when told to.
class FakeDrainDelegate : public BrowserProfileDrainDelegate {
 public:
  void DrainProfile(Profile* profile,
                    const std::string& key,
                    int64_t epoch,
                    base::OnceCallback<void(bool)> done) override {
    drained_keys.push_back(key);
    if (hold) {
      held_done = std::move(done);
      return;
    }
    std::move(done).Run(true);
  }
  bool HasDescendantHandles(Profile* profile) override { return busy; }

  bool hold = false;
  bool busy = false;
  base::OnceCallback<void(bool)> held_done;
  std::vector<std::string> drained_keys;
};

CreateProfileArgs MakeArgs(const std::string& preset, const std::string& locale) {
  CreateProfileArgs args;
  args.request_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  args.workspace_agent = "workspace";
  base::DictValue persona;
  persona.Set("preset", preset);
  if (!locale.empty()) {
    persona.Set("locale", locale);
  }
  args.persona = base::Value(std::move(persona));
  return args;
}

}  // namespace

class BrowserProfileLifecycleBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(kIsolatedProfilesSwitch);
  }

 protected:
  BrowserProfileRouter* router() { return BrowserProfileRouter::Get(); }

  template <typename F>
  RecordResult Await(F&& start) {
    base::RunLoop loop;
    std::optional<RecordResult> result;
    start(base::BindLambdaForTesting([&](RecordResult r) {
      result = std::move(r);
      loop.Quit();
    }));
    loop.Run();
    return std::move(*result);
  }

  RecordResult Create(CreateProfileArgs args) {
    return Await([&](ProfileRecordCallback cb) {
      router()->CreateProfile(std::move(args), std::move(cb));
    });
  }
  RecordResult Load(const std::string& key) {
    return Await([&](ProfileRecordCallback cb) {
      router()->LoadProfile(key, std::move(cb));
    });
  }
  RecordResult Stop(const std::string& key) {
    return Await([&](ProfileRecordCallback cb) {
      router()->StopProfile(key, std::move(cb));
    });
  }
  RecordResult Delete(const std::string& key) {
    return Await([&](ProfileRecordCallback cb) {
      router()->DeleteProfile(key, std::move(cb));
    });
  }

  Profile* ProfileOf(const std::string& key) {
    auto resolved = router()->Resolve(key);
    return resolved.has_value() ? resolved->profile.get() : nullptr;
  }

  base::FilePath SideFile() {
    base::FilePath user_data_dir;
    EXPECT_TRUE(base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir));
    return user_data_dir.AppendASCII(kSideFileName);
  }

  FakeDrainDelegate delegate_;

  void HoldFlush(base::OnceClosure* held) {
    router()->drain_flush_for_testing_ = base::BindLambdaForTesting(
        [held](base::OnceClosure done) { *held = std::move(done); });
  }
  void ClearFlushHook() { router()->drain_flush_for_testing_.Reset(); }
  bool DrainInFlight(const std::string& key) {
    return router()->drains_in_flight_.contains(key);
  }
  bool HasRuntime(const std::string& key) {
    return router()->runtimes_.contains(key);
  }
  void CrashBeforeHandoffCommit() {
    router()->handoff_before_commit_for_testing_ = base::BindLambdaForTesting(
        [this](base::OnceClosure resume) {
          router()->registry_->SetWriteStartedForTesting(base::BindOnce([] {
            SimulateNetworkServiceCrash();
          }));
          std::move(resume).Run();
        });
  }
  bool PersonaPending(const std::string& key) {
    return router()->registry_->Find(key)->persona_replacement_pending;
  }
  void RetireForTest(Profile* profile, const std::string& key,
                     base::OnceCallback<void(bool)> done) {
    const auto* entry = router()->registry_->Find(key);
    router()->RetireProfileContexts(profile, key, entry->lifecycle_epoch,
        entry->persona_input_json, std::move(done));
  }
  void StageForTest(Profile* profile, content::StoragePartition* partition,
                    base::OnceCallback<void(bool)> done) {
    auto operation = router()->handoffs_.at(profile);
    if (operation->phase == BrowserProfileRouter::ProfileStorageHandoff::Phase::kRetiring)
      operation->Transition(BrowserProfileRouter::ProfileStorageHandoff::Phase::kRestoring);
    partition->StageNetworkContextHandoff(
        router()->MakeHandoffProof(profile, partition, operation), std::move(done));
  }
  void CheckProofRejections(Profile* profile, content::StoragePartition* partition,
                            content::StoragePartition* other) {
    auto operation = router()->handoffs_.at(profile);
    operation->Transition(BrowserProfileRouter::ProfileStorageHandoff::Phase::kRestoring);
    auto proof = router()->MakeHandoffProof(profile, partition, operation);
    EXPECT_TRUE(proof.IsValidFor(profile, partition));
    EXPECT_FALSE(proof.AuthorizesConfiguration(profile, partition));
    EXPECT_FALSE(proof.IsValidFor(profile, other));
    base::test::TestFuture<bool> wrong;
    other->StageNetworkContextHandoff(std::move(proof), wrong.GetCallback());
    EXPECT_FALSE(wrong.Get());
    auto stale = router()->MakeHandoffProof(profile, partition, operation);
    // Same operation/participant, but a new retirement attempt and generation.
    base::test::TestFuture<bool> retired;
    partition->RetireNetworkContext(retired.GetCallback());
    ASSERT_TRUE(retired.Get());
    base::test::TestFuture<bool> rejected;
    partition->StageNetworkContextHandoff(std::move(stale), rejected.GetCallback());
    EXPECT_FALSE(rejected.Get());
    auto current = router()->MakeHandoffProof(profile, partition, operation);
    auto moved = std::move(current);
    EXPECT_FALSE(current.IsValidFor(profile, partition));
    base::test::TestFuture<bool> restored;
    partition->StageNetworkContextHandoff(std::move(moved), restored.GetCallback());
    ASSERT_TRUE(restored.Get());
    base::test::TestFuture<bool> reused;
    partition->StageNetworkContextHandoff(std::move(moved), reused.GetCallback());
    EXPECT_FALSE(reused.Get());
    auto cancelled = router()->MakeHandoffProof(profile, partition, operation);
    base::test::TestFuture<void> aborted;
    router()->AbortProfileHandoff(profile, aborted.GetCallback());
    ASSERT_TRUE(aborted.Wait());
    EXPECT_FALSE(cancelled.IsValidFor(profile, partition));
    base::test::TestFuture<bool> after_cancel;
    partition->StageNetworkContextHandoff(std::move(cancelled), after_cancel.GetCallback());
    EXPECT_FALSE(after_cancel.Get());
  }
  void HoldRelease(network::mojom::StorageHandoffTransaction** control,
                   base::RepeatingClosure armed) {
    router()->handoff_release_for_testing_ = base::BindLambdaForTesting(
        [control, armed](network::mojom::StorageHandoffTransaction* transaction) {
          *control = transaction;
          transaction->HoldReleaseReplyForTesting();
          armed.Run();
        });
  }
  void ClearReleaseHook() { router()->handoff_release_for_testing_.Reset(); }
  void ExpectReleasePhase(Profile* profile) {
    EXPECT_EQ(router()->handoffs_.at(profile)->phase,
              BrowserProfileRouter::ProfileStorageHandoff::Phase::kReleasePossible);
  }
  void CancelRetirement(Profile* profile, const std::string& key,
                        base::OnceCallback<void(bool)> retired,
                        base::OnceClosure cancelled) {
    const auto* entry = router()->registry_->Find(key);
    router()->RetireProfileContexts(profile, key, entry->lifecycle_epoch,
        entry->persona_input_json, std::move(retired));
    router()->AbortProfileHandoff(profile, std::move(cancelled));
  }
  void FinishCancelled(const std::string& key, base::OnceClosure callback) {
    router()->FinishHandoffFailure(key, std::move(callback));
  }
  void CheckStaleOuterLoadReply(const std::string& key) {
    const auto entry = *router()->registry_->Find(key);
    // Deliver an obsolete persistence failure: it must not FailLoad, release a
    // runtime, or consume current waiters/locks, even though the reply failed.
    router()->OnProvisionedPersisted(key, entry.lifecycle_epoch - 1,
        entry.persona_input_json,
        base::unexpected(BrowserProfileFailure(BrowserProfileError::kStorageError)));
    EXPECT_EQ(router()->registry_->Find(key)->state, entry.state);
    EXPECT_EQ(router()->registry_->Find(key)->lifecycle_epoch, entry.lifecycle_epoch);
  }
  void CancelRestoredBeforeCommit(Profile* profile) {
    router()->handoff_before_commit_for_testing_ = base::BindLambdaForTesting(
        [this, profile](base::OnceClosure resume) {
          router()->AbortProfileHandoff(profile, std::move(resume));
        });
  }
  void FailHandoffRegistryCommit() {
    router()->handoff_before_commit_for_testing_ = base::BindLambdaForTesting(
        [this](base::OnceClosure resume) {
          router()->registry_->FailNextWriteForTesting();
          std::move(resume).Run();
        });
  }
  void FailSecondActivation(int* count) {
    router()->handoff_activate_for_testing_ = base::BindLambdaForTesting(
        [count](content::StoragePartition* partition,
                base::OnceCallback<void(bool)> done) {
          if (++*count == 2) {
            std::move(done).Run(false);
            return;
          }
          partition->ActivateNetworkContextHandoff(std::move(done));
        });
  }
  void ClearActivationHook() { router()->handoff_activate_for_testing_.Reset(); }
  void DisconnectRestoredOwnerBeforeCommit(Profile* profile) {
    router()->handoff_before_commit_for_testing_ = base::BindLambdaForTesting(
        [profile](base::OnceClosure resume) {
          profile->GetDefaultStoragePartition()->AbortNetworkContextHandoff(
              std::move(resume));
        });
  }
  void PersistPendingIntent(const std::string& key, std::string input) {
    base::RunLoop loop;
    router()->registry_->ReplacePersonaIntent(
        key, std::move(input), "{}",
        base::BindLambdaForTesting([&](auto result) {
          EXPECT_TRUE(result.has_value());
          loop.Quit();
        }));
    loop.Run();
  }

  std::string Request(network::SharedURLLoaderFactory* factory, const GURL& url) {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                  TRAFFIC_ANNOTATION_FOR_TESTS);
    std::string result;
    base::RunLoop loop;
    loader->DownloadToString(factory, base::BindLambdaForTesting(
        [&](std::optional<std::string> body) {
          result = body.value_or("REQUEST_FAILED");
          loop.Quit();
        }), 4096);
    loop.Run();
    return result;
  }
};

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       ServiceHandoffRejectsMissingAndDuplicateMembership) {
  using Result = network::mojom::StorageHandoffReleaseResult;
  auto member = network::mojom::StorageHandoffMember::New(
      base::UnguessableToken::Create(), 1);
  for (bool duplicate : {false, true}) {
    mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
    std::vector<network::mojom::StorageHandoffMemberPtr> expected;
    expected.push_back(member.Clone());
    if (duplicate) expected.push_back(member.Clone());
    base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
    content::GetNetworkService()->BeginStorageHandoff(
        transaction.BindNewPipeAndPassReceiver(), std::move(expected),
        begun.GetCallback());
    ASSERT_TRUE(begun.Wait());
    EXPECT_FALSE(begun.Get<0>().is_empty());
    EXPECT_FALSE(begun.Get<1>().is_empty());
    base::test::TestFuture<Result> released;
    transaction->Release(released.GetCallback());
    EXPECT_EQ(released.Get(), Result::kRejected);
  }
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       ServiceHandoffRejectsStaleAndForeignPreparation) {
  for (bool foreign : {false, true}) {
    auto identity = network::mojom::StorageHandoffMember::New(
        base::UnguessableToken::Create(), 7);
    mojo::Remote<network::mojom::NetworkContext> control;
    auto params = network::mojom::NetworkContextParams::New();
    params->initial_cookies = std::vector<net::CanonicalCookie>();
    params->handoff_member = identity.Clone();
    content::GetNetworkService()->CreateNetworkContext(
        control.BindNewPipeAndPassReceiver(), std::move(params));
    base::test::TestFuture<void> restored;
    control->AcknowledgeStorageHandoff(restored.GetCallback());
    ASSERT_TRUE(restored.Wait());
    mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
    std::vector<network::mojom::StorageHandoffMemberPtr> expected;
    expected.push_back(identity.Clone());
    base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
    content::GetNetworkService()->BeginStorageHandoff(
        transaction.BindNewPipeAndPassReceiver(), std::move(expected),
        begun.GetCallback());
    ASSERT_TRUE(begun.Wait());
    auto token = begun.Get<1>();
    if (foreign) token = base::UnguessableToken::Create();
    else ++identity->generation;
    mojo::Remote<network::mojom::NetworkContext> workload;
    base::test::TestFuture<bool> prepared;
    control->PrepareStorageHandoff(token, std::move(identity),
        workload.BindNewPipeAndPassReceiver(), prepared.GetCallback());
    EXPECT_FALSE(prepared.Get());
    base::test::TestFuture<network::mojom::StorageHandoffReleaseResult> released;
    transaction->Release(released.GetCallback());
    EXPECT_EQ(released.Get(), network::mojom::StorageHandoffReleaseResult::kRejected);
  }
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       ServiceHandoffRejectsDuplicateContextPreparation) {
  auto identity = network::mojom::StorageHandoffMember::New(
      base::UnguessableToken::Create(), 9);
  mojo::Remote<network::mojom::NetworkContext> control;
  auto params = network::mojom::NetworkContextParams::New();
  params->initial_cookies = std::vector<net::CanonicalCookie>();
  params->handoff_member = identity.Clone();
  content::GetNetworkService()->CreateNetworkContext(
      control.BindNewPipeAndPassReceiver(), std::move(params));
  base::test::TestFuture<void> restored;
  control->AcknowledgeStorageHandoff(restored.GetCallback());
  ASSERT_TRUE(restored.Wait());
  mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
  std::vector<network::mojom::StorageHandoffMemberPtr> expected;
  expected.push_back(identity.Clone());
  base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
  content::GetNetworkService()->BeginStorageHandoff(
      transaction.BindNewPipeAndPassReceiver(), std::move(expected), begun.GetCallback());
  ASSERT_TRUE(begun.Wait());
  mojo::Remote<network::mojom::NetworkContext> workloads[2];
  for (size_t i = 0; i < 2; ++i) {
    base::test::TestFuture<bool> prepared;
    control->PrepareStorageHandoff(begun.Get<1>(), identity.Clone(),
        workloads[i].BindNewPipeAndPassReceiver(), prepared.GetCallback());
    EXPECT_EQ(prepared.Get(), i == 0);
  }
  // Rejection of a redundant Prepare must not replace the first workload pipe.
  base::test::TestFuture<void> dispatched;
  workloads[0]->AcknowledgeStorageHandoff(dispatched.GetCallback());
  base::test::TestFuture<network::mojom::StorageHandoffReleaseResult> released;
  transaction->Release(released.GetCallback());
  EXPECT_EQ(released.Get(), network::mojom::StorageHandoffReleaseResult::kReleased);
  EXPECT_TRUE(dispatched.Wait());
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       ServiceHandoffQueuesOrdinaryReceiversUntilCommonRelease) {
  // Count at the HTTP server, not at a mirrored client-side state machine.
  std::atomic<int> requests{0};
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* requests, const net::test_server::HttpRequest& request)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*requests;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_content("released");
        return response;
      }, &requests));
  ASSERT_TRUE(server.Start());
  using Result = network::mojom::StorageHandoffReleaseResult;
  mojo::Remote<network::mojom::NetworkContext> controls[2];
  mojo::Remote<network::mojom::NetworkContext> workloads[2];
  std::vector<network::mojom::StorageHandoffMemberPtr> members;
  for (size_t i = 0; i < 2; ++i) {
    auto member = network::mojom::StorageHandoffMember::New(
        base::UnguessableToken::Create(), i + 1);
    auto params = network::mojom::NetworkContextParams::New();
    params->initial_cookies = std::vector<net::CanonicalCookie>();
    params->handoff_member = member.Clone();
    content::GetNetworkService()->CreateNetworkContext(
        controls[i].BindNewPipeAndPassReceiver(), std::move(params));
    base::test::TestFuture<void> restored;
    controls[i]->AcknowledgeStorageHandoff(restored.GetCallback());
    ASSERT_TRUE(restored.Wait());
    members.push_back(std::move(member));
  }
  mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
  std::vector<network::mojom::StorageHandoffMemberPtr> expected;
  for (const auto& member : members) expected.push_back(member.Clone());
  base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
  content::GetNetworkService()->BeginStorageHandoff(
      transaction.BindNewPipeAndPassReceiver(), std::move(expected),
      begun.GetCallback());
  ASSERT_TRUE(begun.Wait());
  base::test::TestFuture<void> queued[2];
  base::test::TestFuture<uint32_t> bound_counts[2];
  std::unique_ptr<network::SimpleURLLoader> loaders[2];
  scoped_refptr<network::SharedURLLoaderFactory> factories[2];
  base::test::TestFuture<std::optional<std::string>> responses[2];
  for (size_t i = 0; i < 2; ++i) {
    auto receiver = workloads[i].BindNewPipeAndPassReceiver();
    workloads[i]->AcknowledgeStorageHandoff(queued[i].GetCallback());
    // This ordinary request is ahead of factory creation on the same pipe.
    // Its shared service-side observation must see the completed whole batch,
    // not merely this context's own receiver binding.
    workloads[i]->GetStorageHandoffBoundCountForTesting(
        bound_counts[i].GetCallback());
    mojo::Remote<network::mojom::URLLoaderFactory> factory;
    auto factory_params = network::mojom::URLLoaderFactoryParams::New();
    factory_params->process_id = network::OriginatingProcessId::browser();
    factory_params->is_orb_enabled = false;
    factory_params->automatically_assign_isolation_info = true;
    workloads[i]->CreateURLLoaderFactory(factory.BindNewPipeAndPassReceiver(),
                                        std::move(factory_params));
    auto wrapper = base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
        std::move(factory));
    factories[i] = wrapper;
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = server.GetURL("/handoff");
    loaders[i] = network::SimpleURLLoader::Create(
        std::move(request), TRAFFIC_ANNOTATION_FOR_TESTS);
    loaders[i]->DownloadToString(wrapper.get(), responses[i].GetCallback(), 4096);
    base::test::TestFuture<bool> prepared;
    controls[i]->PrepareStorageHandoff(begun.Get<1>(), members[i].Clone(),
        std::move(receiver), prepared.GetCallback());
    ASSERT_TRUE(prepared.Get());
  }
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(queued[0].IsReady());
  EXPECT_FALSE(queued[1].IsReady());
  EXPECT_EQ(requests.load(), 0);
  EXPECT_FALSE(responses[0].IsReady());
  EXPECT_FALSE(responses[1].IsReady());
  base::test::TestFuture<Result> released;
  transaction->Release(released.GetCallback());
  ASSERT_EQ(released.Get(), Result::kReleased);
  EXPECT_TRUE(queued[0].Wait());
  EXPECT_TRUE(queued[1].Wait());
  EXPECT_EQ(bound_counts[0].Get(), 2u);
  EXPECT_EQ(bound_counts[1].Get(), 2u);
  EXPECT_EQ(responses[0].Get(), std::optional<std::string>("released"));
  EXPECT_EQ(responses[1].Get(), std::optional<std::string>("released"));
  EXPECT_EQ(requests.load(), 2);
  // Release is idempotent: it must never certify Rejected after binding.
  base::test::TestFuture<Result> repeated;
  transaction->Release(repeated.GetCallback());
  EXPECT_EQ(repeated.Get(), Result::kReleased);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterRestoreOwnerDisconnectRetainsIntentAndRetries) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  profile->GetDefaultStoragePartition()->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  DisconnectRestoredOwnerBeforeCommit(profile);
  auto result = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key,
        R"({"preset":"linux-chrome","locale":"de-DE"})", std::move(cb));
  });
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  EXPECT_FALSE(PersonaPending(a->core_profile_key));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterPartialActivationRollsBackAllPartitionsAndRetries) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  profile->GetDefaultStoragePartition()->GetNetworkContext();
  // The managed workspace is a second loaded partition (no synthetic protocol).
  auto config = content::StoragePartitionConfig::Create(
      profile, "shuttle-workspace", "", false);
  profile->GetStoragePartition(config)->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  int activations = 0;
  FailSecondActivation(&activations);
  auto result = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key,
        R"({"preset":"linux-chrome","locale":"de-DE"})", std::move(cb));
  });
  ClearActivationHook();
  EXPECT_GE(activations, 2);
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  EXPECT_TRUE(router()->Resolve(b->core_profile_key).has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  EXPECT_FALSE(PersonaPending(a->core_profile_key));
  EXPECT_TRUE(ManagedNetworkAdmissionReady(profile));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterRegistryFailureFencesEverySiblingBeforeCompletion) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* first = profile->GetDefaultStoragePartition();
  auto config = content::StoragePartitionConfig::Create(
      profile, "handoff-sibling", "", false);
  auto* sibling = profile->GetStoragePartition(config);
  first->GetNetworkContext();
  sibling->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  FailHandoffRegistryCommit();
  auto result = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key,
        R"({"preset":"linux-chrome","locale":"de-DE"})", std::move(cb));
  });
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  // The production retry owns fresh authority and may proceed only after ALL
  // old service lifetimes have been fenced by the failed operation.
  for (auto* partition : {first, sibling}) {
    EXPECT_TRUE(partition->GetNetworkContextHandoffState()->cookies.has_value());
  }
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterHandoffLostBeforeCommitCompensatesAndRetries) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  profile->GetDefaultStoragePartition()->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  CrashBeforeHandoffCommit();
  const std::string intent =
      R"({"preset":"linux-chrome","browserVersion":"151.0.7100.1","locale":"de-DE"})";
  auto failed = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key, intent, std::move(cb));
  });
  EXPECT_FALSE(failed.has_value());
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  EXPECT_FALSE(router()->Resolve(a->core_profile_key).has_value());
  // The production loaded-pending path must use the retained EMPTY snapshot,
  // not quietly reconnect the failed staged generation.
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  EXPECT_FALSE(PersonaPending(a->core_profile_key));
  EXPECT_TRUE(ManagedNetworkAdmissionReady(profile));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterCancellationRetirementAndRestoreAreTerminalOnce) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  profile->GetDefaultStoragePartition()->GetNetworkContext();
  CheckStaleOuterLoadReply(a->core_profile_key);
  EXPECT_EQ(ProfileOf(a->core_profile_key), profile);
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  base::test::TestFuture<bool> retired;
  base::test::TestFuture<void> cancelled;
  CancelRetirement(profile, a->core_profile_key, retired.GetCallback(),
                   cancelled.GetCallback());
  EXPECT_TRUE(cancelled.Wait());
  EXPECT_FALSE(retired.Get());
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  // Join the router's production failure finalizer before starting a new op.
  base::test::TestFuture<void> finished;
  FinishCancelled(a->core_profile_key, finished.GetCallback());
  ASSERT_TRUE(finished.Wait());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  CancelRestoredBeforeCommit(profile);
  int completions = 0;
  auto failed = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key,
        R"({"preset":"linux-chrome","locale":"de-DE"})",
        base::BindLambdaForTesting([&, cb = std::move(cb)](RecordResult result) mutable {
          ++completions;
          std::move(cb).Run(std::move(result));
        }));
  });
  EXPECT_FALSE(failed.has_value());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(completions, 1);
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  EXPECT_EQ(completions, 1);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       RouterLostReleaseReplyAfterCookieMutationFailsClosed) {
  net::EmbeddedTestServer server;
  std::atomic<int> requests{0};
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* count, const net::test_server::HttpRequest&)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*count;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->AddCustomHeader("Set-Cookie", "after_release=new; Path=/");
        response->set_content("mutated");
        return response;
      }, &requests));
  ASSERT_TRUE(server.Start());
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* first = profile->GetDefaultStoragePartition();
  auto* sibling = profile->GetStoragePartition(
      content::StoragePartitionConfig::Create(profile, "lost-reply", "", false));
  first->GetNetworkContext();
  sibling->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  network::mojom::StorageHandoffTransaction* release = nullptr;
  base::test::TestFuture<void> armed;
  HoldRelease(&release, armed.GetRepeatingCallback());
  int completions = 0;
  base::test::TestFuture<RecordResult> failed;
  router()->ReplacePersona(a->core_profile_key,
      R"({"preset":"linux-chrome","locale":"de-DE"})",
      base::BindLambdaForTesting([&](RecordResult result) {
        ++completions;
        failed.SetValue(std::move(result));
      }));
  ASSERT_TRUE(armed.Wait());
  ExpectReleasePhase(profile);
  auto duplicate = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key,
        R"({"preset":"linux-chrome","locale":"fr-FR"})", std::move(cb));
  });
  EXPECT_FALSE(duplicate.has_value());
  EXPECT_FALSE(failed.IsReady());
  // A cached browser factory queues through the real staged workload receiver.
  // Release runs in the service, but its Mojo reply remains unsent.
  EXPECT_EQ(Request(first->GetURLLoaderFactoryForBrowserProcess().get(),
                    server.GetURL("/mutate")), "mutated");
  ASSERT_NE(release, nullptr);
  EXPECT_FALSE(failed.IsReady());
  auto page = content::WebContents::Create(content::WebContents::CreateParams(profile));
  ASSERT_TRUE(content::NavigateToURL(page.get(), server.GetURL("/read")));
  EXPECT_EQ("after_release=new", content::EvalJs(page.get(), "document.cookie"));
  page.reset();
  // Drop the actual transport reply only AFTER observing Set-Cookie. This is
  // not partition-only withholding of browser finalization.
  base::test::TestFuture<void> context_disconnected;
  first->AbortNetworkContextHandoff(context_disconnected.GetCallback());
  release->DropReleaseReplyForTesting();
  ASSERT_FALSE(failed.Get().has_value());
  EXPECT_TRUE(context_disconnected.Wait());
  EXPECT_NE(failed.Get().error().message.find("indeterminate"), std::string::npos);
  EXPECT_EQ(completions, 1);
  EXPECT_TRUE(PersonaPending(a->core_profile_key));
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  EXPECT_FALSE(router()->Resolve(a->core_profile_key).has_value());
  for (auto* partition : {first, sibling}) {
    auto state = partition->GetNetworkContextHandoffState();
    EXPECT_TRUE(state->release_possible);
    EXPECT_FALSE(state->cookies.has_value());
    base::test::TestFuture<void> fenced;
    partition->AbortNetworkContextHandoff(fenced.GetCallback());
    EXPECT_TRUE(fenced.IsReady());  // public completion already fenced both
    base::test::TestFuture<bool> replay;
    partition->RetireNetworkContext(replay.GetCallback());
    EXPECT_FALSE(replay.Get());
  }
  EXPECT_FALSE(Load(a->core_profile_key).has_value());
  EXPECT_EQ(completions, 1);
  auto other = ProfileOf(b->core_profile_key)->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
  EXPECT_EQ(Request(other.get(), server.GetURL("/other")), "mutated");
  EXPECT_GE(requests.load(), 3);
  ClearReleaseHook();
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       ReleasedCookieMutationCannotReplayQuarantinedSnapshot) {
  net::EmbeddedTestServer server;
  std::atomic<int> requests{0};
  server.RegisterRequestHandler(base::BindRepeating(
      [](std::atomic<int>* requests, const net::test_server::HttpRequest&)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        ++*requests;
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->AddCustomHeader("Set-Cookie", "after_release=new; Path=/");
        response->set_content("mutated");
        return response;
      }, &requests));
  ASSERT_TRUE(server.Start());
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* partition = profile->GetDefaultStoragePartition();
  auto factory = partition->GetURLLoaderFactoryForBrowserProcess();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  PauseManagedNetworkAdmission(profile);
  base::test::TestFuture<bool> retired;
  RetireForTest(profile, a->core_profile_key, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  auto snapshot = partition->GetNetworkContextHandoffState();
  ASSERT_TRUE(snapshot->cookies.has_value());
  base::test::TestFuture<bool> restored;
  StageForTest(profile, partition, restored.GetCallback());
  ASSERT_TRUE(restored.Get());
  mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
  std::vector<network::mojom::StorageHandoffMemberPtr> members;
  members.push_back(partition->GetNetworkContextHandoffMember());
  base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
  content::GetNetworkService()->BeginStorageHandoff(
      transaction.BindNewPipeAndPassReceiver(), std::move(members), begun.GetCallback());
  ASSERT_TRUE(begun.Wait());
  partition->SetNetworkContextHandoffTransaction(begun.Get<1>());
  base::test::TestFuture<bool> prepared;
  partition->ActivateNetworkContextHandoff(prepared.GetCallback());
  ASSERT_TRUE(prepared.Get());
  partition->SetNetworkContextHandoffReleasePossible(true);
  // Deliberately never finalize the browser snapshot from the reply. This is
  // the partition's production lost-reply state, not a toy transaction model.
  transaction->Release(base::BindOnce(
      [](network::mojom::StorageHandoffReleaseResult) {}));
  EXPECT_EQ(Request(factory.get(), server.GetURL("/mutate")), "mutated");
  EXPECT_EQ(requests.load(), 1);
  auto page = content::WebContents::Create(content::WebContents::CreateParams(profile));
  ASSERT_TRUE(content::NavigateToURL(page.get(), server.GetURL("/read")));
  EXPECT_EQ("after_release=new", content::EvalJs(page.get(), "document.cookie"));
  page.reset();
  base::test::TestFuture<void> aborted, joined;
  partition->AbortNetworkContextHandoff(aborted.GetCallback());
  partition->AbortNetworkContextHandoff(joined.GetCallback());
  ASSERT_TRUE(aborted.Wait());
  ASSERT_TRUE(joined.Wait());
  EXPECT_FALSE(snapshot->cookies.has_value());
  EXPECT_TRUE(snapshot->release_possible);
  base::test::TestFuture<bool> replay;
  partition->RetireNetworkContext(replay.GetCallback());
  EXPECT_FALSE(replay.Get());
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  auto other = ProfileOf(b->core_profile_key)->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
  EXPECT_EQ(Request(other.get(), server.GetURL("/other-profile")), "mutated");
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       HandoffMissingNetworkServiceFailsClosed) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* partition = profile->GetDefaultStoragePartition();
  partition->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  base::test::TestFuture<bool> retired;
  RetireForTest(profile, a->core_profile_key, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  ProfileNetworkContextServiceFactory::GetInstance()->SetTestingFactory(
      profile, base::BindRepeating([](content::BrowserContext*)
          -> std::unique_ptr<KeyedService> { return nullptr; }));
  EXPECT_EQ(nullptr,
      ProfileNetworkContextServiceFactory::GetForContextIfExists(profile));
  base::test::TestFuture<bool> staged;
  StageForTest(profile, partition, staged.GetCallback());
  EXPECT_FALSE(staged.Get());
  EXPECT_FALSE(partition->IsNetworkContextHandoffRestored());
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       HandoffProofRejectsWrongParticipantStaleAttemptReuseAndCancel) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* partition = profile->GetDefaultStoragePartition();
  auto* other = ProfileOf(b->core_profile_key)->GetDefaultStoragePartition();
  partition->GetNetworkContext();
  other->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  base::test::TestFuture<bool> retired;
  RetireForTest(profile, a->core_profile_key, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  CheckProofRejections(profile, partition, other);
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  EXPECT_TRUE(ManagedNetworkAdmissionReady(ProfileOf(b->core_profile_key)));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       EmptyHandoffStagesWithoutAdmissionAndCrashFailsClosed) {
  router()->SetDrainDelegate(&delegate_);
  auto a = Create(MakeArgs("linux-chrome", "en-US"));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* partition = profile->GetDefaultStoragePartition();
  // Force a real context, even though this authoritative snapshot is empty.
  partition->GetNetworkContext();
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  PauseManagedNetworkAdmission(profile);
  base::test::TestFuture<bool> retired;
  RetireForTest(profile, a->core_profile_key, retired.GetCallback());
  ASSERT_TRUE(retired.Get());
  base::test::TestFuture<bool> restored;
  StageForTest(profile, partition, restored.GetCallback());
  ASSERT_TRUE(restored.Get());  // must not deadlock on workload admission
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  EXPECT_TRUE(partition->IsNetworkContextHandoffRestored());
  // A post-persistence fence is on the same service generation, no replay.
  base::test::TestFuture<bool> fence;
  partition->FenceNetworkContextHandoff(fence.GetCallback());
  ASSERT_TRUE(fence.Get());
  SimulateNetworkServiceCrash();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(partition->IsNetworkContextHandoffRestored());
  base::test::TestFuture<bool> failed_activation;
  partition->ActivateNetworkContextHandoff(failed_activation.GetCallback());
  EXPECT_FALSE(failed_activation.Get());
  base::test::TestFuture<bool> stale;
  StageForTest(profile, partition, stale.GetCallback());
  EXPECT_FALSE(stale.Get());
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  // A retry must explicitly retire/re-enter the handoff, not ordinary reconnect.
  base::test::TestFuture<bool> retry_retired;
  partition->RetireNetworkContext(retry_retired.GetCallback());
  ASSERT_TRUE(retry_retired.Get());
  base::test::TestFuture<bool> retry;
  StageForTest(profile, partition, retry.GetCallback());
  ASSERT_TRUE(retry.Get());
  mojo::Remote<network::mojom::StorageHandoffTransaction> transaction;
  std::vector<network::mojom::StorageHandoffMemberPtr> members;
  members.push_back(partition->GetNetworkContextHandoffMember());
  base::test::TestFuture<base::UnguessableToken, base::UnguessableToken> begun;
  content::GetNetworkService()->BeginStorageHandoff(
      transaction.BindNewPipeAndPassReceiver(), std::move(members),
      begun.GetCallback());
  ASSERT_TRUE(begun.Wait());
  partition->SetNetworkContextHandoffTransaction(begun.Get<1>());
  base::test::TestFuture<bool> activated;
  partition->ActivateNetworkContextHandoff(activated.GetCallback());
  EXPECT_TRUE(activated.Get());
  partition->SetNetworkContextHandoffReleasePossible(true);
  base::test::TestFuture<network::mojom::StorageHandoffReleaseResult> released;
  transaction->Release(released.GetCallback());
  ASSERT_EQ(released.Get(), network::mojom::StorageHandoffReleaseResult::kReleased);
  partition->FinalizeNetworkContextHandoff(true);
  auto state = partition->GetNetworkContextHandoffState();
  EXPECT_FALSE(state->cookies.has_value());
  // A second retirement must use the private owner, not accidentally reconnect
  // a workload receiver while admission is closed.
  base::test::TestFuture<bool> second_retirement;
  partition->RetireNetworkContext(second_retirement.GetCallback());
  ASSERT_TRUE(second_retirement.Get());
  EXPECT_TRUE(state->cookies.has_value());
  EXPECT_FALSE(partition->IsNetworkContextHandoffRestored());
  base::test::TestFuture<void> cleanup;
  partition->AbortNetworkContextHandoff(cleanup.GetCallback());
  ASSERT_TRUE(cleanup.Wait());
  EXPECT_TRUE(router()->Resolve(b->core_profile_key).has_value());
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       LoadedReplacementRetiresCachedFactoriesAndKeepsStorage) {
  net::EmbeddedTestServer server;
  server.RegisterRequestHandler(base::BindRepeating(
      [](const net::test_server::HttpRequest& request)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        if (request.relative_url == "/retained-sw.js") {
          response->set_content_type("application/javascript");
          response->set_content("self.addEventListener('install', e => "
                                "e.waitUntil(self.skipWaiting()));");
          return response;
        }
        response->set_content_type("text/html");
        auto ua = request.headers.find("User-Agent");
        auto lang = request.headers.find("Accept-Language");
        auto cookie = request.headers.find("Cookie");
        response->set_content((ua == request.headers.end() ? "" : ua->second) +
            "|" + (lang == request.headers.end() ? "" : lang->second) +
            "|" + (cookie == request.headers.end() ? "" : cookie->second));
        return response;
      }));
  ASSERT_TRUE(server.Start());
  router()->SetDrainDelegate(&delegate_);
  auto args = MakeArgs("linux-chrome", "en-US");
  args.persona.GetDict().Set("browserVersion", "150.0.7000.1");
  auto a = Create(std::move(args));
  auto b = Create(MakeArgs("linux-chrome", "en-US"));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  ASSERT_TRUE(Load(b->core_profile_key).has_value());
  Profile* profile = ProfileOf(a->core_profile_key);
  auto* partition = profile->GetDefaultStoragePartition();
  auto cached = partition->GetURLLoaderFactoryForBrowserProcess();
  auto other = ProfileOf(b->core_profile_key)->GetDefaultStoragePartition()
                   ->GetURLLoaderFactoryForBrowserProcess();
  const auto url = server.GetURL("/identity");
  const auto before = Request(cached.get(), url);
  const auto other_before = Request(other.get(), url);
  EXPECT_NE(std::string::npos, before.find("Chrome/150."));
  {
    auto page = content::WebContents::Create(
        content::WebContents::CreateParams(profile));
    ASSERT_TRUE(content::NavigateToURL(page.get(), url));
    EXPECT_TRUE(content::ExecJs(page.get(),
        "document.cookie='retained=yes; path=/'; localStorage.retained='yes'"));
    EXPECT_EQ(true, content::EvalJs(page.get(), R"JS(
      (async () => {
        const db = await new Promise((resolve, reject) => {
          const r = indexedDB.open('persona-retention', 1);
          r.onupgradeneeded = () => r.result.createObjectStore('state');
          r.onsuccess = () => resolve(r.result);
          r.onerror = () => reject(r.error);
        });
        await new Promise((resolve, reject) => {
          const tx = db.transaction('state', 'readwrite');
          tx.objectStore('state').put('retained-value', 'sentinel');
          tx.oncomplete = resolve;
          tx.onerror = () => reject(tx.error);
        });
        db.close();
        const reg = await navigator.serviceWorker.register('/retained-sw.js',
                                                          {scope: '/'});
        await navigator.serviceWorker.ready;
        return !!reg.active;
      })()
    )JS"));
  }
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  const std::string intent =
      R"({"preset":"linux-chrome","browserVersion":"151.0.7100.1","locale":"de-DE","languages":["de-DE","de"]})";
  auto replaced = Await([&](ProfileRecordCallback cb) {
    router()->ReplacePersona(a->core_profile_key, intent, std::move(cb));
  });
  ASSERT_TRUE(replaced.has_value());
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  EXPECT_EQ(profile, ProfileOf(a->core_profile_key));
  const auto after = Request(cached.get(), url);
  EXPECT_NE(std::string::npos, after.find("Chrome/151."));
  EXPECT_NE(std::string::npos, after.find("|de-DE"));
  EXPECT_NE(std::string::npos, after.find("retained=yes"));
  EXPECT_EQ(other_before, Request(other.get(), url));
  {
    auto page = content::WebContents::Create(
        content::WebContents::CreateParams(profile));
    ASSERT_TRUE(content::NavigateToURL(page.get(), url));
    EXPECT_EQ("yes", content::EvalJs(page.get(), "localStorage.retained"));
    EXPECT_EQ("retained=yes", content::EvalJs(page.get(), "document.cookie"));
    EXPECT_EQ("retained-value", content::EvalJs(page.get(), R"JS(
      (async () => {
        const db = await new Promise((resolve, reject) => {
          const r = indexedDB.open('persona-retention', 1);
          r.onsuccess = () => resolve(r.result);
          r.onerror = () => reject(r.error);
        });
        const value = await new Promise((resolve, reject) => {
          const r = db.transaction('state').objectStore('state').get('sentinel');
          r.onsuccess = () => resolve(r.result);
          r.onerror = () => reject(r.error);
        });
        db.close();
        return value;
      })()
    )JS"));
    // getRegistration, not register: stopping workers must not unregister them.
    EXPECT_EQ(true, content::EvalJs(page.get(), R"JS(
      (async () => {
        const reg = await navigator.serviceWorker.getRegistration('/');
        return !!reg && !!reg.active &&
               new URL(reg.active.scriptURL).pathname === '/retained-sw.js';
      })()
    )JS"));
    // This bare WebContents deliberately has no Chrome tab UA override: its
    // renderer navigation must obtain the new context's default UA as well.
    EXPECT_EQ(true, content::EvalJs(page.get(),
        "document.body.textContent.includes('Chrome/151.')"));
  }
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  PersistPendingIntent(a->core_profile_key,
      R"({"preset":"linux-chrome","browserVersion":"152.0.7200.1","locale":"fr-FR"})");
  ASSERT_TRUE(Load(a->core_profile_key).has_value());
  const auto recovered = Request(cached.get(), url);
  EXPECT_NE(std::string::npos, recovered.find("Chrome/152."));
  EXPECT_NE(std::string::npos, recovered.find("|fr-FR"));
  EXPECT_NE(std::string::npos, recovered.find("retained=yes"));
  {
    auto page = content::WebContents::Create(
        content::WebContents::CreateParams(profile));
    ASSERT_TRUE(content::NavigateToURL(page.get(), url));
    EXPECT_EQ("yes", content::EvalJs(page.get(), "localStorage.retained"));
    EXPECT_EQ("retained=yes", content::EvalJs(page.get(), "document.cookie"));
  }
  SimulateNetworkServiceCrash();
  EXPECT_NE(std::string::npos, Request(cached.get(), url).find("Chrome/152."));
  EXPECT_EQ(other_before, Request(other.get(), url));
  router()->SetDrainDelegate(nullptr);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       NetworkAdmissionWaitsAndFailureDropsContinuation) {
  content::TestBrowserContext context;
  BeginManagedNetworkAdmission(&context);
  bool configured = false;
  AwaitManagedNetworkAdmission(&context, base::BindLambdaForTesting([&] {
    configured = true;
  }));
  EXPECT_FALSE(configured);
  EXPECT_FALSE(ManagedNetworkAdmissionReady(&context));
  FinishManagedNetworkAdmission(&context, true);
  EXPECT_TRUE(configured);
  EXPECT_TRUE(ManagedNetworkAdmissionReady(&context));

  content::TestBrowserContext failed;
  BeginManagedNetworkAdmission(&failed);
  configured = false;
  AwaitManagedNetworkAdmission(&failed, base::BindLambdaForTesting([&] {
    configured = true;
  }));
  FinishManagedNetworkAdmission(&failed, false);
  AwaitManagedNetworkAdmission(&failed, base::BindLambdaForTesting([&] {
    configured = true;
  }));
  EXPECT_FALSE(configured);
  EXPECT_FALSE(ManagedNetworkAdmissionReady(&failed));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       LoadedPendingIntentCannotAcceptOldPersona) {
  router()->SetDrainDelegate(&delegate_);
  auto created = Create(MakeArgs("linux-chrome", ""));
  ASSERT_TRUE(created.has_value());
  const auto key = created->core_profile_key;
  ASSERT_TRUE(Load(key).has_value());
  Profile* profile = ProfileOf(key);
  ASSERT_TRUE(Stop(key).has_value());
  const std::string pending = R"({"preset":"linux-chrome","locale":"de-DE"})";
  // Reproduce persisted replacement whose live apply never completed.
  PersistPendingIntent(key, pending);
  EXPECT_FALSE(ManagedProfilePersonaMatchesIntent(profile, pending));
  ASSERT_TRUE(Load(key).has_value());
  EXPECT_EQ(profile, ProfileOf(key));
  EXPECT_TRUE(ManagedProfilePersonaMatchesIntent(profile, pending));
  ASSERT_TRUE(Stop(key).has_value());
  PersistPendingIntent(key, R"({"preset":"not-a-preset"})");
  auto corrupt = Load(key);
  ASSERT_FALSE(corrupt.has_value());
  EXPECT_EQ(BrowserProfileError::kPersonaInvalid, corrupt.error().code);
  EXPECT_FALSE(router()->Resolve(key).has_value());
  EXPECT_FALSE(ManagedNetworkAdmissionReady(profile));
  router()->SetDrainDelegate(nullptr);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       FlushTimeoutRetryJoinsSameDrain) {
  router()->SetDrainDelegate(&delegate_);
  auto created = Create(MakeArgs("linux-chrome", ""));
  ASSERT_TRUE(created.has_value());
  const auto key = created->core_profile_key;
  ASSERT_TRUE(Load(key).has_value());
  base::OnceClosure held;
  HoldFlush(&held);
  router()->SetDrainTimeoutForTesting(base::Milliseconds(30));
  auto first = Stop(key);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(BrowserProfileError::kDrainTimeout, first.error().code);
  ASSERT_TRUE(held);
  EXPECT_TRUE(DrainInFlight(key));
  EXPECT_TRUE(HasRuntime(key));
  auto retry = Stop(key);
  ASSERT_FALSE(retry.has_value());
  EXPECT_EQ(BrowserProfileError::kDrainTimeout, retry.error().code);
  EXPECT_EQ(1u, delegate_.drained_keys.size());
  std::move(held).Run();
  ClearFlushHook();
  auto stopped = Stop(key);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ("stopped", stopped->lifecycle);
  EXPECT_FALSE(DrainInFlight(key));
  EXPECT_FALSE(HasRuntime(key));
  router()->SetDrainDelegate(nullptr);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, CapabilitiesAndStartup) {
  ASSERT_TRUE(router());
  ASSERT_TRUE(router()->IsAvailable());
  base::DictValue caps = BuildCapabilities();
  EXPECT_EQ(1, caps.FindInt("capabilitiesVersion"));
  EXPECT_EQ(true, caps.FindDictByDottedPath("isolatedProfiles")->FindBool("supported"));
  EXPECT_EQ(true, caps.FindDictByDottedPath("isolatedProfiles")->FindBool("enabled"));
  // Scoped routing is B2's; it stays false until that lane declares it.
  EXPECT_EQ(false, caps.FindBool("scopedRouting"));
  router()->SetScopedRoutingReady(true);
  EXPECT_EQ(true, BuildCapabilities().FindBool("scopedRouting"));
  router()->SetScopedRoutingReady(false);
  EXPECT_EQ(*caps.FindString("browserEpoch"), router()->browser_epoch());

  // The legacy scope is the startup Profile, never a managed one, and it is
  // not marked as managed.
  auto startup = router()->Resolve(std::nullopt);
  ASSERT_TRUE(startup.has_value());
  EXPECT_TRUE(startup->is_startup);
  EXPECT_EQ(browser()->profile()->GetOriginalProfile(), startup->profile.get());
  EXPECT_FALSE(IsManagedProfile(startup->profile));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, MaliciousKeysFailClosed) {
  for (const char* bad : {"", "../../etc", "bp_", "BP_0123456789ABCDEF0123456789ABCDEF",
                          "bp_0123456789abcdef0123456789abcdef/../x", "Default"}) {
    auto resolved = router()->Resolve(std::string(bad));
    ASSERT_FALSE(resolved.has_value()) << bad;
    EXPECT_EQ(BrowserProfileError::kInvalidProfileKey, resolved.error().code) << bad;
    RecordResult loaded = Load(bad);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(BrowserProfileError::kInvalidProfileKey, loaded.error().code);
  }
  auto unknown = router()->Resolve(std::string("bp_00000000000000000000000000000000"));
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotFound, unknown.error().code);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       TwoRegularProfilesAreIndependent) {
  router()->SetDrainDelegate(&delegate_);

  RecordResult a = Create(MakeArgs("windows-chrome", "en-US"));
  RecordResult b = Create(MakeArgs("macos-chrome", "de-DE"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ("stopped", a->lifecycle);
  EXPECT_NE(a->core_profile_key, b->core_profile_key);

  // Replaying create (same requestId) returns the same profile, no new one.
  // (The args are rebuilt with a fresh requestId above, so replay is covered
  // by the registry unit test.)

  RecordResult loaded_a = Load(a->core_profile_key);
  RecordResult loaded_b = Load(b->core_profile_key);
  ASSERT_TRUE(loaded_a.has_value());
  ASSERT_TRUE(loaded_b.has_value());
  EXPECT_EQ("ready", loaded_a->lifecycle);
  EXPECT_EQ("ready", loaded_b->lifecycle);
  // Loading incremented the epoch exactly once each; repeating is idempotent.
  EXPECT_EQ(2, loaded_a->lifecycle_epoch);
  RecordResult again = Load(a->core_profile_key);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(2, again->lifecycle_epoch);

  Profile* profile_a = ProfileOf(a->core_profile_key);
  Profile* profile_b = ProfileOf(b->core_profile_key);
  Profile* startup = router()->ResolveStartupProfile();
  ASSERT_TRUE(profile_a && profile_b && startup);
  EXPECT_NE(profile_a, profile_b);
  EXPECT_NE(profile_a, startup);
  EXPECT_NE(profile_b, startup);
  EXPECT_NE(profile_a->GetPath(), profile_b->GetPath());
  EXPECT_EQ(g_browser_process->profile_manager()->user_data_dir(),
            profile_a->GetPath().DirName());

  // Each Profile carries its own committed persona; A's preset is not B's.
  ProfilePersonaService* persona_a = ProfilePersonaServiceFactory::GetForProfile(profile_a);
  ProfilePersonaService* persona_b = ProfilePersonaServiceFactory::GetForProfile(profile_b);
  ASSERT_TRUE(persona_a && persona_b);
  ASSERT_TRUE(persona_a->Get().has_value());
  ASSERT_TRUE(persona_b->Get().has_value());
  EXPECT_EQ("windows-chrome", persona_a->Get()->preset);
  EXPECT_EQ("macos-chrome", persona_b->Get()->preset);
  EXPECT_NE(persona_a->Get()->seed, persona_b->Get()->seed);
  EXPECT_TRUE(IsManagedProfile(profile_a));
  EXPECT_TRUE(IsManagedProfile(profile_b));

  // Stop A: B and the startup Profile are untouched, the browser lives.
  RecordResult stopped = Stop(a->core_profile_key);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ("stopped", stopped->lifecycle);
  EXPECT_EQ(3, stopped->lifecycle_epoch);  // the drain incremented it
  EXPECT_EQ(std::vector<std::string>{a->core_profile_key}, delegate_.drained_keys);
  auto resolved_a = router()->Resolve(a->core_profile_key);
  ASSERT_FALSE(resolved_a.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotReady, resolved_a.error().code);
  EXPECT_EQ(profile_b, ProfileOf(b->core_profile_key));
  EXPECT_EQ(startup, router()->ResolveStartupProfile());
  EXPECT_TRUE(g_browser_process->profile_manager()->IsValidProfile(profile_b));
  // Stopping again is idempotent.
  RecordResult stopped_again = Stop(a->core_profile_key);
  ASSERT_TRUE(stopped_again.has_value());
  EXPECT_EQ(3, stopped_again->lifecycle_epoch);

  // Reload A: persona restored, not regenerated.
  uint64_t seed_a = persona_a->Get()->seed;
  RecordResult reloaded = Load(a->core_profile_key);
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(4, reloaded->lifecycle_epoch);
  Profile* profile_a2 = ProfileOf(a->core_profile_key);
  ASSERT_TRUE(profile_a2);
  EXPECT_EQ(seed_a, ProfilePersonaServiceFactory::GetForProfile(profile_a2)->Get()->seed);

  // Delete needs `stopped`, no handles, and never touches B.
  RecordResult busy = Delete(a->core_profile_key);
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileBusy, busy.error().code);
  ASSERT_TRUE(Stop(a->core_profile_key).has_value());
  delegate_.busy = true;
  RecordResult with_handles = Delete(a->core_profile_key);
  ASSERT_FALSE(with_handles.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileBusy, with_handles.error().code);
  delegate_.busy = false;
  RecordResult deleted = Delete(a->core_profile_key);
  ASSERT_TRUE(deleted.has_value());
  EXPECT_EQ("deleted", deleted->lifecycle);
  EXPECT_TRUE(deleted->deletion_pending);
  RecordResult load_deleted = Load(a->core_profile_key);
  ASSERT_FALSE(load_deleted.has_value());
  EXPECT_EQ(BrowserProfileError::kProfileNotFound, load_deleted.error().code);
  EXPECT_EQ(profile_b, ProfileOf(b->core_profile_key));

  router()->SetDrainDelegate(nullptr);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, ConcurrentLoadsShareOneInit) {
  RecordResult created = Create(MakeArgs("linux-chrome", ""));
  ASSERT_TRUE(created.has_value());
  std::optional<RecordResult> first;
  std::optional<RecordResult> second;
  base::RunLoop loop;
  int pending = 2;
  auto done = [&](std::optional<RecordResult>* slot, RecordResult r) {
    *slot = std::move(r);
    if (--pending == 0) {
      loop.Quit();
    }
  };
  router()->LoadProfile(created->core_profile_key,
                        base::BindLambdaForTesting([&](RecordResult r) { done(&first, std::move(r)); }));
  router()->LoadProfile(created->core_profile_key,
                        base::BindLambdaForTesting([&](RecordResult r) { done(&second, std::move(r)); }));
  loop.Run();
  ASSERT_TRUE(first && first->has_value());
  ASSERT_TRUE(second && second->has_value());
  // One init: both see the same epoch (one increment from 1).
  EXPECT_EQ(2, (*first)->lifecycle_epoch);
  EXPECT_EQ(2, (*second)->lifecycle_epoch);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, DrainTimeoutKeepsDraining) {
  router()->SetDrainDelegate(&delegate_);
  RecordResult created = Create(MakeArgs("windows-chrome", ""));
  ASSERT_TRUE(created.has_value());
  ASSERT_TRUE(Load(created->core_profile_key).has_value());

  delegate_.hold = true;
  router()->SetDrainTimeoutForTesting(base::Milliseconds(50));
  RecordResult timed_out = Stop(created->core_profile_key);
  ASSERT_FALSE(timed_out.has_value());
  EXPECT_EQ(BrowserProfileError::kDrainTimeout, timed_out.error().code);
  // Never reported stopped while the drain is unfinished.
  auto list = router()->ListProfiles();
  ASSERT_TRUE(list.has_value());
  bool found = false;
  for (const ProfileRecord& record : list->profiles) {
    if (record.core_profile_key == created->core_profile_key) {
      found = true;
      EXPECT_EQ("draining", record.lifecycle);
    }
  }
  EXPECT_TRUE(found);
  // Admission is closed while draining.
  EXPECT_FALSE(router()->Resolve(created->core_profile_key).has_value());

  // The late completion finishes the stop.
  ASSERT_TRUE(delegate_.held_done);
  std::move(delegate_.held_done).Run(true);
  base::RunLoop().RunUntilIdle();
  RecordResult stopped = Stop(created->core_profile_key);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ("stopped", stopped->lifecycle);
  router()->SetDrainDelegate(nullptr);
}

// Persona and network admission precede bulk services, not early independent
// extension preference services.
IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, BootstrapRunsBeforeKeyedServices) {
  bool callback_ran = false;
  bool persona_ready_first = false;
  bool marker_ready_first = false;
  auto subscription =
      BrowserContextDependencyManager::GetInstance()
          ->RegisterCreateServicesCallbackForTesting(
              base::BindRepeating([&](content::BrowserContext* context) {
                Profile* profile = Profile::FromBrowserContext(context);
                // Only the managed profile created by this test: it has the
                // marker pref already, a startup/other profile does not.
                PrefService* prefs = profile->GetPrefs();
                if (!prefs->GetUserPrefValue(prefs::kManagedProfile)) {
                  return;
                }
                callback_ran = true;
                EXPECT_TRUE(ManagedNetworkAdmissionReady(profile));
                marker_ready_first = true;
                persona_ready_first =
                    FingerprintPersonaFromPrefValue(
                        prefs->GetUserPrefValue(prefs::kFingerprintPersona))
                        .state == StoredFingerprintPersona::State::kPersona;
                // No ProfilePersonaService exists yet.
                EXPECT_FALSE(ProfilePersonaServiceFactory::GetForProfileIfExists(profile));
              }));
  RecordResult created = Create(MakeArgs("windows-chrome", ""));
  ASSERT_TRUE(created.has_value());
  ASSERT_TRUE(Load(created->core_profile_key).has_value());
  EXPECT_TRUE(callback_ran);
  EXPECT_TRUE(marker_ready_first);
  EXPECT_TRUE(persona_ready_first);
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest,
                       FirstNetworkConfigurationHasPersonaAndProxyAuth) {
  auto args = MakeArgs("linux-chrome", "de-DE");
  args.proxy = base::Value("http://user:password@127.0.0.1:19999");
  auto created = Create(std::move(args));
  ASSERT_TRUE(created.has_value());
  ASSERT_TRUE(Load(created->core_profile_key).has_value());
  Profile* profile = ProfileOf(created->core_profile_key);
  ASSERT_TRUE(ManagedNetworkAdmissionReady(profile));
  auto* proxies = ProfileProxyServiceFactory::GetForProfile(profile);
  ASSERT_TRUE(proxies);
  EXPECT_TRUE(proxies->PasswordStorageReady());
  EXPECT_EQ(StoredProxy::State::kProxy, proxies->Lookup("workspace").state);
  auto params = network::mojom::NetworkContextParams::New();
  ASSERT_TRUE(ProfilePersonaServiceFactory::GetForProfile(profile)
                  ->ConfigureNetworkContextParams(params.get()));
  EXPECT_FALSE(params->user_agent.empty());
  EXPECT_FALSE(params->accept_language.empty());
  // Exercises the real content -> admission -> configuration -> construction
  // path, not only the pure gate; HTTP fixture evidence remains required.
  EXPECT_TRUE(profile->GetDefaultStoragePartition()->GetNetworkContext());
}

// A managed Profile whose committed persona is damaged is never ready and
// never falls back to the stock identity.
IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, CorruptPersonaFailsClosed) {
  router()->SetDrainDelegate(&delegate_);
  RecordResult created = Create(MakeArgs("windows-chrome", ""));
  ASSERT_TRUE(created.has_value());
  ASSERT_TRUE(Load(created->core_profile_key).has_value());
  Profile* profile = ProfileOf(created->core_profile_key);
  ASSERT_TRUE(profile);
  ProfilePersonaService* personas = ProfilePersonaServiceFactory::GetForProfile(profile);
  ASSERT_TRUE(personas->IsManaged());

  ASSERT_TRUE(Stop(created->core_profile_key).has_value());
  profile->GetPrefs()->SetDict(prefs::kFingerprintPersona,
                               base::DictValue().Set("version", 99));
  EXPECT_EQ(StoredFingerprintPersona::State::kCorrupt, personas->Lookup().state);
  EXPECT_FALSE(personas->Get().has_value());
  // Clearing would be a stock fallback: refused for a managed profile.
  personas->Clear();
  EXPECT_EQ(StoredFingerprintPersona::State::kCorrupt, personas->Lookup().state);

  RecordResult reload = Load(created->core_profile_key);
  ASSERT_FALSE(reload.has_value());
  EXPECT_EQ(BrowserProfileError::kPersonaInvalid, reload.error().code);
  EXPECT_FALSE(router()->Resolve(created->core_profile_key).has_value());
  router()->SetDrainDelegate(nullptr);
}

// PRE_: create a profile (never loaded) and remember its key and directory.
IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, PRE_RestartPersistsMapping) {
  ProfileManager* manager = g_browser_process->profile_manager();
  base::FilePath expected_dir = manager->GetNextExpectedProfileDirectoryPath();
  RecordResult created = Create(MakeArgs("windows-chrome", ""));
  ASSERT_TRUE(created.has_value());
  std::string content = created->core_profile_key + "\n" +
                        expected_dir.AsUTF8Unsafe() + "\n" +
                        std::to_string(created->lifecycle_epoch) + "\n" +
                        router()->browser_epoch() + "\n";
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(base::WriteFile(SideFile(), content));
}

IN_PROC_BROWSER_TEST_F(BrowserProfileLifecycleBrowserTest, RestartPersistsMapping) {
  std::string content;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::ReadFileToString(SideFile(), &content));
  }
  std::vector<std::string> lines = base::SplitString(
      content, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  ASSERT_EQ(4u, lines.size());
  const std::string& key = lines[0];

  // The mapping survived, the entry recovered as stopped, with a new boot id.
  auto list = router()->ListProfiles();
  ASSERT_TRUE(list.has_value());
  const ProfileRecord* found = nullptr;
  for (const ProfileRecord& record : list->profiles) {
    if (record.core_profile_key == key) {
      found = &record;
    }
  }
  ASSERT_TRUE(found);
  EXPECT_EQ("stopped", found->lifecycle);
  EXPECT_FALSE(found->loaded);
  EXPECT_EQ(lines[2], std::to_string(found->lifecycle_epoch));
  EXPECT_NE(lines[3], router()->browser_epoch());

  // A load Chromium starts on its own (session restore, --profile-directory)
  // is refused by the bootstrap: only the router may bring a managed Profile up.
  base::FilePath dir = base::FilePath::FromUTF8Unsafe(lines[1]);
  base::RunLoop loop;
  Profile* unexpected = reinterpret_cast<Profile*>(1);
  g_browser_process->profile_manager()->CreateProfileAsync(
      dir, base::BindLambdaForTesting([&](Profile* profile) {
        unexpected = profile;
        loop.Quit();
      }));
  loop.Run();
  EXPECT_EQ(nullptr, unexpected);

  // Through the router it loads, and the persona is committed.
  RecordResult loaded = Load(key);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ("ready", loaded->lifecycle);
  EXPECT_EQ(std::stoi(lines[2]) + 1, loaded->lifecycle_epoch);
}

}  // namespace shuttle

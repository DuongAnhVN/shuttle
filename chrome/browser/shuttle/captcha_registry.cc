// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/captcha_registry.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/types/expected.h"
#include "chrome/browser/shuttle/captcha_detector.h"
#include "chrome/browser/shuttle/session_manager.h"
#include "chrome/browser/shuttle/shuttle_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace shuttle {

namespace {

constexpr size_t kRingSize = 256;              // contract: 256 events
constexpr size_t kMaxWaiters = 64;
constexpr size_t kMaxChallenges = 128;
constexpr int kMissBeforeGone = 2;             // absent reads before "gone"
constexpr int kDefaultVerifyTimeoutMs = 6000;  // contract §2
constexpr int kMaxVerifyTimeoutMs = 20000;
constexpr int kDefaultDeadlineMs = 15000;
constexpr int kMaxDeadlineMs = 120000;

bool IsBusyState(const std::string& state) {
  return state == "solving" || state == "executing" ||
         state == "verifying";
}

bool IsLiveState(const std::string& state) {
  return state != "gone" && state != "finished";
}

bool RuleStillApplied(const std::vector<CaptchaRule>& rules,
                      const CaptchaRule& rule) {
  const std::string mine = CanonicalRuleJson(rule);
  return std::ranges::any_of(rules, [&](const CaptchaRule& candidate) {
    return candidate.enabled && candidate.id == rule.id &&
           CanonicalRuleJson(candidate) == mine;
  });
}

bool GetDouble(const base::DictValue& dict,
               std::string_view key,
               double* out) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return false;
  }
  if (value->is_double()) {
    *out = value->GetDouble();
    return true;
  }
  if (value->is_int()) {
    *out = static_cast<double>(value->GetInt());
    return true;
  }
  return false;
}

// The contract's Outcome object. `verified` is true only for "verified".
base::DictValue MakeOutcome(const std::string& outcome,
                            bool acted,
                            const std::string& reason,
                            int capture_ms,
                            int64_t execute_ms,
                            int64_t total_ms,
                            const std::string& detail) {
  base::DictValue value;
  value.Set("outcome", outcome);
  value.Set("acted", acted);
  value.Set("verified", outcome == "verified");
  if (!reason.empty()) {
    value.Set("reason", reason);
  }
  base::DictValue timings;
  if (capture_ms > 0) {
    timings.Set("captureMs", capture_ms);
  }
  if (execute_ms > 0) {
    timings.Set("executeMs", static_cast<int>(execute_ms));
  }
  timings.Set("totalMs", static_cast<int>(total_ms));
  value.Set("timings", std::move(timings));
  if (!detail.empty()) {
    value.Set("detail", detail);
  }
  return value;
}

CaptchaBindingSpec ToBindingSpec(const CaptchaRule& rule) {
  CaptchaBindingSpec spec;
  spec.root = rule.detect.root;
  spec.background = rule.binding.background;
  spec.piece = rule.binding.piece;
  spec.handle = rule.binding.handle;
  spec.track = rule.binding.track;
  spec.instruction = rule.binding.instruction;
  return spec;
}

}  // namespace

// static
CaptchaRegistry& CaptchaRegistry::Get() {
  static base::NoDestructor<CaptchaRegistry> instance;
  return *instance;
}

CaptchaRegistry::CaptchaRegistry()
    : boot_id_(base::HexEncodeLower(base::RandBytesAsVector(16))) {}

CaptchaRegistry::~CaptchaRegistry() = default;

CaptchaRegistry::SetOutcome::SetOutcome() = default;
CaptchaRegistry::SetOutcome::SetOutcome(SetOutcome&&) = default;
CaptchaRegistry::SetOutcome& CaptchaRegistry::SetOutcome::operator=(SetOutcome&&) = default;
CaptchaRegistry::SetOutcome::~SetOutcome() = default;

CaptchaRegistry::Waiter::Waiter(int64_t at_cursor) : cursor(at_cursor) {}

CaptchaRegistry::Waiter::~Waiter() = default;

CaptchaRegistry::Challenge::Challenge(content::WebContents* contents)
    : target_id(SessionManager::TargetIdOf(contents)), contents(contents) {}

CaptchaRegistry::Challenge::~Challenge() = default;

// --- Rule snapshot -------------------------------------------------------------

CaptchaRegistry::SetOutcome CaptchaRegistry::SetRules(
    const base::DictValue& args) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  SetOutcome outcome;
  int revision = 0;
  std::vector<CaptchaRule> rules;
  std::vector<CaptchaRulesError> errors;
  if (!ParseCaptchaRules(args, &revision, &rules, &errors)) {
    outcome.error = "invalid-rules";
    for (const CaptchaRulesError& error : errors) {
      base::DictValue detail;
      if (!error.rule_id.empty()) {
        detail.Set("ruleId", error.rule_id);
      }
      detail.Set("error", error.error);
      outcome.details.Append(std::move(detail));
    }
    last_error = errors.empty() ? "invalid rules" : errors.front().error;
    return outcome;
  }
  const std::string canonical = SerializeCaptchaRules(rules);
  if (revision <= revision_ && canonical != rules_canonical) {
    outcome.error = "stale-revision";
    outcome.revision = revision_;
    last_error = "stale-revision";
    return outcome;
  }
  if (canonical == rules_canonical && revision_ != 0) {
    revision_ = std::max(revision_, revision);
    last_error.clear();
    outcome.ok = true;
    outcome.revision = revision_;
    outcome.applied = static_cast<int>(rules_.size());
    return outcome;
  }
  const bool identical = canonical == rules_canonical;
  if (!identical || revision_ == 0) {
    rules_ = std::move(rules);
    rules_canonical = std::move(canonical);
  }
  revision_ = std::max(revision_, revision);
  enabled_count_ = static_cast<int>(std::ranges::count_if(
      rules_, [](const CaptchaRule& rule) { return rule.enabled; }));
  last_error.clear();
  outcome.ok = true;
  outcome.revision = revision_;
  outcome.applied = static_cast<int>(rules_.size());

  // The snapshot replaced: challenges whose rule went away retire (solvers in
  // flight finish on their own), detectors forget their frames.
  std::vector<Challenge*> doomed;
  for (auto& [id, challenge] : challenges_) {
    if (IsBusyState(challenge->state)) {
      continue;
    }
    const bool kept = std::ranges::any_of(rules_, [&](const CaptchaRule& rule) {
      return rule.enabled && rule.id == challenge->rule_id &&
             SerializeCaptchaRules(std::vector<CaptchaRule>{rule}) ==
                 SerializeCaptchaRules(std::vector<CaptchaRule>{challenge->rule});
    });
    if (!kept || !IsLiveState(challenge->state)) {
      doomed.push_back(challenge.get());
    }
  }
  for (Challenge* challenge : doomed) {
    RetireChallenge(challenge, "rule-changed");
  }
  std::vector<CaptchaDetector*> detectors;
  detectors.reserve(detectors_.size());
  for (auto& entry : detectors_) detectors.push_back(entry.second.get());
  for (CaptchaDetector* detector : detectors) {
    detector->ResetWatches();
    detector->CheckNow();
  }
  return outcome;
}

base::DictValue CaptchaRegistry::RulesStatusValue() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::DictValue value;
  value.Set("revision", revision_);
  value.Set("applied", static_cast<int>(rules_.size()));
  value.Set("enabledCount", enabled_count_);
  if (!last_error.empty()) {
    value.Set("lastError", last_error);
  }
  return value;
}

// --- Tab observation -----------------------------------------------------------

void CaptchaRegistry::ObserveProfile(ShuttleService* service) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!HasEnabledRules() || !service) {
    return;
  }
  for (const Session* session : service->sessions().List()) {
    for (content::WebContents* contents :
         service->sessions().TabsOf(session->id)) {
      EnsureTabObserved(contents);
    }
  }
}

bool CaptchaRegistry::EnsureTabObserved(content::WebContents* contents) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!HasEnabledRules() || !contents) {
    return false;
  }
  if (detectors_.contains(contents)) {
    return true;
  }
  CaptchaDetector* detector =
      detectors_.emplace(contents, std::make_unique<CaptchaDetector>(
                                       contents, this))
          .first->second.get();
  detector->CheckNow();
  return true;
}

void CaptchaRegistry::ForgetTab(content::WebContents* contents) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!contents) return;
  std::vector<std::string> ids;
  for (const auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents) ids.push_back(id);
  }
  for (const auto& id : ids) {
    auto it = challenges_.find(id);
    if (it == challenges_.end()) continue;
    Challenge* ch = it->second.get();
    const bool live = IsLiveState(ch->state);
    AbortSolve(ch, "tab-closed", "gone");
    RetireExecutor(&ch->executor);
    ch->state = "gone";
    if (live) EmitEvent("challenge-gone", *ch, std::string(), "tab-closed");
    challenges_.erase(it);
  }
  if (auto detector = detectors_.find(contents); detector != detectors_.end()) {
    detector->second->ResetWatches();
    detector->second->CheckNow();
  }
}

bool CaptchaRegistry::ProbeTab(content::WebContents* contents,
                               CaptchaDetector::ProbeCallback done) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!contents) {
    return false;  // `done` untouched: the tool answers the failure itself
  }
  if (!HasEnabledRules() || !EnsureTabObserved(contents)) {
    std::move(done).Run(base::ListValue());
    return true;
  }
  auto it = detectors_.find(contents);
  if (it == detectors_.end() || !it->second->CanProbe()) {
    return false;  // `done` untouched (over capacity)
  }
  return it->second->RunProbe(std::move(done));
}

// --- Tab lock --------------------------------------------------------------------

std::string CaptchaRegistry::LockStatusOf(
    content::WebContents* contents) const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!contents) {
    return std::string();
  }
  for (const auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents && IsBusyState(challenge->state)) {
      return challenge->state;
    }
  }
  return std::string();
}

bool CaptchaRegistry::IsTabLocked(content::WebContents* contents) const {
  return !LockStatusOf(contents).empty();
}

// --- From the detector ------------------------------------------------------------

CaptchaRegistry::Challenge* CaptchaRegistry::FindLiveChallenge(
    content::WebContents* contents,
    const std::string& frame_key,
    const std::string& rule_id) {
  for (auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents &&
        challenge->frame_key == frame_key && challenge->rule_id == rule_id &&
        IsLiveState(challenge->state)) {
      return challenge.get();
    }
  }
  return nullptr;
}

void CaptchaRegistry::OnWidgetState(content::WebContents* contents,
                                    const std::string& frame_key,
                                    const std::string& rule_id,
                                    const std::string& state,
                                    const std::string& widget_key,
                                    const base::ListValue* missing) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!contents) {
    return;
  }
  Challenge* challenge = FindLiveChallenge(contents, frame_key, rule_id);
  if (state == "ready") {
    const CaptchaRule* rule = nullptr;
    for (const CaptchaRule& candidate : rules_) {
      if (candidate.enabled && candidate.id == rule_id) {
        rule = &candidate;
        break;
      }
    }
    if (!rule) {
      return;  // disabled or gone from the snapshot: nothing to open
    }
    if (!challenge) {
      content::RenderFrameHost* frame = FindFrame(contents, frame_key);
      if (!frame) {
        return;
      }
      auto fresh = std::make_unique<Challenge>(contents);
      fresh->id = base::StrCat(
          {"ch_", base::HexEncodeLower(base::RandBytesAsVector(8))});
      fresh->frame_key = frame_key;
      fresh->rule_id = rule->id;
      fresh->rule = *rule;
      fresh->frame_url = frame->GetLastCommittedURL().spec();
      Challenge* raw = fresh.get();
      challenges_.emplace(raw->id, std::move(fresh));
      TrimChallenges();
      challenge = raw;
    }
    challenge->miss_streak = 0;
    if (IsBusyState(challenge->state) || challenge->collect_in_flight) {
      return;  // the check after the solve/collect finishes re-enters this
    }
    const bool key_changed =
        !widget_key.empty() && widget_key != challenge->widget_key;
    if (!widget_key.empty()) {
      challenge->widget_key = widget_key;
    }
    if (!challenge->collected || key_changed) {
      StartCollect(challenge);
    }
    return;
  }
  // none / candidate / excluded: absent reads. `candidate` keeps a challenge
  // alive (its widget is still there, not solvable yet).
  if (!challenge || IsBusyState(challenge->state) ||
      challenge->collect_in_flight || state == "candidate") {
    if (challenge && state == "candidate") {
      challenge->miss_streak = 0;
    }
    return;
  }
  if (++challenge->miss_streak >= kMissBeforeGone) {
    RetireChallenge(challenge, state == "excluded" ? "expired" : "gone");
  }
}

void CaptchaRegistry::StartCollect(Challenge* challenge) {
  if (!RuleStillApplied(rules_, challenge->rule)) {
    RetireChallenge(challenge, "rule-changed");
    return;
  }
  content::RenderFrameHost* frame =
      FindFrame(challenge->contents, challenge->frame_key);
  if (!frame) {
    return;  // OnFrameGone retires it
  }
  challenge->collect_in_flight = true;
  challenge->frame_url = frame->GetLastCommittedURL().spec();
  CollectChallenge(frame, ToBindingSpec(challenge->rule),
                   base::BindOnce(&CaptchaRegistry::OnCollected,
                                  weak_factory_.GetWeakPtr(),
                                  challenge->weak_factory.GetWeakPtr(),
                                  base::TimeTicks::Now()));
}

void CaptchaRegistry::OnCollected(
    base::WeakPtr<Challenge> challenge,
    base::TimeTicks started,
    base::expected<CollectedChallenge, CollectError> result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Challenge* ch = challenge.get();
  if (!ch) {
    return;  // retired mid-flight; nothing waits on this
  }
  ch->collect_in_flight = false;
  if (!IsLiveState(ch->state)) {
    return;  // retired while the read was in flight
  }
  if (!result.has_value()) {
    // A solve awaiting this read (started while the collect was in flight)
    // fails with the collector's reason and no input sent.
    return;  // the next pass retries; no event for a failed read
  }
  if (IsBusyState(ch->state)) {
    return;  // a solve is using the stored snapshot: the next pass re-collects
  }
  CollectedChallenge collected = std::move(*result);
  const bool same = ch->collected && ch->digest == collected.digest;
  ch->capture_ms =
      static_cast<int>((base::TimeTicks::Now() - started).InMilliseconds());
  ch->captured_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  ch->collected = std::move(collected);
  if (same) {
    if (ch->state == "detected") {
      ch->state = "captured";
    }
    return;  // re-fire for the same live instance is suppressed
  }
  ch->digest = ch->collected->digest;
  ch->state_revision += 1;
  ch->state = "captured";
  EmitEvent("challenge-ready", *ch, std::string(), std::string());
}

void CaptchaRegistry::OnFrameGone(content::WebContents* contents,
                                  const std::string& frame_key) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::vector<Challenge*> doomed;
  for (auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents &&
        challenge->frame_key == frame_key &&
        IsLiveState(challenge->state)) {
      doomed.push_back(challenge.get());
    }
  }
  for (Challenge* challenge : doomed) {
    RetireChallenge(challenge, "frame-gone");
  }
}

void CaptchaRegistry::OnPageReset(content::WebContents* contents) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::vector<Challenge*> doomed;
  for (auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents && IsLiveState(challenge->state)) {
      doomed.push_back(challenge.get());
    }
  }
  for (Challenge* challenge : doomed) {
    RetireChallenge(challenge, "frame-gone");
  }
}

void CaptchaRegistry::OnTabGone(content::WebContents* contents,
                                CaptchaDetector* detector) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  std::vector<std::string> doomed;
  for (auto& [id, challenge] : challenges_) {
    if (challenge->contents == contents) {
      doomed.push_back(id);
    }
  }
  for (const std::string& id : doomed) {
    auto it = challenges_.find(id);
    if (it == challenges_.end()) {
      continue;
    }
    Challenge* challenge = it->second.get();
    const bool live = IsLiveState(challenge->state);
    AbortSolve(challenge, "tab-closed", "gone");
    // Never destroy an executor with its challenge while it may still be on
    // the stack: retire it and let the list clear on the next task.
    RetireExecutor(&challenge->executor);
    challenge->state = "gone";
    if (live) {
      EmitEvent("challenge-gone", *challenge, std::string(), "tab-closed");
    }
    challenges_.erase(it);  // the tab pointer must not outlive the tab
  }
  DetachLater(contents, detector);
}

// --- Challenges: retire -----------------------------------------------------------

void CaptchaRegistry::RetireChallenge(Challenge* challenge,
                                      const std::string& reason) {
  if (!challenge || !IsLiveState(challenge->state)) {
    return;
  }
  if (IsBusyState(challenge->state)) {
    if (challenge->executor && challenge->state == "executing" &&
        reason != "tab-closed") {
      challenge->retire_after_solve = true;
      challenge->retire_reason = reason;
      return;
    }
    AbortSolve(challenge, reason, "gone");
  }
  challenge->state = "gone";
  challenge->collected.reset();
  challenge->digest.clear();
  EmitEvent("challenge-gone", *challenge, std::string(), reason);
}

void CaptchaRegistry::TrimChallenges() {
  while (challenges_.size() > kMaxChallenges) {
    auto doomed = challenges_.end();
    for (auto it = challenges_.begin(); it != challenges_.end(); ++it) {
      const Challenge& challenge = *it->second;
      if (!IsBusyState(challenge.state) && !challenge.executor &&
          !challenge.solve_callback && !IsLiveState(challenge.state)) {
        doomed = it;
        break;
      }
    }
    if (doomed == challenges_.end()) {
      return;  // everything is live or in flight
    }
    challenges_.erase(doomed);
  }
}

// --- Solve / cancel --------------------------------------------------------------

void CaptchaRegistry::SolveChallenge(const base::DictValue& args,
                                     ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::string* id = args.FindString("challengeId");
  const std::optional<int> revision = args.FindInt("stateRevision");
  const base::DictValue* solution = args.FindDict("solution");
  double slide_x_proportion = 0.0;
  if (!id || !revision || !solution ||
      !GetDouble(*solution, "slideXProportion", &slide_x_proportion)) {
    std::move(callback).Run(ToolResult::Error(
        "solve-captcha-challenge needs {challengeId, stateRevision, "
        "solution.slideXProportion}"));
    return;
  }
  const int verify_timeout_ms =
      std::clamp(args.FindInt("verifyTimeoutMs").value_or(
                     kDefaultVerifyTimeoutMs),
                 0, kMaxVerifyTimeoutMs);
  const int deadline_ms = std::clamp(
      args.FindInt("deadlineMs").value_or(kDefaultDeadlineMs),
      500, std::min(kMaxDeadlineMs, 15000));

  auto it = challenges_.find(*id);
  if (it == challenges_.end() || !IsLiveState(it->second->state)) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "challenge-gone", 0, 0, 0, std::string()))));
    return;
  }
  Challenge* challenge = it->second.get();
  content::WebContents* contents = challenge->contents;
  if (IsBusyState(challenge->state)) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "captcha-in-progress", challenge->capture_ms, 0, 0,
        std::string()))));
    return;
  }
  if (challenge->collect_in_flight) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "challenge-capturing", challenge->capture_ms, 0, 0,
        std::string()))));
    return;
  }
  if (revision.value() < challenge->state_revision) {
    // An older view: answer stale, send no input (contract §2).
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "stale", false, std::string(), challenge->capture_ms, 0, 0,
        "older stateRevision"))));
    return;
  }
  if (revision.value() > challenge->state_revision) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "bad-solution", challenge->capture_ms, 0, 0,
        "stateRevision is ahead of the challenge"))));
    return;
  }
  if (!std::isfinite(slide_x_proportion) || slide_x_proportion < 0.0 ||
      slide_x_proportion > 1.0) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "bad-solution", challenge->capture_ms, 0, 0,
        std::string()))));
    return;
  }
  if (!challenge->collected) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "capture-failed", challenge->capture_ms, 0, 0,
        std::string()))));
    return;
  }
  content::RenderFrameHost* frame =
      FindFrame(contents, challenge->frame_key);
  if (!frame) {
    RetireChallenge(challenge, "frame-gone");
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", false, "challenge-gone", challenge->capture_ms, 0, 0,
        std::string()))));
    return;
  }
  challenge->state = "solving";
  challenge->solve_finished = false;
  challenge->retire_after_solve = false;
  challenge->solve_started = base::TimeTicks::Now();
  const uint64_t attempt = ++challenge->attempt;
  challenge->solve_callback = std::move(callback);
  EmitEvent("solve-started", *challenge, std::string(), std::string());
  challenge->solve_watchdog.Start(
      FROM_HERE, base::Milliseconds(deadline_ms) + base::Seconds(5),
      base::BindOnce(&CaptchaRegistry::OnSolveDeadline,
                    weak_factory_.GetWeakPtr(),
                     challenge->weak_factory.GetWeakPtr(), attempt));
  RecheckChallenge(frame, ToBindingSpec(challenge->rule),
                   *challenge->collected,
                   base::BindOnce(
                       &CaptchaRegistry::OnRechecked,
                       weak_factory_.GetWeakPtr(),
                        challenge->weak_factory.GetWeakPtr(),
                        attempt,
                       slide_x_proportion,
                       base::Milliseconds(verify_timeout_ms),
                       base::Milliseconds(deadline_ms)));
}

void CaptchaRegistry::OnRechecked(base::WeakPtr<Challenge> challenge,
                                  uint64_t attempt,
                                  double slide_x_proportion,
                                  base::TimeDelta verify_timeout,
                                  base::TimeDelta deadline,
                                  RecheckResult rechecked) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Challenge* ch = challenge.get();
  if (!ch || ch->attempt != attempt || !ch->solve_callback || !IsBusyState(ch->state)) {
    return;  // the solve was aborted meanwhile (its caller was answered)
  }
  if (!rechecked.fresh) {
    const std::string why =
        rechecked.reason.empty() ? "image-changed" : rechecked.reason;
    if (why == "frame-gone") {
      AbortSolve(ch, "challenge-gone", "captured");
      RetireChallenge(ch, "frame-gone");
      return;
    }
    // Contract §2: a fresh digest that differs (or the widget moved,
    // changed, hid) is answered stale with NO input sent.
    ch->solve_watchdog.Stop();
    ch->solve_finished = true;
    const int64_t total_ms =
        ch->solve_started.is_null()
            ? 0
            : (base::TimeTicks::Now() - ch->solve_started).InMilliseconds();
    ToolCallback callback = std::move(ch->solve_callback);
    EmitEvent("solve-finished", *ch, "stale", why);
    ch->state = "captured";
    if (callback) {
      std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
          "stale", false, why, ch->capture_ms, 0, total_ms, std::string()))));
    }
    StartCollect(ch);
    return;
  }
  // Executor reports only its final verdict; there is no phase notification
  // for an independently observable `verifying` state, so remain executing.
  if (!ch->collected) {
    AbortSolve(ch, "capture-failed", "captured");
    return;
  }
  const auto& geometry = ch->collected->geometry;
  const auto background = geometry.find("background");
  const auto piece = geometry.find("piece");
  const auto handle = geometry.find("handle");
  const auto track = geometry.find("track");
  if (background == geometry.end() || piece == geometry.end() ||
      handle == geometry.end() || track == geometry.end()) {
    AbortSolve(ch, "geometry-missing", "captured");
    return;
  }
  const std::optional<CaptchaSlideMath> math = BuildSlidePlan(
      slide_x_proportion, background->second.width, piece->second.width,
      handle->second.width,
      handle->second.x + handle->second.width / 2.0,
      handle->second.y + handle->second.height / 2.0, track->second.width,
      ch->rule.travel_direct);
  if (!math) {
    AbortSolve(ch, "geometry-missing", "captured");
    return;
  }
  content::RenderFrameHost* frame = FindFrame(ch->contents, ch->frame_key);
  if (!frame) {
    AbortSolve(ch, "challenge-gone", "gone");
    return;
  }
  CaptchaSlidePlan plan;
  plan.from_x = math->from_x;
  plan.from_y = math->from_y;
  plan.to_x = math->to_x;
  plan.to_y = math->to_y;
  plan.moves = math->moves;
  plan.hold_ms = math->hold_ms;
  CaptchaVerifySpec verify;
  verify.success_text = ch->rule.success.text;
  verify.success_selector = ch->rule.success.selector;
  verify.success_url = ch->rule.success.url;
  verify.failure_text = ch->rule.failure.text;
  verify.failure_selector = ch->rule.failure.selector;
  ch->state = "executing";
  ch->executor = std::make_unique<CaptchaExecutor>(ch->contents, frame);
  ch->executor->Run(
       plan, verify, verify_timeout, ch->solve_started + deadline,
       base::BindOnce(&CaptchaRegistry::OnSolved, weak_factory_.GetWeakPtr(),
                      ch->weak_factory.GetWeakPtr(), attempt));
}

void CaptchaRegistry::OnSolved(base::WeakPtr<Challenge> challenge,
                               uint64_t attempt,
                               CaptchaExecResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Challenge* ch = challenge.get();
  RetireExecutor(ch ? &ch->executor : nullptr);
  if (!ch || ch->attempt != attempt || ch->solve_finished) {
    return;  // gone with the tab, or an abort already answered this attempt
  }
  ch->solve_watchdog.Stop();
  const int64_t total_ms =
      ch->solve_started.is_null()
          ? 0
          : (base::TimeTicks::Now() - ch->solve_started).InMilliseconds();
  std::string outcome = result.outcome;
  std::string reason = result.reason;
  if (reason == "canceled" || reason == "frame-gone" || reason == "gone" ||
      reason == "rule-changed" || reason == "verify-failure") {
    reason = reason == "canceled" ? "cancelled" :
             reason == "verify-failure" ? "rejected" : "challenge-gone";
  }
  if (ch->forced) {
    // An abort (tab/frame death, cancel, deadline) speaks its own reason;
    // the executor's result still carries the true `acted` and timings.
    outcome = "error";
    reason = ch->forced_reason;
    ch->forced = false;
    ch->forced_reason.clear();
    ch->solve_finished = true;
    ch->state = ch->forced_state;
    ch->forced_state.clear();
    EmitEvent("solve-finished", *ch, outcome, reason);
    if (ch->solve_callback) {
      ToolCallback callback = std::move(ch->solve_callback);
      std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
          outcome, result.acted, reason, ch->capture_ms, result.execute_ms,
          total_ms, result.detail))));
    }
    if (ch->retire_after_solve) {
      ch->retire_after_solve = false;
      RetireChallenge(ch, ch->retire_reason);
    }
    return;
  }
  ch->solve_finished = true;
  if (outcome == "verified" || outcome == "rejected") {
    // The widget answered: retire it. The checks will see the same through
    // the excluded marker; the Manager treats a gone challenge on get.
    ch->collected.reset();
    ch->digest.clear();
    ch->state = "gone";
    EmitEvent("solve-finished", *ch, outcome, reason);
    EmitEvent("challenge-gone", *ch, std::string(),
              outcome == "verified" ? "solved" : "rejected");
  } else {
    // stale / expired / unconfirmed / error / unsupported: the picture may
    // still be live; the next check pass re-arms it.
    ch->state = "captured";
    EmitEvent("solve-finished", *ch, outcome, reason);
  }
  if (ch->solve_callback) {
    ToolCallback callback = std::move(ch->solve_callback);
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        outcome, result.acted, reason, ch->capture_ms, result.execute_ms,
        total_ms, result.detail))));
  }
  if (ch->retire_after_solve) {
    ch->retire_after_solve = false;
    RetireChallenge(ch, ch->retire_reason);
  }
  if (ch->state == "captured") {
    StartCollect(ch);
  }
}

void CaptchaRegistry::OnSolveDeadline(base::WeakPtr<Challenge> challenge,
                                      uint64_t attempt) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Challenge* ch = challenge.get();
  if (!ch || ch->attempt != attempt || !IsBusyState(ch->state)) {
    return;
  }
  AbortSolve(ch, "deadline-exceeded", "captured");
}

void CaptchaRegistry::AbortSolve(Challenge* challenge,
                                 const std::string& reason,
                                 const std::string& final_state) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsBusyState(challenge->state)) {
    if (IsLiveState(challenge->state)) {
      challenge->state = final_state;
    }
    return;
  }
  challenge->solve_watchdog.Stop();
  const bool maybe_acted = challenge->executor != nullptr;
  if (challenge->executor) {
    // Cancel() releases a held press and answers the Run() callback (at most
    // once); OnSolved then speaks this abort (forced) with the executor's
    // real acted/timings. The executor stays owned until OnSolved retires
    // it, so it never dies inside its own stack.
    challenge->forced = true;
    challenge->forced_reason = reason;
    challenge->forced_state = final_state;
    challenge->executor->Cancel(reason);
    if (!challenge->forced) {
      return;  // OnSolved ran inside Cancel() and answered
    }
    // The run had already finished without an answer reaching us (or Cancel
    // stayed silent): take the executor out and answer here.
    challenge->forced = false;
    challenge->forced_reason.clear();
    challenge->forced_state.clear();
    RetireExecutor(&challenge->executor);
  }
  challenge->solve_finished = true;
  const int64_t total_ms =
      challenge->solve_started.is_null()
          ? 0
          : (base::TimeTicks::Now() -
             challenge->solve_started)
                  .InMilliseconds();
  ToolCallback callback = std::move(challenge->solve_callback);
  EmitEvent("solve-finished", *challenge, "error", reason);
  challenge->state = final_state;
  if (callback) {
    std::move(callback).Run(ToolResult::Json(base::Value(MakeOutcome(
        "error", maybe_acted, reason, challenge->capture_ms, 0, total_ms,
        std::string()))));
  }
  if (IsLiveState(challenge->state)) StartCollect(challenge);
}

void CaptchaRegistry::RetireExecutor(
    std::unique_ptr<CaptchaExecutor>* slot) {
  if (!slot || !*slot) {
    return;
  }
  // Never destroy an executor from inside its own callback: retire it and
  // clear the list on the next task.
  retired_executors_.push_back(std::move(*slot));
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&CaptchaRegistry::ClearRetiredExecutors,
                                weak_factory_.GetWeakPtr()));
}

void CaptchaRegistry::ClearRetiredExecutors() {
  retired_executors_.clear();
}

void CaptchaRegistry::CancelChallenge(const std::string& challenge_id,
                                      ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto it = challenges_.find(challenge_id);
  if (it == challenges_.end()) {
    std::move(callback).Run(ToolResult::Error("challenge-not-found: " + challenge_id));
    return;
  }
  Challenge* challenge = it->second.get();
  if (IsBusyState(challenge->state)) {
    AbortSolve(challenge, "cancelled", "captured");
  }
  base::DictValue value;
  value.Set("ok", true);
  value.Set("state", challenge->state);
  std::move(callback).Run(ToolResult::Json(base::Value(std::move(value))));
}

void CaptchaRegistry::GetChallenge(const std::string& challenge_id,
                                   ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto it = challenges_.find(challenge_id);
  if (it == challenges_.end()) {
    std::move(callback).Run(ToolResult::Error("challenge-not-found: " + challenge_id));
    return;
  }
  Challenge& challenge = *it->second;
  if (!IsLiveState(challenge.state)) {
    std::move(callback).Run(ToolResult::Error("challenge-gone: " + challenge_id));
    return;
  }
  if (!challenge.collected || challenge.collect_in_flight) {
    std::move(callback).Run(ToolResult::Error("challenge-capturing: " + challenge_id));
    return;
  }
  base::DictValue images;
  for (const auto& [role, image] : challenge.collected->images) {
    images.Set(role,
               base::DictValue()
                   .Set("mime", image.mime)
                   .Set("b64", image.b64)
                   .Set("width", image.width)
                   .Set("height", image.height)
                   .Set("source", image.source));
  }
  base::DictValue geometry;
  for (const auto& [role, box] : challenge.collected->geometry) {
    geometry.Set(role, base::DictValue()
                           .Set("x", box.x)
                           .Set("y", box.y)
                           .Set("width", box.width)
                           .Set("height", box.height));
  }
  base::DictValue value;
  value.Set("challengeId", challenge.id);
  value.Set("targetId", challenge.target_id);
  value.Set("ruleId", challenge.rule_id);
  value.Set("type", "puzzle-slide");
  value.Set("stateRevision", static_cast<int>(challenge.state_revision));
  value.Set("capturedAt",
            static_cast<double>(challenge.captured_at_ms));
  value.Set("images", std::move(images));
  if (challenge.collected->instruction.has_value()) {
    value.Set("instruction", *challenge.collected->instruction);
  } else {
    value.Set("instruction", base::Value());
  }
  value.Set("geometry", std::move(geometry));
  value.Set("frameUrl", challenge.frame_url);
  std::move(callback).Run(ToolResult::Json(base::Value(std::move(value))));
}

// --- Events -----------------------------------------------------------------------

void CaptchaRegistry::EmitEvent(const std::string& type,
                                const Challenge& challenge,
                                const std::string& outcome,
                                const std::string& reason) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::DictValue event;
  event.Set("seq", static_cast<int>(next_event_seq_++));
  event.Set("at", static_cast<double>(
                      base::Time::Now().InMillisecondsSinceUnixEpoch()));
  event.Set("type", type);
  event.Set("challengeId", challenge.id);
  event.Set("targetId", challenge.target_id);
  event.Set("ruleId", challenge.rule_id);
  event.Set("kind", "puzzle-slide");
  if (!outcome.empty()) {
    event.Set("outcome", outcome);
  }
  if (!reason.empty()) {
    event.Set("reason", reason);
  }
  events_.push_back(std::move(event));
  while (events_.size() > kRingSize) {
    events_.pop_front();
    ++dropped_events_;
  }
  FlushWaiters();
}

void CaptchaRegistry::WaitForEvents(int cursor,
                                    base::TimeDelta wait,
                                    ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (cursor >= next_event_seq_) cursor = 0;
  const bool has_events = std::any_of(
      events_.begin(), events_.end(), [cursor](const base::DictValue& event) {
        return event.FindInt("seq").value_or(0) > cursor;
      });
  if (has_events || wait.is_zero() || waiters_.size() >= kMaxWaiters) {
    base::ListValue events;
    int64_t last = cursor;
    for (const base::DictValue& event : events_) {
      const int seq = event.FindInt("seq").value_or(0);
      if (seq > cursor) {
        events.Append(event.Clone());
        last = seq;
      }
    }
    base::DictValue value;
    value.Set("events", std::move(events));
    value.Set("cursor", static_cast<int>(last));
    const int oldest = events_.empty() ? static_cast<int>(next_event_seq_) :
        events_.front().FindInt("seq").value_or(0);
    const int dropped = static_cast<int>(std::max<int64_t>(0, oldest - cursor - 1));
    if (dropped) value.Set("dropped", dropped);
    value.Set("bootId", boot_id_);
    std::move(callback).Run(ToolResult::Json(base::Value(std::move(value))));
    return;
  }
  const int64_t waiter_id = next_waiter_id_++;
  auto waiter = std::make_unique<Waiter>(cursor);
  Waiter* raw = waiter.get();
  raw->callback = std::move(callback);
  waiters_.emplace(waiter_id, std::move(waiter));
  raw->timer.Start(FROM_HERE, wait,
                   base::BindOnce(&CaptchaRegistry::OnWaiterExpired,
                                  weak_factory_.GetWeakPtr(), waiter_id));
}

void CaptchaRegistry::FlushWaiters() {
  std::vector<int64_t> due;
  for (const auto& [id, waiter] : waiters_) {
    for (const base::DictValue& event : events_) {
      if (event.FindInt("seq").value_or(0) > waiter->cursor) {
        due.push_back(id);
        break;
      }
    }
  }
  for (const int64_t id : due) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CaptchaRegistry::DeliverWaiterPosted,
                                  weak_factory_.GetWeakPtr(), id));
  }
}

void CaptchaRegistry::DeliverWaiterPosted(int64_t waiter_id) {
  DeliverWaiter(waiter_id);
}

void CaptchaRegistry::DeliverWaiter(int64_t waiter_id) {
  auto it = waiters_.find(waiter_id);
  if (it == waiters_.end()) {
    return;
  }
  Waiter* waiter = it->second.get();
  if (waiter->cursor >= next_event_seq_) waiter->cursor = 0;
  waiter->timer.Stop();
  ToolCallback callback = std::move(waiter->callback);
  const int cursor = static_cast<int>(waiter->cursor);
  waiters_.erase(it);
  base::ListValue events;
  int64_t last = cursor;
  for (const base::DictValue& event : events_) {
    const int seq = event.FindInt("seq").value_or(0);
    if (seq > cursor) {
      events.Append(event.Clone());
      last = seq;
    }
  }
  base::DictValue value;
  value.Set("events", std::move(events));
  value.Set("cursor", static_cast<int>(last));
  value.Set("bootId", boot_id_);
  const int oldest = events_.empty() ? static_cast<int>(next_event_seq_)
                                     : events_.front().FindInt("seq").value_or(0);
  const int dropped = static_cast<int>(std::max<int64_t>(0, oldest - cursor - 1));
  if (dropped) value.Set("dropped", dropped);
  std::move(callback).Run(ToolResult::Json(base::Value(std::move(value))));
}

void CaptchaRegistry::OnWaiterExpired(int64_t waiter_id) {
  DeliverWaiter(waiter_id);
}

// --- Detach ------------------------------------------------------------------------

void CaptchaRegistry::DetachLater(content::WebContents* contents,
                                  CaptchaDetector* detector) {
  pending_detach_.emplace_back(contents, detector);
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CaptchaRegistry::DetachPending,
                     weak_factory_.GetWeakPtr()));
}

void CaptchaRegistry::DetachPending() {
  for (const auto& [contents, detector] : pending_detach_) {
    auto it = detectors_.find(contents);
    // The detector pointer is the token: an address reused by a fresh tab
    // keeps its (different) detector.
    if (it != detectors_.end() && it->second.get() == detector) {
      detectors_.erase(it);
    }
  }
  pending_detach_.clear();
}

// static
content::RenderFrameHost* CaptchaRegistry::FindFrame(
    content::WebContents* contents,
    const std::string& frame_key) {
  if (!contents) {
    return nullptr;
  }
  content::RenderFrameHost* found = nullptr;
  contents->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [&frame_key, &found](content::RenderFrameHost* frame) {
        if (!found && frame->IsRenderFrameLive() &&
            CaptchaDetector::FrameKey(frame) == frame_key) {
          found = frame;
        }
      });
  return found;
}

}  // namespace shuttle

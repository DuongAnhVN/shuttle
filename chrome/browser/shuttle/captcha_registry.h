// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_REGISTRY_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_REGISTRY_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/shuttle/captcha_collector.h"
#include "chrome/browser/shuttle/captcha_executor.h"
#include "chrome/browser/shuttle/captcha_rules.h"
#include "chrome/browser/shuttle/mcp_tools.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace shuttle {

class CaptchaDetector;
class ShuttleService;

// Browser-process singleton of the native CAPTCHA detection (the wire format
// is captcha-contract.md; the design and tool list live in
// tools/shuttle/docs/MCP.md). Owns:
//  - the applied rule snapshot and its revision (set-captcha-rules);
//  - one CaptchaDetector per observed tab (attached while MCP calls run for
//    the profile, dropped with the tab);
//  - the live challenges: {challengeId ch_<hex>, targetId, frame key, rule,
//    stateRevision, state, the collected data cache and its internal digest}
//    (the digest and the images never travel anywhere except out through
//    get-captcha-challenge);
//  - the event ring (256 entries, seq/cursor/dropped) and the waiters of
//    wait-captcha-events;
//  - the tab lock: while a challenge of a tab is in state solving, executing
//    or verifying, mutating page tools and session CDP input/navigation on
//    that tab answer captcha-in-progress (released on finish, cancel, tab
//    close and deadline).
// Everything here runs on the UI thread (the MCP server, the WebContents
// observers and the executor all do). Not yet compiled (written on a machine
// without a Chromium build).
class CaptchaRegistry {
 public:
  CaptchaRegistry(const CaptchaRegistry&) = delete;
  CaptchaRegistry& operator=(const CaptchaRegistry&) = delete;

  // The process-wide instance (lives to the end of the process; nothing binds
  // to it except deferred tasks through `weak_factory_`).
  static CaptchaRegistry& Get();

  CaptchaRegistry();
  ~CaptchaRegistry();

  // --- Rule snapshot (contract sections 1 and 2) -----------------------------

  struct SetOutcome {
    SetOutcome();
    SetOutcome(SetOutcome&&);
    SetOutcome& operator=(SetOutcome&&);
    ~SetOutcome();
    bool ok = false;
    int revision = 0;
    int applied = 0;                     // rules stored (enabled or not)
    std::string error;                   // "stale-revision" | "invalid-rules"
    base::ListValue details;             // [{ruleId?, error}, ...]
  };

  // Validates atomically; a rejected snapshot leaves the previous one in
  // place. Replacing a snapshot retires every live challenge that is not in
  // the middle of a solve (events say challenge-gone) and re-arms detectors.
  SetOutcome SetRules(const base::DictValue& args);

  // {revision, applied, enabledCount, lastError?}
  base::DictValue RulesStatusValue() const;

  // The applied snapshot (all rules, enabled flagged); pointers into it are
  // stable within one UI-thread task. Detectors run the URL gate themselves.
  const std::vector<CaptchaRule>& rules() const { return rules_; }
  bool HasEnabledRules() const { return enabled_count_ > 0; }

  // --- Tab observation ----------------------------------------------------

  // Attaches detectors to every leased tab of every session of `service`
  // (no-op without enabled rules). Called from ShuttleService for each MCP
  // request: the tabs an agent can use are its tabs.
  void ObserveProfile(ShuttleService* service);
  // Attaches the detector of `contents` if rules are enabled and runs a
  // first pass. False if there is nothing to observe.
  bool EnsureTabObserved(content::WebContents* contents);
  void ForgetTab(content::WebContents* contents);
  // probe-captcha: runs one detection pass through the tab's detector.
  // `done` is the detector's ProbeCallback (base::OnceCallback<void
  // (base::ListValue)>). Answers at once (frames: []) when rules are off.
  // Returns false without consuming `done` when no probe could start
  // (tab gone, over capacity): the caller answers "busy" then.
  bool ProbeTab(content::WebContents* contents,
                base::OnceCallback<void(base::ListValue)> done);

  // --- Tab lock (contract section 2, "Tab lock") ----------------------------

  // While any challenge of this tab is in state solving, executing or
  // verifying. Null `contents` reads as unlocked.
  bool IsTabLocked(content::WebContents* contents) const;
  // The locking state ("solving"/"executing"/"verifying"), "" when unlocked:
  // the refusal text asks for this.
  std::string LockStatusOf(content::WebContents* contents) const;

  // --- Notifications from the detector (UI thread) ----------------------------

  // One rule's result for one frame: state is none|candidate|ready|excluded,
  // widget_key the cheap DOM identity, missing the readiness gaps (may be
  // null).
  void OnWidgetState(content::WebContents* contents,
                     const std::string& frame_key,
                     const std::string& rule_id,
                     const std::string& state,
                     const std::string& widget_key,
                     const base::ListValue* missing);
  // The frame's document was replaced or the frame died: challenges of that
  // frame are gone (an active solve is cancelled with challenge-gone).
  void OnFrameGone(content::WebContents* contents,
                   const std::string& frame_key);
  // The tab's main document changed: every challenge of the tab is gone.
  void OnPageReset(content::WebContents* contents);
  // The tab is being destroyed: same as OnPageReset with tab-closed, and the
  // detector detaches itself (deferred: called from inside a notification).
  void OnTabGone(content::WebContents* contents, CaptchaDetector* detector);

  // --- Events (wait-captcha-events) ------------------------------------------

  // Answers at once with the buffered events after `cursor` when there are
  // any, else after `wait` (with an empty list). Cursor = last seq returned.
  void WaitForEvents(int cursor, base::TimeDelta wait, ToolCallback callback);

  // --- Challenges (get / solve / cancel) -------------------------------------

  void GetChallenge(const std::string& challenge_id, ToolCallback callback);
  // Validates and answers the contract's Outcome; `args` are the tool's
  // arguments. The solve refuses (outcome stale / error, acted false) when
  // the revision is older, the frame is gone, the fresh digest differs (no
  // input is sent in those cases) or another solve already runs.
  void SolveChallenge(const base::DictValue& args, ToolCallback callback);
  void CancelChallenge(const std::string& challenge_id,
                       ToolCallback callback);

 private:
  // One challenge instance: one (tab, frame, widget) of one rule.
  struct Challenge {
    explicit Challenge(content::WebContents* contents);
    Challenge(const Challenge&) = delete;
    Challenge& operator=(const Challenge&) = delete;
    ~Challenge();

    std::string id;                    // ch_<hex>, opaque
    std::string target_id;             // SessionManager target id of the tab
    raw_ptr<content::WebContents> contents = nullptr;
    std::string frame_key;             // child_id:routing_id at discovery
    std::string rule_id;
    CaptchaRule rule;                  // snapshot copy (binding, verify)
    std::string state = "detected";    // see the header of this file
    int64_t state_revision = 0;
    std::string widget_key;            // cheap DOM identity from the check
    std::string digest;                // internal, NEVER sent out
    int64_t captured_at_ms = 0;        // epoch ms of the last good capture
    int capture_ms = 0;
    std::string frame_url;
    std::optional<CollectedChallenge> collected;
    int miss_streak = 0;               // absent reads before "gone"
    bool collect_in_flight = false;
    uint64_t attempt = 0;
    // True once the solve attempt has been answered (verdict or abort): a
    // late executor callback is then ignored and never answers twice.
    bool solve_finished = false;
    bool retire_after_solve = false;
    std::string retire_reason;
    // Set while an abort drives the executor's Cancel(): when its callback
    // comes (sync inside Cancel() or later), OnSolved speaks the abort's
    // reason instead of the executor's verdict and ends the attempt in
    // `forced_state`.
    bool forced = false;
    std::string forced_reason;
    std::string forced_state;
    base::TimeTicks solve_started;
    base::OneShotTimer solve_watchdog;
    std::unique_ptr<CaptchaExecutor> executor;
    ToolCallback solve_callback;       // the in-flight solve's caller

    base::WeakPtrFactory<Challenge> weak_factory{this};  // must stay last
  };

  // Applies the digest/state bookkeeping and emits challenge-ready after a
  // collection answered.
  void OnCollected(base::WeakPtr<Challenge> challenge,
                   base::TimeTicks started,
                   base::expected<CollectedChallenge, CollectError> result);
  void OnRechecked(base::WeakPtr<Challenge> challenge,
                   uint64_t attempt,
                   double slide_x_proportion,
                   base::TimeDelta verify_timeout,
                   base::TimeDelta deadline,
                   RecheckResult rechecked);
  void OnSolved(base::WeakPtr<Challenge> challenge,
                uint64_t attempt,
                CaptchaExecResult result);
  void OnSolveDeadline(base::WeakPtr<Challenge> challenge, uint64_t attempt);
  void OnWaiterExpired(int64_t waiter_id);

  Challenge* FindLiveChallenge(content::WebContents* contents,
                               const std::string& frame_key,
                               const std::string& rule_id);
  // Reads the challenge picture through the collector (event order: the
  // challenge-ready event fires when the digest actually changed).
  void StartCollect(Challenge* challenge);
  // Marks gone (event included); a running solve is aborted with `reason`.
  void RetireChallenge(Challenge* challenge, const std::string& reason);
  void TrimChallenges();
  // Cancels a running executor (if any) and answers the solve caller with
  // `reason`; the challenge ends in `final_state` ("captured" to allow a
  // later attempt, "gone" to retire). Safe against executor reentrancy.
  void AbortSolve(Challenge* challenge,
                  const std::string& reason,
                  const std::string& final_state);
  // Moves the executor of `slot` to the retire list (never freed inside its
  // own callback) and schedules the clear.
  void RetireExecutor(std::unique_ptr<CaptchaExecutor>* slot);
  void ClearRetiredExecutors();

  // Builds and stores one ring event (and wakes waiters).
  void EmitEvent(const std::string& type,
                 const Challenge& challenge,
                 const std::string& outcome,
                 const std::string& reason);
  void FlushWaiters();
  void DeliverWaiter(int64_t waiter_id);
  void DeliverWaiterPosted(int64_t waiter_id);

  // Detaches a detector whose tab died (deferred out of the observer
  // callback; the detector pointer is the token so an address reuse cannot
  // remove a fresh entry).
  void DetachLater(content::WebContents* contents, CaptchaDetector* detector);
  void DetachPending();

  static content::RenderFrameHost* FindFrame(content::WebContents* contents,
                                             const std::string& frame_key);

  struct Waiter {
    explicit Waiter(int64_t at_cursor);
    ~Waiter();
    int64_t cursor;
    ToolCallback callback;
    base::OneShotTimer timer;
  };

  std::vector<CaptchaRule> rules_;
  std::string rules_canonical;         // identity for the revision check
  int revision_ = 0;
  int enabled_count_ = 0;
  std::string last_error;
  std::string boot_id_;

  std::map<content::WebContents*, std::unique_ptr<CaptchaDetector>> detectors_;
  std::vector<std::pair<content::WebContents*, CaptchaDetector*>>
      pending_detach_;

  std::map<std::string, std::unique_ptr<Challenge>> challenges_;

  std::deque<base::DictValue> events_;  // oldest first, <= kRingSize
  int64_t next_event_seq_ = 1;
  int64_t dropped_events_ = 0;
  std::map<int64_t, std::unique_ptr<Waiter>> waiters_;
  int64_t next_waiter_id_ = 1;

  // Executors whose callback ran inside their own method: destroyed on the
  // next task.
  std::vector<std::unique_ptr<CaptchaExecutor>> retired_executors_;

  // Must stay last.
  base::WeakPtrFactory<CaptchaRegistry> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_REGISTRY_H_

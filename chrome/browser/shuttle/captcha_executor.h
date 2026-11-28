// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_EXECUTOR_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_EXECUTOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace shuttle {

// Drives one puzzle-slide drag with TRUSTED input and no DevTools: the mouse
// events are blink::WebMouseEvent objects delivered through
// content::RenderWidgetHost::ForwardMouseEvent on the frame's widget host
// (the same mechanism content/browser/devtools/protocol/input_handler.cc
// uses, minus the CDP plumbing and minus the kFromDebugger modifier). The
// verdict is read by synchronous page scripts through
// RenderFrameHost::ExecuteJavaScriptInIsolatedWorld. Behaviour mirrors
// shuttle-manager captcha/executor.js; outcomes and reason slugs follow
// captcha-contract.md §2.
//
// The caller owns the object (UI thread). The Run() callback runs at most
// once and is posted to the UI task runner (never run inline), so a
// frame-deletion teardown cannot be re-entered through it; the destructor
// never runs it. The mouse button is ALWAYS released before the result is
// reported and on every early exit (error, Cancel(), deadline, frame death);
// in the destructor a still-open press is released best-effort if the widget
// is still reachable.
//
// Release events MUST carry button = kLeft: blink suppresses a mouse release
// whose button is kNoButton outright (EventHandler::HandleMouseReleaseEvent,
// third_party/blink/renderer/core/input/event_handler.cc:1269-1270, returns
// kHandledSuppressed), so no mouseup/pointerup would ever reach the page.
// The DOM `buttons` state is the modifier bit instead: kLeftButtonDown on the
// press and on dragged moves, cleared on the release (matching
// content/browser/devtools/protocol/input_handler.cc, which sets
// mouse_event->button = event_button for every CDP mouse event including
// mouseReleased with button "left", input_handler.cc:506/2040).
//
// Caveat: ForwardMouseEvent is a fire-and-forget IPC. If the widget drops an
// event (suspended/hidden renderer, discarded frame), the executor cannot
// detect it: `events` in `detail` counts sends, not deliveries, and a
// "verified" verdict can only be true when the page itself reported it.

// The rule's verdict conditions (contract §1 `success` / `failure`), flattened.
// An all-empty spec means "no verify": the run ends unconfirmed/no-verify.
struct CaptchaVerifySpec {
  CaptchaVerifySpec();
  CaptchaVerifySpec(const CaptchaVerifySpec&);
  CaptchaVerifySpec& operator=(const CaptchaVerifySpec&);
  CaptchaVerifySpec(CaptchaVerifySpec&&);
  CaptchaVerifySpec& operator=(CaptchaVerifySpec&&);
  ~CaptchaVerifySpec();
  std::string success_text;
  std::string success_selector;
  std::string success_url;
  std::string failure_text;
  std::string failure_selector;
};

// One slide gesture, CSS px in the FRAME's viewport (the coordinate space of
// CollectedChallenge geometry; the executor maps it to the widget's space).
struct CaptchaSlidePlan {
  double from_x = 0;
  double from_y = 0;
  double to_x = 0;
  double to_y = 0;
  int moves = 14;    // mouseMoved events between press and release
  int hold_ms = 120;  // held-down pause after the press, before the motion
};

struct CaptchaExecResult {
  CaptchaExecResult();
  CaptchaExecResult(const CaptchaExecResult&);
  CaptchaExecResult& operator=(const CaptchaExecResult&);
  CaptchaExecResult(CaptchaExecResult&&);
  CaptchaExecResult& operator=(CaptchaExecResult&&);
  ~CaptchaExecResult();
  // "verified" | "rejected" | "stale" | "expired" | "unconfirmed" |
  // "unsupported" | "error" (contract §2; the executor itself emits
  // verified/rejected/stale/unconfirmed/error).
  std::string outcome;
  bool acted = false;     // a mouseDown reached the widget
  bool verified = false;  // true ONLY for outcome "verified"
  std::string reason;     // contract §2 slug ("" for verified)
  int64_t execute_ms = 0;  // gesture time, press..release, not the verdict wait
  std::string detail;      // human-readable notes (events, polls, href, errors)
};

class CaptchaExecutor final : public content::WebContentsObserver {
 public:
  // Both pointers may be null: Run() then answers outcome "error" without
  // touching anything. UI thread.
  CaptchaExecutor(content::WebContents* contents,
                  content::RenderFrameHost* frame);
  CaptchaExecutor(const CaptchaExecutor&) = delete;
  CaptchaExecutor& operator=(const CaptchaExecutor&) = delete;
  ~CaptchaExecutor() override;

  // Runs the drag, then polls the frame for the verdict (<=250 ms ticks,
  // failure checked before success). Hard stop = min(`deadline`, 15 s from
  // Run()); a watchdog timer enforces it even while a page-script reply is
  // outstanding. `verify_timeout` bounds the verdict wait inside that stop.
  // A deadline already passed at Run() answers stale/acted=false and sends
  // no input. Called after Cancel() before the first Run(): answers
  // error/cancelled immediately and sends no input.
  void Run(const CaptchaSlidePlan& plan,
           const CaptchaVerifySpec& verify,
           base::TimeDelta verify_timeout,
           base::TimeTicks deadline,
           base::OnceCallback<void(CaptchaExecResult)> callback);

  // Aborts a run in flight: releases an open press, then answers
  // unconfirmed/cancelled when input was sent, error/cancelled otherwise.
  // No-op once finished. Called before Run() it marks the executor, and the
  // next Run() answers error/cancelled without touching the page.
  void Cancel(const std::string& reason);

 private:
  // One step of the gesture schedule.
  struct GestureAction {
    enum class Kind { kMove, kDown, kUp };
    Kind kind = Kind::kMove;
    double css_x = 0;
    double css_y = 0;
    base::TimeDelta delay;  // pause BEFORE this action is delivered
  };

  void StartGesture();
  void OnFrameOffset(base::Value value);
  void Tick();
  void DeadlineExceeded();
  void OnWatchdog();
  void OnVerifyDeadline();
  void StartVerify();
  void PollVerify();
  void ScheduleNextPoll();
  void OnVerifyResult(base::Value value);
  bool SendMouseEvent(GestureAction::Kind kind, bool button_down, double css_x,
                      double css_y, int click_count);
  void ReleasePressed();

  void Finish(const std::string& outcome, const std::string& reason,
              const std::string& detail);
  void DeliverResult();  // runs the posted result callback, if any

  // content::WebContentsObserver
  void RenderFrameDeleted(content::RenderFrameHost* render_frame_host) override;
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void WebContentsDestroyed() override;

  CaptchaSlidePlan plan_;
  CaptchaVerifySpec verify_;

  raw_ptr<content::RenderFrameHost> frame_;
  // Survives an RFH swap: the FrameTreeNode outlives its RenderFrameHost
  // (content/public/browser/frame_tree_node_id.h:13-33), so after acted_ the
  // verdict poll re-finds the frame's current RFH after a cross-process
  // navigation (WebContents::UnsafeFindFrameByFrameTreeNodeId,
  // content/public/browser/web_contents.h:586-594).
  content::FrameTreeNodeId frame_node_id_;
  int frame_misses_ = 0;
  // Cumulative iframe offset (CSS px, frame viewport -> the widget's local
  // root viewport); 0 for the main frame and for out-of-process frames, whose
  // guest widget origin IS the frame viewport origin.
  double offset_x_ = 0;
  double offset_y_ = 0;
  // True until the in-process iframe offset is known (main frame / OOPIF skip
  // that lookup).
  bool offset_ready_ = false;
  // Scale applied to CSS px to reach widget coordinates (page zoom; pinch and
  // CSS zoom are treated as 1 - see the .cc note).
  double widget_scale_ = 1.0;

  bool started_ = false;
  bool finished_ = false;
  bool verify_phase_ = false;
  bool cancel_before_run_ = false;
  std::string pre_cancel_reason_;
  bool acted_ = false;
  bool press_open_ = false;

  std::vector<GestureAction> actions_;
  size_t action_index_ = 0;

  // Coordinates of the last delivered event (widget space inputs, kept in
  // frame CSS px so a release lands where the drag stopped).
  double last_x_ = 0;
  double last_y_ = 0;

  base::TimeTicks t0_;
  base::TimeTicks hard_stop_;
  base::TimeTicks verify_end_;
  base::TimeDelta verify_timeout_;
  base::TimeTicks press_at_;  // when the first mouseDown was delivered
  int64_t execute_ms_ = 0;
  bool execute_measured_ = false;
  int events_sent_ = 0;
  int verify_polls_ = 0;
  int verify_errors_ = 0;
  bool verify_first_poll_done_ = false;

  bool have_verify_ = false;

  CaptchaExecResult pending_result_;
  bool has_pending_result_ = false;

  base::OnceCallback<void(CaptchaExecResult)> callback_;

  // The step/poll scheduler and the two independent enforcement timers: the
  // hard-stop watchdog covers the window where a page-script reply is
  // outstanding (no other timer runs then), verify_deadline_ ends the verdict
  // wait on time even while a poll is in flight.
  base::OneShotTimer timer_;
  base::OneShotTimer watchdog_;
  base::OneShotTimer verify_deadline_;
  base::WeakPtrFactory<CaptchaExecutor> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_EXECUTOR_H_

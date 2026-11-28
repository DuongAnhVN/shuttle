// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Trusted slide input without DevTools.
//
// The mouse events are plain blink::WebMouseEvent objects delivered with
// content::RenderWidgetHost::ForwardMouseEvent (content/public/browser/
// render_widget_host.h:183) on the widget host of the frame,
// frame->GetRenderWidgetHost() (render_frame_host.h:321-329). This mirrors
// content/browser/devtools/protocol/input_handler.cc, which builds events the
// same way (constructor third_party/blink/public/common/input/
// web_mouse_event.h:74; click_count field line 25; button and position
// through WebPointerProperties, third_party/blink/public/common/input/
// web_pointer_properties.h:41-72) and injects them through the same
// ForwardMouseEvent call (input_handler.cc:767) - with these deliberate
// differences:
// - no blink::WebInputEvent::kFromDebugger modifier (browser-injected trusted
//   input, not debugger input);
// - the DOM buttons bit follows W3C exactly: kLeftButtonDown on the press and
//   on dragged moves, cleared on the release (web_input_event.h:99, the
//   correction blink's UpdateEventModifiersToMatchButton documents,
//   web_mouse_event.h:111-119). The `button` field stays kLeft on press,
//   pressed moves AND release, because blink suppresses a release whose
//   button is kNoButton outright (EventHandler::HandleMouseReleaseEvent,
//   third_party/blink/renderer/core/input/event_handler.cc:1269-1270).
//   DevTools likewise sets mouse_event->button from the CDP button for every
//   event type (input_handler.cc:506 and :2040 - the release of a real CDP
//   drag carries button "left").
// - position_in_screen = position_in_widget + the target view's bounds origin
//   in screen DIP, the same conversion DevTools applies through
//   ConvertWidgetPointToScreenPoint (input_handler.cc:635-642 used at :2041)
//   instead of the debugger shortcut at :516, so screenX differs from clientX
//   like it does for a real pointer. GetViewBounds() is documented "in screen
//   coordinates" (render_widget_host_view.h:154-155), in DIP on the platforms
//   Shuttle builds for; widget positions are DIP too (see below), so the
//   addition is unit-consistent.
//
// Coordinate spaces (verified in this tree):
// - WebMouseEvent position_in_widget is the widget's own coordinate space,
//   relative to the RenderWidget bounds, in DIP, without DevTools emulation
//   or overscroll (web_pointer_properties.h:116-119). DevTools converts CSS px
//   -> widget by multiplying by the zoom factor (input_handler.cc:65-67 +
//   ScaleFactor() lines 1561-1584).
// - The target widget is the frame's own RenderWidgetHost. For the main frame
//   and out-of-process frames the widget origin IS the frame viewport, so the
//   plan's frame-viewport CSS px need only the zoom multiply. A same-process
//   child frame shares the root widget, so its iframe-chain offset (including
//   the iframe element's client border and padding) is first read from the
//   page (the /*captcha-frame-offset*/ script, the same one the collector
//   uses).
// - RenderWidgetHostView::TransformPointToRootCoordSpaceF
//   (render_widget_host_view.h:109-122) maps a child view point into ROOT
//   widget space; it is deliberately not used for delivery: events go to the
//   frame's own widget host, the same way content's InputHandler injects them
//   for frame targets.
// Known limits: pinch page-scale and CSS zoom applied to the embedding
// element are content/browser internals (Page::GetPageScaleFactor,
// RenderWidgetHostViewBase::GetCSSZoomFactor), not public chrome/browser API,
// so they are treated as 1 here. Captcha widgets essentially never use
// either; a wrong scale would only shift the gesture proportionally.
//
// Frame survival: a FrameTreeNode outlives its RenderFrameHost across a
// cross-process navigation (content/public/browser/frame_tree_node_id.h:13-
// 33). Once acted_, deleting our RFH does not end the run: the poll re-finds
// the frame's current RFH through WebContents::UnsafeFindFrameByFrameTreeNode
// Id (web_contents.h:586-594) and keeps reading the verdict, so a solve that
// navigates the frame cross-site can still match `success_url`. Before acted_
// the run ends with the frame, as before.

#include "chrome/browser/shuttle/captcha_executor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/string_escape.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"

namespace shuttle {
namespace {

// Mirrors Manager captcha/executor.js and mcp/pointer.js.
constexpr base::TimeDelta kStepDelay = base::Milliseconds(12);
constexpr base::TimeDelta kMaxRunTime = base::Seconds(15);  // POINTER cap
constexpr base::TimeDelta kVerifyPoll = base::Milliseconds(200);
constexpr int kMaxVerifyErrors = 3;
constexpr double kCoordLimit = 16384.0;
constexpr int kMaxMoves = 200;
constexpr int kMaxHoldMs = 5000;

// The same isolated world the collector reads the page from:
// content::ISOLATED_WORLD_ID_CONTENT_END (= 1), justified in detail in
// captcha_collector.cc (it is the only id in the API's legal 1..11 range
// nothing else can claim); it must stay identical across the two files.
constexpr int32_t kShuttleCaptchaWorldId =
    content::ISOLATED_WORLD_ID_CONTENT_END;

// Text a widget uses when its challenge went away (Manager EXPIRY_MARKERS;
// the verdict text from the page script is already lower-cased).
constexpr const char* kExpiryMarkers[] = {"expired", "timed out", "time out",
                                          "try again", "session over",
                                          "reload"};

std::string JsonQuote(std::string_view value) {
  return base::GetQuotedJSONString(value);
}

// JS smoothstep, the same curve as Manager executor.js easeInOut / pointer.js.
double EaseInOut(double t) {
  return t * t * (3.0 - 2.0 * t);
}

bool CoordsOk(double x, double y) {
  return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= kCoordLimit &&
         std::abs(y) <= kCoordLimit;
}

// The page's own verdict report, re-read on every poll so a navigation in
// between is survived: every poll sends a fresh script against whatever
// document is there now (Manager verifyScript, semantics copied verbatim;
// conditions are data - JSON string literals - never code). The caller
// checks failure before success: a page that reports both has not been
// passed.
std::string BuildVerifyScript(const CaptchaVerifySpec& verify) {
  std::string success = "{";
  bool first = true;
  for (const auto& field : {
           std::pair{"text", &verify.success_text},
           std::pair{"selector", &verify.success_selector},
           std::pair{"url", &verify.success_url},
       }) {
    if (field.second->empty()) {
      continue;
    }
    if (!first) {
      success += ",";
    }
    first = false;
    success += JsonQuote(field.first) + ":" + JsonQuote(*field.second);
  }
  success += "}";
  std::string failure = "{";
  first = true;
  for (const auto& field : {
           std::pair{"text", &verify.failure_text},
           std::pair{"selector", &verify.failure_selector},
       }) {
    if (field.second->empty()) {
      continue;
    }
    if (!first) {
      failure += ",";
    }
    first = false;
    failure += JsonQuote(field.first) + ":" + JsonQuote(*field.second);
  }
  failure += "}";

  const std::string conditions = "{\"success\":" +
                                 (success == "{}" ? std::string("null")
                                                  : success) +
                                 ",\"failure\":" +
                                 (failure == "{}" ? std::string("null")
                                                  : failure) +
                                 "}";
  return "(() => { /*captcha-verify*/\n  const V = " + conditions + R"JS(;
  const norm = (s) => String(s || '').replace(/\s+/g, ' ').toLowerCase();
  const text = norm(document.body ? document.body.innerText : '').slice(0, 40000);
  const visible = (sel) => {
    let el = null;
    try { el = document.querySelector(sel); } catch (e) { return false; }
    if (!el) return false;
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return false;
    const cs = getComputedStyle(el);
    return cs.display !== 'none' && cs.visibility !== 'hidden';
  };
  const hit = (c) => {
    if (!c) return false;
    if (c.text && text.indexOf(norm(c.text)) >= 0) return true;
    if (c.selector && visible(c.selector)) return true;
    if (c.url && String(location.href).indexOf(c.url) >= 0) return true;
    return false;
  };
  return { ok: true, href: String(location.href).slice(0, 300), success: hit(V.success), failure: hit(V.failure), text };
})())JS";
}

// The same /*captcha-frame-offset*/ read the collector makes: the frame's
// viewport origin inside its shared widget's viewport. `depth` is the
// same-widget ancestor chain length, computed browser-side; the walk stops
// (returns null) when an embedding frame is cross-origin and its
// frameElement unreadable. The child viewport starts at the iframe element's
// CONTENT box, so the border (clientLeft/clientTop) and the padding add to
// the border-box rect.
std::string FrameOffsetScript(int depth) {
  return "(() => { /*captcha-frame-offset*/\n"
         "  const DEPTH = " +
         std::to_string(depth) +
         ";\n"
         "  let x = 0;\n"
         "  let y = 0;\n"
         "  try {\n"
         "    let w = window;\n"
         "    for (let i = 0; i < DEPTH; i++) {\n"
         "      if (w === w.top) return { x, y };\n"
         "      const fe = w.frameElement;\n"
         "      if (!fe) return null;\n"
         "      const r = fe.getBoundingClientRect();\n"
         "      const cs = getComputedStyle(fe);\n"
         "      x += r.left + (fe.clientLeft || 0) + (parseFloat(cs.paddingLeft) || 0);\n"
         "      y += r.top + (fe.clientTop || 0) + (parseFloat(cs.paddingTop) || 0);\n"
         "      w = w.parent;\n"
         "    }\n"
         "  } catch (e) { return null; }\n"
         "  return { x, y };\n"
         "})()";
}

bool MentionsExpiry(std::string_view lower_text) {
  for (const char* marker : kExpiryMarkers) {
    if (lower_text.find(marker) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

CaptchaVerifySpec::CaptchaVerifySpec() = default;
CaptchaVerifySpec::CaptchaVerifySpec(const CaptchaVerifySpec&) = default;
CaptchaVerifySpec& CaptchaVerifySpec::operator=(const CaptchaVerifySpec&) = default;
CaptchaVerifySpec::CaptchaVerifySpec(CaptchaVerifySpec&&) = default;
CaptchaVerifySpec& CaptchaVerifySpec::operator=(CaptchaVerifySpec&&) = default;
CaptchaVerifySpec::~CaptchaVerifySpec() = default;

CaptchaExecResult::CaptchaExecResult() = default;
CaptchaExecResult::CaptchaExecResult(const CaptchaExecResult&) = default;
CaptchaExecResult& CaptchaExecResult::operator=(const CaptchaExecResult&) = default;
CaptchaExecResult::CaptchaExecResult(CaptchaExecResult&&) = default;
CaptchaExecResult& CaptchaExecResult::operator=(CaptchaExecResult&&) = default;
CaptchaExecResult::~CaptchaExecResult() = default;

CaptchaExecutor::CaptchaExecutor(content::WebContents* contents,
                                 content::RenderFrameHost* frame)
    : content::WebContentsObserver(
          contents ? contents
                   : (frame ? content::WebContents::FromRenderFrameHost(frame)
                            : nullptr)),
      frame_(frame) {
  if (frame_) {
    frame_node_id_ = frame_->GetFrameTreeNodeId();
  }
}

CaptchaExecutor::~CaptchaExecutor() {
  // The guarantee, even on teardown: a press we opened is released if the
  // widget is still reachable (if the frame is already gone its renderer
  // died with the button state).
  timer_.Stop();
  watchdog_.Stop();
  verify_deadline_.Stop();
  ReleasePressed();
  // The Run() callback is deliberately NOT run after destruction: the posted
  // DeliverResult is weak-bound and dies with this object, and callback_ is
  // dropped here.
  callback_ = base::OnceCallback<void(CaptchaExecResult)>();
  has_pending_result_ = false;
}

void CaptchaExecutor::Run(
    const CaptchaSlidePlan& plan,
    const CaptchaVerifySpec& verify,
    base::TimeDelta verify_timeout,
    base::TimeTicks deadline,
    base::OnceCallback<void(CaptchaExecResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (started_) {
    CaptchaExecResult result;
    result.outcome = "error";
    result.reason = "execute-failed";
    result.detail = "this executor has already run";
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::move(result)));
    return;
  }
  started_ = true;
  callback_ = std::move(callback);
  plan_ = plan;
  verify_ = verify;
  verify_timeout_ = verify_timeout;
  t0_ = base::TimeTicks::Now();
  // Hard cap = min(deadline, 15 s) (Manager: Math.min(deadlineAt, t0 +
  // POINTER_TIME_LIMIT_MS)). A null deadline means "not given".
  const base::TimeTicks cap = t0_ + kMaxRunTime;
  hard_stop_ = (!deadline.is_null() && deadline < cap) ? deadline : cap;
  // Watchdog: the hard stop stays true even when the next continuation is a
  // page-script reply that never arrives (finding: no timer is armed while a
  // JS reply is outstanding).
  watchdog_.Start(FROM_HERE, hard_stop_ - t0_, this,
                  &CaptchaExecutor::OnWatchdog);
  if (cancel_before_run_) {
    // Cancel() arrived before the first Run(): honour it, touch nothing.
    Finish("error", "cancelled",
           base::StrCat({"cancelled before Run(): ", pre_cancel_reason_}));
    return;
  }

  if (!frame_ || !web_contents() || !frame_->IsRenderFrameLive()) {
    Finish("error", "execute-failed", "no live frame for this executor");
    return;
  }
  // The whole plan is checked before anything is sent: a refused plan moves
  // no mouse (Manager planProblem; its reason 'bad-plan' is kept).
  if (!CoordsOk(plan_.from_x, plan_.from_y) ||
      !CoordsOk(plan_.to_x, plan_.to_y)) {
    Finish("error", "bad-plan",
           "a plan point is not a finite viewport coordinate within 16384");
    return;
  }
  if (plan_.moves < 1 || plan_.moves > kMaxMoves) {
    Finish("error", "bad-plan",
           base::StrCat({"moves ", base::NumberToString(plan_.moves),
                         " is outside 1..",
                         base::NumberToString(kMaxMoves)}));
    return;
  }
  if (plan_.hold_ms < 0 || plan_.hold_ms > kMaxHoldMs) {
    Finish("error", "bad-plan", "hold_ms is outside 0..5000");
    return;
  }
  if (!deadline.is_null() && base::TimeTicks::Now() >= deadline) {
    Finish("stale", "deadline-exceeded",
           base::StrCat({"the deadline had passed ",
                         base::NumberToString((base::TimeTicks::Now() -
                                               deadline)
                                                  .InMillisecondsF()),
                         " ms before Run()"}));
    return;
  }

  // Map the frame-viewport coordinate space onto the target widget (see the
  // file note): main frame and out-of-process guest frames need no offset; a
  // same-process child first reads the iframe-chain offset from the page.
  content::RenderFrameHost* widget_root = frame_;
  int depth = 0;
  while (content::RenderFrameHost* parent = widget_root->GetParent()) {
    if (parent->GetRenderWidgetHost() != frame_->GetRenderWidgetHost()) {
      break;  // the frame is the root of its own (guest) widget
    }
    ++depth;
    widget_root = parent;
  }
  content::RenderFrameHost* frame_ptr = frame_;
  content::RenderFrameHost* zoom_frame = depth == 0 ? frame_ptr : widget_root;
  const double zoom_factor = blink::ZoomLevelToZoomFactor(
      content::HostZoomMap::GetZoomLevel(web_contents(),
                                         zoom_frame->GetGlobalId()));
  widget_scale_ =
      std::isfinite(zoom_factor) && zoom_factor > 0 ? zoom_factor : 1.0;

  if (depth == 0) {
    offset_ready_ = true;
    StartGesture();
    return;
  }
  frame_->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(FrameOffsetScript(depth)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&CaptchaExecutor::OnFrameOffset,
                         weak_factory_.GetWeakPtr()),
          base::Value()),
      kShuttleCaptchaWorldId);
}

void CaptchaExecutor::OnFrameOffset(base::Value value) {
  if (!started_ || finished_) {
    return;
  }
  if (!frame_) {  // the reply arrived during/after the frame teardown
    Finish("error", "execute-failed",
           "the frame died before the gesture began");
    return;
  }
  const base::DictValue* dict = value.GetIfDict();
  const std::optional<double> x = dict ? dict->FindDouble("x") : std::nullopt;
  const std::optional<double> y = dict ? dict->FindDouble("y") : std::nullopt;
  if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
    // A same-process child frame whose embedding chain is cross-origin: its
    // place in the shared widget is unknown, so NO input is sent (sending at
    // an assumed offset would act on the wrong element).
    Finish("error", "execute-failed",
           "the iframe offset of this same-process frame is not readable "
           "(cross-origin embedding); no input was sent");
    return;
  }
  offset_x_ = *x;
  offset_y_ = *y;
  offset_ready_ = true;
  StartGesture();
}

void CaptchaExecutor::StartGesture() {
  if (finished_) {
    return;
  }
  // move to the handle, press, hold, eased moves (12 ms apart), release at
  // the target - the Manager drag step, verbatim in event order.
  actions_.clear();
  action_index_ = 0;
  actions_.push_back({GestureAction::Kind::kMove, plan_.from_x, plan_.from_y,
                      base::TimeDelta()});
  actions_.push_back({GestureAction::Kind::kDown, plan_.from_x, plan_.from_y,
                      base::TimeDelta()});
  for (int i = 1; i <= plan_.moves; ++i) {
    const double t = EaseInOut(static_cast<double>(i) / plan_.moves);
    double x = plan_.from_x + (plan_.to_x - plan_.from_x) * t;
    double y = plan_.from_y + (plan_.to_y - plan_.from_y) * t;
    if (i == plan_.moves) {
      x = plan_.to_x;  // the last step lands exactly on the target
      y = plan_.to_y;
    }
    // The first move waits out the hold; later moves 12 ms; the release
    // follows the last move at once (Manager moveTo()/mouseReleased order).
    actions_.push_back(
        {GestureAction::Kind::kMove, x, y,
         i == 1 ? base::Milliseconds(plan_.hold_ms) : kStepDelay});
  }
  actions_.push_back({GestureAction::Kind::kUp, plan_.to_x, plan_.to_y,
                      base::TimeDelta()});
  timer_.Start(FROM_HERE, base::TimeDelta(), this, &CaptchaExecutor::Tick);
}

void CaptchaExecutor::Tick() {
  if (!started_ || finished_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (now >= hard_stop_) {
    DeadlineExceeded();
    return;
  }
  const GestureAction action = actions_[action_index_];
  switch (action.kind) {
    case GestureAction::Kind::kMove: {
      if (!SendMouseEvent(action.kind, press_open_, action.css_x, action.css_y,
                          0)) {
        if (acted_) {
          // The frame was replaced (cross-process navigation) mid-drag after
          // the press: stop sending, release state, and let the verdict poll
          // follow the new document (file note; finding 6).
          press_open_ = false;
          execute_ms_ = (now - press_at_).InMilliseconds();
          execute_measured_ = true;
          StartVerify();
          return;
        }
        Finish("error", "execute-failed",
               base::StrCat({"the frame widget died mid-gesture (events=",
                             base::NumberToString(events_sent_), ")"}));
        return;
      }
      break;
    }
    case GestureAction::Kind::kDown: {
      if (!SendMouseEvent(action.kind, /*button_down=*/true, action.css_x,
                          action.css_y, 1)) {
        Finish("error", "execute-failed",
               "the frame widget died before the press was delivered");
        return;
      }
      // acted=true from the first delivered mouseDown (contract).
      acted_ = true;
      press_open_ = true;
      press_at_ = base::TimeTicks::Now();
      break;
    }
    case GestureAction::Kind::kUp: {
      SendMouseEvent(action.kind, /*button_down=*/false, action.css_x,
                     action.css_y, 1);
      press_open_ = false;  // sent or not, we are past the schedule
      break;
    }
  }
  ++action_index_;
  if (action_index_ >= actions_.size()) {
    // execute_ms is the gesture itself: press delivered .. release delivered
    // (header contract), not the run since Run().
    execute_ms_ = (base::TimeTicks::Now() - press_at_).InMilliseconds();
    execute_measured_ = true;
    StartVerify();
    return;
  }
  const base::TimeDelta delay = actions_[action_index_].delay;
  // Never sleep past the hard stop (Manager: gap = min(delay, left())).
  const base::TimeDelta left = hard_stop_ - base::TimeTicks::Now();
  timer_.Start(FROM_HERE, std::min(delay, std::max(base::TimeDelta(), left)),
               this, &CaptchaExecutor::Tick);
}

void CaptchaExecutor::DeadlineExceeded() {
  const std::string detail =
      base::StrCat({"events=", base::NumberToString(events_sent_)});
  if (acted_) {
    // The page was touched and the schedule never reached its release:
    // Finish() releases the button, the verdict is simply unknown
    // (Manager: acted ? unconfirmed : stale, both deadline-exceeded).
    Finish("unconfirmed", "deadline-exceeded", detail);
  } else {
    Finish("stale", "deadline-exceeded", detail);
  }
}

void CaptchaExecutor::OnWatchdog() {
  if (!started_ || finished_) {
    return;
  }
  if (verify_phase_) {
    // Hard stop reached while a verdict poll was outstanding.
    Finish("unconfirmed", "verify-timeout",
           base::StrCat({"events=", base::NumberToString(events_sent_),
                         " polls=", base::NumberToString(verify_polls_),
                         " capped=1 watchdog=1"}));
    return;
  }
  DeadlineExceeded();
}

void CaptchaExecutor::OnVerifyDeadline() {
  if (!started_ || finished_ || !verify_phase_) {
    return;
  }
  // The verdict window closed while a poll reply was outstanding; the reply
  // (WrapCallbackWithDefaultInvokeIfNotRun) will arrive too late and be
  // ignored by the finished_ guard.
  Finish("unconfirmed", "verify-timeout",
         base::StrCat({"events=", base::NumberToString(events_sent_),
                       " polls=", base::NumberToString(verify_polls_),
                       " in-flight=1"}));
}

void CaptchaExecutor::StartVerify() {
  if (finished_ || verify_phase_) {
    return;
  }
  timer_.Stop();
  have_verify_ =
      !verify_.success_text.empty() || !verify_.success_selector.empty() ||
      !verify_.success_url.empty() || !verify_.failure_text.empty() ||
      !verify_.failure_selector.empty();
  if (!have_verify_) {
    Finish("unconfirmed", "no-verify",
           base::StrCat({"events=", base::NumberToString(events_sent_)}));
    return;
  }
  // The verdict window: min(caller's verify_timeout, remaining hard cap).
  verify_phase_ = true;
  verify_end_ = std::min(base::TimeTicks::Now() +
                             std::max(base::TimeDelta(), verify_timeout_),
                         hard_stop_);
  verify_deadline_.Start(FROM_HERE, verify_end_ - base::TimeTicks::Now(), this,
                         &CaptchaExecutor::OnVerifyDeadline);
  PollVerify();
}

void CaptchaExecutor::PollVerify() {
  if (!started_ || finished_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  // Manager polls in a do-while: the verdict is read at least once even with
  // a zero window, and the expiry is decided after that read.
  if (now >= verify_end_ && verify_first_poll_done_) {
    const bool capped = verify_end_ >= hard_stop_;
    Finish("unconfirmed", "verify-timeout",
           base::StrCat({"events=", base::NumberToString(events_sent_),
                         " polls=", base::NumberToString(verify_polls_),
                         capped ? " capped=1" : ""}));
    return;
  }
  if (!frame_) {
    // The acting frame was replaced by a navigation: re-find the node's
    // current RFH and keep polling it (the verdict script needs no
    // coordinates; the gesture is over). A node with no live RFH yet waits
    // for RenderFrameHostChanged or the next poll; kMaxVerifyErrors misses
    // end the run like a page that cannot be read.
    content::WebContents* wc = web_contents();
    content::RenderFrameHost* replacement =
        (wc && frame_node_id_)
            ? wc->UnsafeFindFrameByFrameTreeNodeId(frame_node_id_)
            : nullptr;
    if (replacement && replacement->IsActive() &&
        replacement->IsRenderFrameLive()) {
      // RenderFrameHost::IsActive() is the documented-safe liveness guard
      // (render_frame_host.h:807-822); ExecuteJavaScriptInIsolatedWorld
      // CHECKs a committed frame (render_frame_host_impl.cc AssertFrameWas-
      // Committed), so never adopt a speculative/pending-commit/BFCached host.
      frame_ = replacement;
      frame_misses_ = 0;
      // Coordinates for further input are not needed any more (the press was
      // already released or died with its widget); do not trust the stale
      // mapping for anything beyond that.
      offset_ready_ = false;
    } else if (++frame_misses_ >= kMaxVerifyErrors) {
      Finish("error", "execute-failed",
             base::StrCat({"the frame could not be re-found after its "
                           "deletion (misses=",
                           base::NumberToString(frame_misses_),
                           ", polls=", base::NumberToString(verify_polls_),
                           ")"}));
      return;
    } else {
      ScheduleNextPoll();  // wait for the swap to land
      return;
    }
  }
  verify_first_poll_done_ = true;
  frame_->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildVerifyScript(verify_)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&CaptchaExecutor::OnVerifyResult,
                         weak_factory_.GetWeakPtr()),
          base::Value()),
      kShuttleCaptchaWorldId);
}

void CaptchaExecutor::ScheduleNextPoll() {
  // <=250 ms ticks (Manager DEFAULT_VERIFY_POLL_MS = 200), never past the
  // verdict window; the next PollVerify turns an expiry into the final
  // unconfirmed/verify-timeout answer.
  const base::TimeDelta gap =
      std::min(kVerifyPoll, verify_end_ - base::TimeTicks::Now());
  timer_.Start(FROM_HERE, std::max(base::TimeDelta(), gap), this,
               &CaptchaExecutor::PollVerify);
}

void CaptchaExecutor::OnVerifyResult(base::Value value) {
  if (!started_ || finished_) {
    return;
  }
  const base::DictValue* dict = value.GetIfDict();
  const bool ok = dict && dict->FindBool("ok").value_or(false);
  if (!ok) {
    // A failed read: a navigation may cost one, a dead frame costs three
    // (Manager MAX_VERIFY_ERRORS). The next poll is scheduled regardless;
    // PollVerify re-resolves a replaced frame, the watchdog ends the run if
    // nothing ever replies.
    if (++verify_errors_ >= kMaxVerifyErrors) {
      Finish("error", "execute-failed",
             base::StrCat({"the page could not be read for the verdict (",
                           base::NumberToString(verify_errors_),
                           " consecutive reads failed; polls=",
                           base::NumberToString(verify_polls_), ")"}));
      return;
    }
    ScheduleNextPoll();
    return;
  }
  verify_errors_ = 0;
  ++verify_polls_;
  const bool failure = dict->FindBool("failure").value_or(false);
  const bool success = dict->FindBool("success").value_or(false);
  const std::string text =
      dict->FindString("text") ? *dict->FindString("text") : std::string();
  const std::string href =
      dict->FindString("href") ? *dict->FindString("href") : std::string();
  // Failure is checked FIRST: a page reporting both has not been passed.
  if (failure) {
    Finish("rejected", "verify-failure",
           base::StrCat({"events=", base::NumberToString(events_sent_),
                         " polls=", base::NumberToString(verify_polls_),
                         MentionsExpiry(text) ? " page-reported-expiry=1" : "",
                         " href=", href}));
    return;
  }
  if (success) {
    Finish("verified", "",
           base::StrCat({"events=", base::NumberToString(events_sent_),
                         " polls=", base::NumberToString(verify_polls_),
                         " href=", href}));
    return;
  }
  ScheduleNextPoll();
}

bool CaptchaExecutor::SendMouseEvent(GestureAction::Kind kind,
                                     bool button_down,
                                     double css_x,
                                     double css_y,
                                     int click_count) {
  if (!frame_ || !offset_ready_) {
    return false;
  }
  content::RenderWidgetHost* widget = frame_->GetRenderWidgetHost();
  if (!widget) {
    return false;
  }
  blink::WebInputEvent::Type type = blink::WebInputEvent::Type::kMouseMove;
  if (kind == GestureAction::Kind::kDown) {
    type = blink::WebInputEvent::Type::kMouseDown;
  } else if (kind == GestureAction::Kind::kUp) {
    type = blink::WebInputEvent::Type::kMouseUp;
  }
  blink::WebMouseEvent event(type, blink::WebInputEvent::kNoModifiers,
                             base::TimeTicks::Now());
  // `button` and the buttons modifier are separate states (file note): the
  // release must carry kLeft or blink suppresses it as
  // kHandledSuppressed (event_handler.cc:1269-1270); only the modifier is
  // cleared for the release.
  const bool carries_left = button_down || kind == GestureAction::Kind::kUp;
  event.button = carries_left ? blink::WebMouseEvent::Button::kLeft
                              : blink::WebMouseEvent::Button::kNoButton;
  if (button_down) {
    event.SetModifiers(blink::WebInputEvent::kLeftButtonDown);
  }
  event.click_count = click_count;
  const double scale = widget_scale_ > 0 ? widget_scale_ : 1.0;
  const gfx::PointF in_widget((css_x + offset_x_) * scale,
                              (css_y + offset_y_) * scale);
  event.SetPositionInWidget(in_widget);
  // Screen position = widget position + the target view's origin on screen,
  // the real-input convention (DevTools' ConvertWidgetPointToScreenPoint,
  // input_handler.cc:635-642). This is what puts a browser-chrome offset
  // between screenX and clientX, which an all-zero screen position would
  // betray as automation.
  gfx::PointF in_screen = in_widget;
  if (content::RenderWidgetHostView* view = widget->GetView()) {
    const gfx::Rect bounds = view->GetViewBounds();  // screen DIP
    in_screen = gfx::PointF(in_widget.x() + bounds.x(),
                            in_widget.y() + bounds.y());
  }
  event.SetPositionInScreen(in_screen);
  event.force = 0.0f;  // like CDP-injected mouse events (input_handler.cc:509)
  widget->ForwardMouseEvent(event);
  last_x_ = css_x;
  last_y_ = css_y;
  ++events_sent_;
  return true;
}

void CaptchaExecutor::ReleasePressed() {
  if (!press_open_) {
    return;
  }
  press_open_ = false;
  if (!frame_) {
    return;  // the widget died with the button state
  }
  // Manager retries a lost CDP release once; ForwardMouseEvent is a
  // synchronous browser->widget hand-off with no failure path to retry on,
  // so one release at the last position is the complete equivalent. The
  // release carries button=kLeft (see SendMouseEvent).
  SendMouseEvent(GestureAction::Kind::kUp, /*button_down=*/false, last_x_,
                 last_y_, 1);
}

void CaptchaExecutor::Finish(const std::string& outcome,
                             const std::string& reason,
                             const std::string& detail) {
  if (finished_) {
    return;
  }
  finished_ = true;
  timer_.Stop();
  watchdog_.Stop();
  verify_deadline_.Stop();
  // The guarantee, whatever path leaves this run: no button of ours stays
  // down (contract §2: the button is ALWAYS released).
  ReleasePressed();

  CaptchaExecResult result;
  result.outcome = outcome;
  result.acted = acted_;
  // verified is true ONLY for outcome "verified" (contract §2).
  result.verified = outcome == "verified";
  result.reason = reason;
  // execute_ms covers press..release (or press..finish when the schedule was
  // cut short after the press); a run that never pressed reports 0.
  if (acted_ && !execute_measured_) {
    execute_ms_ = (base::TimeTicks::Now() - press_at_).InMilliseconds();
    execute_measured_ = true;
  }
  result.execute_ms = execute_ms_;
  result.detail = detail;

  // Posted, never inline: an observer notification / dropped-mojo-callback
  // teardown must not run caller code inside it (the reply arrives as a
  // default-injected null during RenderFrameHost::TearDownMojoConnection,
  // right after this notification).
  pending_result_ = std::move(result);
  has_pending_result_ = true;
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&CaptchaExecutor::DeliverResult,
                                weak_factory_.GetWeakPtr()));
}

void CaptchaExecutor::DeliverResult() {
  if (!has_pending_result_) {
    return;  // the destructor already dropped it
  }
  has_pending_result_ = false;
  base::OnceCallback<void(CaptchaExecResult)> callback = std::move(callback_);
  callback_ = base::OnceCallback<void(CaptchaExecResult)>();
  CaptchaExecResult result = std::move(pending_result_);
  std::move(callback).Run(std::move(result));
}

void CaptchaExecutor::Cancel(const std::string& reason) {
  if (!started_) {
    // No Run() yet: remember it, the next Run() answers error/cancelled and
    // touches nothing (running Finish() here would run a null callback_).
    cancel_before_run_ = true;
    pre_cancel_reason_ = reason;
    return;
  }
  if (finished_) {
    return;
  }
  timer_.Stop();
  watchdog_.Stop();
  verify_deadline_.Stop();
  ReleasePressed();
  const std::string detail =
      base::StrCat({"caller cancel: ", reason,
                    " events=", base::NumberToString(events_sent_)});
  // Manager: acted + abort -> unconfirmed/cancelled; untouched ->
  // error/cancelled.
  if (acted_) {
    Finish("unconfirmed", "cancelled", detail);
  } else {
    Finish("error", "cancelled", detail);
  }
}

void CaptchaExecutor::RenderFrameDeleted(
    content::RenderFrameHost* render_frame_host) {
  if (render_frame_host != frame_) {
    return;
  }
  // Always clear: a frame may die between construction and Run(), and a
  // dangling raw_ptr would make Run() unsafe later.
  frame_ = nullptr;
  if (!started_ || finished_) {
    return;  // no callback to run yet; a finished run is already reported
  }
  if (!acted_) {
    // Nothing reached the page: the widget is gone, its button state died
    // with it, there is nothing to release and nothing to verify.
    Finish("error", "execute-failed",
           base::StrCat({"the frame was deleted mid-run (acted=0, events=",
                         base::NumberToString(events_sent_), ")"}));
    return;
  }
  // acted already: the solve may have navigated the frame. Wait for the
  // replacement RFH of the same FrameTreeNode; PollVerify re-resolves and the
  // verify deadline bounds the wait.
  press_open_ = false;  // the old widget (and its press state) is gone
  if (!verify_phase_) {
    // Deleted between the press and the gesture's end: stop the schedule and
    // judge the new document instead.
    if (!execute_measured_) {
      execute_ms_ = (base::TimeTicks::Now() - press_at_).InMilliseconds();
      execute_measured_ = true;
    }
    StartVerify();
  }
}

void CaptchaExecutor::RenderFrameHostChanged(
    content::RenderFrameHost* old_host,
    content::RenderFrameHost* new_host) {
  if (finished_ || old_host != frame_) {
    return;
  }
  if (!acted_) {
    RenderFrameDeleted(old_host);
    return;
  }
  if (new_host && new_host->IsActive() && new_host->IsRenderFrameLive()) {
    frame_ = new_host;
    frame_node_id_ = new_host->GetFrameTreeNodeId();
    // The new document may have committed while a poll was outstanding; the
    // mapping of the OLD document no longer addresses anything (input is
    // already done in this phase), so do not trust it again.
    offset_ready_ = false;
    press_open_ = false;
    return;
  }
  RenderFrameDeleted(old_host);
}

void CaptchaExecutor::WebContentsDestroyed() {
  frame_ = nullptr;  // always clear: may fire between construction and Run()
  if (!started_ || finished_) {
    return;
  }
  Finish(acted_ ? "unconfirmed" : "error", "tab-closed",
         base::StrCat({"the tab closed mid-run (events=",
                       base::NumberToString(events_sent_), ")"}));
}

}  // namespace shuttle

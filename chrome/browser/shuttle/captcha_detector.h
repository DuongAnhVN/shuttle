// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_DETECTOR_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_DETECTOR_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class NavigationHandle;
class Page;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace shuttle {

class CaptchaRegistry;
struct CaptchaRule;

// One observer per observed tab of the native CAPTCHA detection
// (captcha-contract.md section 2, "Detection"): the URL gate (rule host/path
// against every frame's committed URL) and the in-frame readiness check run
// through content::RenderFrameHost::ExecuteJavaScriptInIsolatedWorld in an
// embedder-range isolated world (content::ISOLATED_WORLD_ID_CONTENT_END -
// the public API CHECKs `world_id <= ISOLATED_WORLD_ID_MAX` in this tree, and
// this is the world captcha_collector.cc uses). No CDP and no DevTools: the
// check script is a FIXED template whose only variable part is one JSON
// string literal (base::GetQuotedJSONString of the serialized detect/binding
// spec), parsed with JSON.parse in the page; every selector is then handed to
// querySelector inside try/catch there. Selectors are data, never code.
//
// Wake-up strategy, measured against this tree (documented per the plan):
// a MutationObserver set up by the check script cannot wake the browser
// (rendered -> browser callbacks need a Mojo binding this feature must not
// add), and ExecuteJavaScriptInIsolatedWorld delivers the EVALUATED VALUE
// only: a Promise is not awaited by that API (the callback receives a
// serialized snapshot of the result, and a promise serializes to null), so
// event-driven results are not deliverable either. Detection therefore runs
// as: one pass debounced 120 ms after each DidFinishNavigation /
// DidFinishLoad / PrimaryPageChanged / RenderFrameCreated / DOMContentLoaded,
// plus a cadence timer per tab while any frame holds a candidate or a ready
// widget: every 500 ms for 30 s from the first candidate seen in that frame,
// then every 2 s. After any load-style event the same 500 ms window is held
// for 30 s regardless (single-page apps mount widgets late, without a
// navigation to observe). Trade-off: worst-case ready latency is the current
// tick (<= 500 ms in the fast window, <= 2 s after), at <= 2 script
// evaluations per frame per 2 s in steady state; a Mojo wake-up channel would
// remove the polling but adds a renderer-visible interface this contract
// forbids.
//
// The detector itself decides nothing: it forwards
// {ruleId, state, widget key, missing} per frame to CaptchaRegistry, which
// owns challenges, events, collection and the tab lock. Owned by the
// registry; UI thread only.
class CaptchaDetector : public content::WebContentsObserver {
 public:
  CaptchaDetector(content::WebContents* contents, CaptchaRegistry* registry);
  CaptchaDetector(const CaptchaDetector&) = delete;
  CaptchaDetector& operator=(const CaptchaDetector&) = delete;
  ~CaptchaDetector() override;

  // One detection pass for probe-captcha: `done` runs once every eligible
  // live frame has answered (or at once when none can), with one row per
  // (frame, eligible rule): {url, ruleId, state, missing[]} plus a plain
  // {url, state:"none"} row for frames no rule applies to. Detect only: the
  // pass never touches challenge or lock state.
  static constexpr size_t kMaxProbesInFlight = 4;
  using ProbeCallback = base::OnceCallback<void(base::ListValue)>;
  bool RunProbe(ProbeCallback done);
  // Whether RunProbe would accept a probe now (callers answer "busy"
  // themselves so the callback is never consumed on a refused probe).
  bool CanProbe() const;

  // The registry calls this when it attaches a detector (and when the rule
  // snapshot changes) so freshly set rules see the page that is already
  // there.
  void CheckNow();

  // Forget cached frame state (the rule snapshot was replaced).
  void ResetWatches();

  // Stable ID of a live frame for as long as its document stays: the
  // child process id and the frame routing id ("<n>:<m>"), used as the
  // frame token in challenges and events.
  static std::string FrameKey(const content::RenderFrameHost* frame);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void PrimaryPageChanged(content::Page& page) override;
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  void RenderFrameDeleted(content::RenderFrameHost* render_frame_host) override;
  void DOMContentLoaded(content::RenderFrameHost* render_frame_host) override;
  void WebContentsDestroyed() override;

 private:
  // Per live frame: what the last pass saw (drives the cadence) and whether
  // a watch pass is in flight.
  struct FrameWatch {
    int checks_in_flight = 0;
    bool has_candidate = false;
    bool has_ready = false;
    // When the first candidate was seen here (drives 500 ms -> 2 s).
    base::TimeTicks candidate_since;
  };

  // A probe pass in flight, keyed by a per-detector generation so late
  // answers can never land in a recycled slot.
  struct ProbeRequest {
    ProbeRequest();
    ProbeRequest(ProbeRequest&&);
    ProbeRequest& operator=(ProbeRequest&&);
    ~ProbeRequest();

    int waiting = 0;
    base::ListValue rows;
    std::map<std::string, std::string> url_by_frame;  // frame key -> URL
    ProbeCallback done;
  };

  // Runs one pass over the live frames. `probe` (when non-null) collects a
  // probe's rows instead of reporting to the registry; `generation` names
  // that probe (0 for a watch pass).
  void StartPass(ProbeRequest* probe, uint64_t generation);
  void OnCheckResult(std::string frame_key,
                     uint64_t generation,
                     bool for_probe,
                     base::Value result);
  void FinishProbeIfNeeded(uint64_t generation);

  // Debounce after page events, then the pass.
  void ScheduleCheck();
  void UpdateCadence();

  // The rules of the current snapshot that address this frame (the URL
  // gate). Pointers are valid within one UI-thread task (the snapshot is
  // immutable while pass building runs).
  std::vector<const CaptchaRule*> EligibleRules(
      const content::RenderFrameHost* frame,
      const GURL& page_url) const;

  raw_ptr<CaptchaRegistry> registry_;
  std::map<std::string, FrameWatch> frames_;
  // generation -> probe in flight (entries are erased as they finish).
  std::map<uint64_t, std::unique_ptr<ProbeRequest>> probes_;
  uint64_t next_probe_generation_ = 1;
  base::OneShotTimer debounce_;
  base::RepeatingTimer cadence_;
  // After every load-style event the tab gets a fast polling grace window:
  // single-page apps mount widgets long after DidFinishLoad without any
  // navigation to observe, and without a Mojo wake-up channel a late
  // appearance is only visible to the next tick.
  base::TimeTicks load_grace_until_;

  // Must stay last: invalidates everything bound from this object.
  base::WeakPtrFactory<CaptchaDetector> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_DETECTOR_H_

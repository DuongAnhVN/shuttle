// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/captcha_detector.h"

#include <utility>
#include <map>
#include <memory>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/shuttle/captcha_registry.h"
#include "chrome/browser/shuttle/captcha_rules.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"

namespace shuttle {

CaptchaDetector::ProbeRequest::ProbeRequest() = default;
CaptchaDetector::ProbeRequest::ProbeRequest(ProbeRequest&&) = default;
CaptchaDetector::ProbeRequest& CaptchaDetector::ProbeRequest::operator=(
    ProbeRequest&&) = default;
CaptchaDetector::ProbeRequest::~ProbeRequest() = default;

namespace {

// Fast polling window while a candidate is young (contract: ~500 ms for
// ~30 s after a candidate frame appears, then 2 s).
constexpr base::TimeDelta kFastTick = base::Milliseconds(500);
constexpr base::TimeDelta kSlowTick = base::Seconds(2);
constexpr base::TimeDelta kFastWindow = base::Seconds(30);
constexpr base::TimeDelta kEventDebounce = base::Milliseconds(120);

// The fixed check template. Its only variable part is the JSON string
// literal spliced between PREFIX and SUFFIX; the page parses it with
// JSON.parse and hands every selector to querySelector inside try/catch.
// Per rule it answers {id, state: none|candidate|ready|excluded,
// missing[], key}: `key` is a cheap widget identity — role boxes RELATIVE
// TO THE ROOT (viewport coordinates would change on every scroll and make
// the registry re-collect, and the collector's scrollIntoView would then
// fight the user), plus image natural sizes and src lengths. It lets the
// registry notice an image swap or a moved widget without a fresh collect.
constexpr char kCheckScriptPrefix[] = R"JS((() => {
  let rules = [];
  try { rules = JSON.parse( )JS";

constexpr char kCheckScriptSuffix[] = R"JS(); } catch (e) { return '[]'; }
  const out = [];
  // null = no match, BAD = the selector threw (invalid), undefined = no
  // selector given. querySelector's own null must not read as "invalid".
  const BAD = {};
  const qs = (node, sel) => {
    if (typeof sel !== 'string' || sel === '') return undefined;
    try { return node.querySelector(sel); } catch (e) { return BAD; }
  };
  const visible = (el) => {
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return false;
    for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
      const st = getComputedStyle(n);
      if (st.display === 'none' || st.visibility === 'hidden') return false;
    }
    return true;
  };
  // Box of `el` relative to `base` (the root rect): scroll-independent.
  const rel = (el, base) => {
    const r = el.getBoundingClientRect();
    return Math.round((r.x - base.x) * 10) / 10 + ',' +
        Math.round((r.y - base.y) * 10) / 10 + ',' +
        Math.round(r.width * 10) / 10 + ',' + Math.round(r.height * 10) / 10;
  };
  for (const r of rules) {
    const res = {id: r.id, state: 'none', missing: [], key: ''};
    const spec = r.b || {};
    const root = qs(document, r.root);
    if (root === BAD) { res.missing.push('bad-selector'); out.push(res); continue; }
    if (!root) { out.push(res); continue; }
    if (r.vis && !visible(root)) {
      res.state = 'candidate';
      res.missing.push('hidden');
      out.push(res);
      continue;
    }
    const rootBox = root.getBoundingClientRect();
    let excluded = false;
    let bad = false;
    for (const s of r.exc || []) {
      const el = qs(root, s);
      if (el === BAD) { bad = true; break; }
      if (el) { excluded = true; break; }
    }
    if (bad) {
      res.missing.length = 0;
      res.missing.push('bad-selector');
      out.push(res);
      continue;
    }
    if (excluded) { res.state = 'excluded'; out.push(res); continue; }
    for (let i = 0; i < (r.req || []).length; i++) {
      const el = qs(root, r.req[i]);
      if (el === BAD) { bad = true; break; }
      if (!el) res.missing.push('required-' + i);
    }
    if (bad) {
      res.state = 'none';
      res.missing.length = 0;
      res.missing.push('bad-selector');
      out.push(res);
      continue;
    }
    const roles = [['bg', spec.background], ['pc', spec.piece],
                   ['hd', spec.handle], ['tr', spec.track]];
    for (const pair of roles) {
      const el = qs(root, pair[1]);
      if (el === BAD) { bad = true; break; }
      if (!el) { res.missing.push('missing-' + pair[0]); continue; }
      if (r.vis && !visible(el)) { res.missing.push('hidden-' + pair[0]); continue; }
      if (el.tagName === 'IMG' && (!el.complete || el.naturalWidth === 0)) {
        res.missing.push('loading-' + pair[0]);
        continue;
      }
      res.key += pair[0] + ':' + rel(el, rootBox) + '#';
      if (el.tagName === 'IMG') {
        res.key += pair[0] + ':' + el.naturalWidth + 'x' + el.naturalHeight +
            ':' + String(el.currentSrc || el.src || '').length + '#';
      }
    }
    if (bad) {
      res.state = 'none';
      res.missing.length = 0;
      res.missing.push('bad-selector');
      out.push(res);
      continue;
    }
    res.state = res.missing.length === 0 ? 'ready' : 'candidate';
    out.push(res);
  }
  return JSON.stringify(out);
})())JS";

}  // namespace

CaptchaDetector::CaptchaDetector(content::WebContents* contents,
                                 CaptchaRegistry* registry)
    : content::WebContentsObserver(contents), registry_(registry) {}

CaptchaDetector::~CaptchaDetector() {
  std::map<uint64_t, std::unique_ptr<ProbeRequest>> probes = std::move(probes_);
  for (auto& [generation, request] : probes) {
    if (request && request->done) {
      std::move(request->done).Run(std::move(request->rows));
    }
  }
}

std::string CaptchaDetector::FrameKey(
    const content::RenderFrameHost* frame) {
  const content::GlobalRenderFrameHostId id = frame->GetGlobalId();
  return base::StrCat({base::NumberToString(id.child_id.value()), ":",
                       base::NumberToString(id.frame_routing_id)});
}

bool CaptchaDetector::CanProbe() const {
  return web_contents() != nullptr && probes_.size() < kMaxProbesInFlight;
}

bool CaptchaDetector::RunProbe(ProbeCallback done) {
  if (!web_contents() || probes_.size() >= kMaxProbesInFlight) {
    return false;
  }
  const uint64_t generation = next_probe_generation_++;
  probes_[generation] = std::make_unique<ProbeRequest>();
  ProbeRequest* request = probes_[generation].get();
  request->done = std::move(done);
  StartPass(request, generation);
  if (request->waiting == 0) {
    FinishProbeIfNeeded(generation);
  }
  return true;
}

void CaptchaDetector::CheckNow() {
  if (!web_contents() || !registry_->HasEnabledRules()) {
    return;
  }
  StartPass(nullptr, 0);
  UpdateCadence();
}

void CaptchaDetector::ResetWatches() {
  frames_.clear();
  load_grace_until_ = base::TimeTicks();
  debounce_.Stop();
  cadence_.Stop();
}

void CaptchaDetector::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!registry_->HasEnabledRules() || !web_contents()) {
    return;
  }
  if (handle->HasCommitted() && !handle->IsSameDocument()) {
    if (handle->IsInPrimaryMainFrame()) {
      frames_.clear();
      registry_->OnPageReset(web_contents());
    } else if (content::RenderFrameHost* rfh = handle->GetRenderFrameHost()) {
      const std::string key = FrameKey(rfh);
      frames_.erase(key);
      registry_->OnFrameGone(web_contents(), key);
    }
  }
  ScheduleCheck();
}

void CaptchaDetector::DidFinishLoad(content::RenderFrameHost* render_frame_host,
                                    const GURL& validated_url) {
  ScheduleCheck();
}

void CaptchaDetector::PrimaryPageChanged(content::Page& page) {
  if (!registry_->HasEnabledRules() || !web_contents()) {
    return;
  }
  // Back/forward restore and same-process page swaps: no navigation handle
  // reaches us for the old state, so drop everything and re-detect.
  frames_.clear();
  registry_->OnPageReset(web_contents());
  ScheduleCheck();
}

void CaptchaDetector::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  ScheduleCheck();
}

void CaptchaDetector::RenderFrameDeleted(
    content::RenderFrameHost* render_frame_host) {
  if (!web_contents()) {
    return;
  }
  const std::string key = FrameKey(render_frame_host);
  const bool watched = frames_.erase(key) > 0;
  registry_->OnFrameGone(web_contents(), key);
  // Probes waiting on this frame answer now: the result callback is dropped
  // with the frame.
  std::vector<uint64_t> done;
  for (auto& [generation, request] : probes_) {
    if (request->url_by_frame.erase(key) > 0 && request->waiting > 0 &&
        --request->waiting == 0) {
      done.push_back(generation);
    }
  }
  for (const uint64_t generation : done) {
    FinishProbeIfNeeded(generation);
  }
  if (watched) {
    UpdateCadence();
  }
}

void CaptchaDetector::DOMContentLoaded(
    content::RenderFrameHost* render_frame_host) {
  ScheduleCheck();
}

void CaptchaDetector::WebContentsDestroyed() {
  debounce_.Stop();
  cadence_.Stop();
  frames_.clear();
  content::WebContents* contents = web_contents();
  // Pending probes answer with what they have; a tab that dies never answers
  // again.
  std::vector<std::pair<ProbeCallback, base::ListValue>> finishing;
  for (auto& [generation, request] : probes_) {
    if (request && request->waiting > 0 && request->done) {
      finishing.emplace_back(std::move(request->done),
                             std::move(request->rows));
    }
  }
  probes_.clear();
  registry_->OnTabGone(contents, this);
  for (auto& [done, rows] : finishing) {
    std::move(done).Run(std::move(rows));
  }
}

std::vector<const CaptchaRule*> CaptchaDetector::EligibleRules(
    const content::RenderFrameHost* frame,
    const GURL& page_url) const {
  std::vector<const CaptchaRule*> out;
  const GURL url = frame->GetLastCommittedURL();
  if (!url.is_valid()) {
    return out;
  }
  for (const CaptchaRule& rule : registry_->rules()) {
    if (!rule.enabled) {
      continue;
    }
    if (CaptchaRuleAppliesToFrame(rule, !frame->GetParent(), url.host(),
                                  url.path(), page_url.host(),
                                  page_url.path())) {
      out.push_back(&rule);
    }
  }
  return out;
}

void CaptchaDetector::StartPass(ProbeRequest* probe, uint64_t generation) {
  content::WebContents* contents = web_contents();
  if (!contents) {
    return;
  }
  if (!probe && !registry_->HasEnabledRules()) {
    debounce_.Stop();
    cadence_.Stop();
    return;
  }
  content::RenderFrameHost* main = contents->GetPrimaryMainFrame();
  if (!main || !main->IsRenderFrameLive()) {
    return;
  }
  const GURL page_url = main->GetLastCommittedURL();

  std::vector<content::RenderFrameHost*> frames;
  // The primary page only: WebContents-level iteration would also reach
  // bfcached and prerendered frames, which are not the agent's document.
  main->ForEachRenderFrameHost(
      [&frames](content::RenderFrameHost* frame) {
        if (frame->IsRenderFrameLive()) {
          frames.push_back(frame);
        }
      });

  for (content::RenderFrameHost* frame : frames) {
    std::vector<const CaptchaRule*> rules = EligibleRules(frame, page_url);
    if (rules.empty()) {
      if (probe) {
        const GURL url = frame->GetLastCommittedURL();
        probe->rows.Append(base::DictValue()
                               .Set("url", url.spec())
                               .Set("state", "none"));
      }
      continue;
    }
    const std::string key = FrameKey(frame);
    if (!probe) {
      FrameWatch& watch = frames_[key];
      if (watch.checks_in_flight > 0) {
        continue;  // a slower answer is already on its way for this frame
      }
      ++watch.checks_in_flight;
    }
    if (probe) {
      ++probe->waiting;
      probe->url_by_frame[key] = frame->GetLastCommittedURL().spec();
    }
    const std::string script =
        base::StrCat({kCheckScriptPrefix,
                      QuoteAsJsonLiteral(CaptchaCheckSpecJson(rules)),
                      kCheckScriptSuffix});
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script),
        base::BindOnce(&CaptchaDetector::OnCheckResult,
                       weak_factory_.GetWeakPtr(), key, generation,
                       probe != nullptr),
        // A valid embedder-range world id: the public API CHECKs
        // `world_id <= ISOLATED_WORLD_ID_MAX` in this tree
        // (content/browser/renderer_host/render_frame_host_impl.cc:4005-4017),
        // so Chrome's internal id (>= 2^31-ish) would crash. The collector
        // uses the same world (captcha_collector.cc).
        content::ISOLATED_WORLD_ID_CONTENT_END);
  }
}

void CaptchaDetector::OnCheckResult(std::string frame_key,
                                    uint64_t generation,
                                    bool for_probe,
                                    base::Value result) {
  const base::ListValue* parsed = nullptr;
  base::ListValue results;
  if (result.is_string()) {
    std::optional<base::ListValue> read =
        base::JSONReader::ReadList(result.GetString(), base::JSON_PARSE_RFC);
    if (read) {
      results = std::move(*read);
      parsed = &results;
    }
  }
  if (for_probe) {
    auto it = probes_.find(generation);
    if (it == probes_.end() || !it->second) {
      return;
    }
    ProbeRequest& request = *it->second;
    auto url_it = request.url_by_frame.find(frame_key);
    if (url_it == request.url_by_frame.end()) {
      return;  // RenderFrameDeleted already counted this frame done
    }
    if (parsed) {
      for (const base::Value& value : *parsed) {
        const base::DictValue* entry_ptr = value.GetIfDict();
        if (!entry_ptr) {
          continue;
        }
        const base::DictValue& entry = *entry_ptr;
        base::DictValue row;
        row.Set("url", url_it->second);
        if (const std::string* rule_id = entry.FindString("id")) {
          row.Set("ruleId", *rule_id);
        }
        row.Set("state", entry.FindString("state")
                             ? *entry.FindString("state")
                             : "none");
        if (const base::ListValue* missing = entry.FindList("missing");
            missing && !missing->empty()) {
          row.Set("missing", missing->Clone());
        }
        request.rows.Append(std::move(row));
      }
    }
    if (request.waiting > 0) {
      --request.waiting;
    }
    if (request.waiting == 0) {
      FinishProbeIfNeeded(generation);
    }
    return;
  }

  auto it = frames_.find(frame_key);
  if (it == frames_.end()) {
    return;  // the frame is gone; OnFrameGone already spoke for it
  }
  FrameWatch& watch = it->second;
  if (watch.checks_in_flight > 0) {
    --watch.checks_in_flight;
  }
  if (!parsed) {
    UpdateCadence();  // unreadable answer: keep the flags, try on the next tick
    return;
  }
  bool has_candidate = false;
  bool has_ready = false;
  {
    for (const base::Value& value : *parsed) {
      const base::DictValue* entry_ptr = value.GetIfDict();
      if (!entry_ptr) {
        continue;
      }
      const base::DictValue& entry = *entry_ptr;
      const std::string* state = entry.FindString("state");
      if (state && *state == "candidate") {
        has_candidate = true;
      } else if (state && *state == "ready") {
        has_ready = true;
      }
      if (web_contents() && state && entry.FindString("id")) {
        registry_->OnWidgetState(web_contents(), frame_key,
                                 *entry.FindString("id"), *state,
                                 entry.FindString("key")
                                     ? *entry.FindString("key")
                                     : std::string(),
                                 entry.FindList("missing"));
      }
    }
  }
  watch.has_candidate = has_candidate;
  watch.has_ready = has_ready;
  if (has_candidate && watch.candidate_since.is_null()) {
    watch.candidate_since = base::TimeTicks::Now();
  }
  if (!has_candidate) {
    watch.candidate_since = base::TimeTicks();
  }
  UpdateCadence();
}

void CaptchaDetector::FinishProbeIfNeeded(uint64_t generation) {
  auto it = probes_.find(generation);
  if (it == probes_.end() || !it->second || it->second->waiting > 0 ||
      !it->second->done) {
    return;
  }
  ProbeCallback done = std::move(it->second->done);
  base::ListValue rows = std::move(it->second->rows);
  probes_.erase(it);
  std::move(done).Run(std::move(rows));
}

void CaptchaDetector::ScheduleCheck() {
  if (!web_contents() || !registry_->HasEnabledRules()) {
    return;
  }
  load_grace_until_ = base::TimeTicks::Now() + kFastWindow;
  if (debounce_.IsRunning()) {
    return;  // the debounced pass will see everything that changed
  }
  debounce_.Start(FROM_HERE, kEventDebounce, this,
                  &CaptchaDetector::CheckNow);
}

void CaptchaDetector::UpdateCadence() {
  const base::TimeTicks now = base::TimeTicks::Now();
  bool any = now < load_grace_until_;  // late-mounted SPA widgets: keep the
                                        // grace window ticking
  bool fast = any;
  for (auto& [key, watch] : frames_) {
    if (watch.has_candidate) {
      any = true;
      if (watch.candidate_since.is_null()) {
        watch.candidate_since = now;
      }
      if (now - watch.candidate_since < kFastWindow) {
        fast = true;
      }
    }
    if (watch.has_ready) {
      any = true;
    }
  }
  if (!any) {
    cadence_.Stop();
    return;
  }
  cadence_.Start(FROM_HERE, fast ? kFastTick : kSlowTick, this,
                 &CaptchaDetector::CheckNow);
}

}  // namespace shuttle

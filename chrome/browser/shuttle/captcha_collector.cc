// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Reads one puzzle-slide challenge out of a frame without DevTools/CDP.
//
// Everything page-side is a synchronous IIFE run through
// content::RenderFrameHost::ExecuteJavaScriptInIsolatedWorld. That API does
// NOT await promises: the renderer evaluates with
// mojom::blink::PromiseResultOption::kDoNotWait
// (third_party/blink/renderer/core/frame/local_frame_mojo_handler.cc:1017),
// so no script below returns a promise. The in-page picture read is limited
// to (a) decoding data: URLs and (b) synchronous canvas.toDataURL /
// drawImage of an already-loaded image. Anything the page cannot hand over
// synchronously (cross-origin <img>, http(s) CSS background, tainted canvas)
// falls back to a browser-side capture of the element's own box through
// content::RenderWidgetHostView::CopyFromSurface, PNG-encoded with
// gfx::PNGCodec - the no-CDP equivalent of the Manager's
// Page.captureScreenshot clip.
//
// Coordinate spaces (verified in this tree):
// - The measure script reports boxes in CSS px of the FRAME's viewport.
// - CopyFromSurface's `src_rect` is in the VIEW's own coordinates, i.e. DIP
//   (content/public/browser/render_widget_host_view.h:226-256); the Aura path
//   scales it by the device scale factor before asking Viz
//   (content/browser/renderer_host/delegated_frame_host.cc:229-232), so the
//   returned bitmap is in physical pixels. GetVisibleViewportSize() returns
//   the visible viewport in the same DIP space (render_widget_host_view.h:
//   213-217).
// - CSS px -> view DIP: multiply by the frame tree's page-zoom factor
//   (content::HostZoomMap::GetZoomLevel -> blink::ZoomLevelToZoomFactor; the
//   same browser-zoom leg content's own DevTools input uses in
//   content/browser/devtools/protocol/input_handler.cc:1561-1584).
// - Out-of-process frames are captured through their OWN guest view: the
//   guest's local coordinate origin IS the guest viewport, so no root
//   transform is applied (RenderWidgetHostView::TransformPointToRootCoordSpace
//   F, render_widget_host_view.h:109-122, exists for addressing the ROOT view
//   from a child point - deliberately not used here). A same-process child
//   frame shares its local-root widget's view, so the iframe-chain offset
//   from the local root down to the frame is read from the page
//   (/*captcha-frame-offset*/): border-box rect + the iframe element's
//   clientLeft/clientTop (borders) + computed paddingLeft/paddingTop, because
//   the child viewport starts at the element's CONTENT box.
// Known limits: pinch page-scale and CSS zoom on the embedding element are
// only reachable through content/browser internals
// (RenderWidgetHostViewBase::GetCSSZoomFactor, VisualProperties), not from
// chrome/browser over public APIs, so they are assumed 1. Captcha widgets
// essentially never use either.
//
// Lifetime notes (review-driven):
// - CopyFromSurface's callback "isn't guaranteed to run on the same task
//   sequence as this method was called from" (render_widget_host_view.h:
//   244-248): the reply is wrapped in base::BindPostTask to the UI runner
//   (base/task/bind_post_task.h:66-78) instead of touching WeakPtrs on an
//   unknown sequence.
// - A frame's deletion notifies WebContentsObserver FIRST and only then
//   tears down the mojo connection, which makes
//   mojo::WrapCallbackWithDefaultInvokeIfNotRun run the JS reply callback
//   synchronously (with a null Value) inside the teardown. Every reply
//   handler therefore re-checks the frame and answers "frame-gone" /
//   kFrameGone itself, and the user callback is POSTED to the UI runner by
//   Finish(), never run inline, so caller code never executes inside RFH
//   teardown.
// - A hung page or a hidden/occluded tab (CopyFromSurface times out there,
//   render_widget_host_view.h:249-251 + CopyFromSurfaceError::kTimeout,
//   lines 52-61) cannot wedge a session: an overall 10 s deadline answers.
//   Collect: kCaptureFailed (the enum carries no detail channel, so the
//   capture-timeout distinction is not expressible on the wire - documented
//   here and in the header). Recheck: "image-changed" while the frame is
//   alive (Manager's "a plan is never trusted on doubt"; "missing" would
//   claim the element vanished, "moved"/"hidden" would lie, "frame-gone"
//   only applies when it truly is), "frame-gone" when the frame died.

#include "chrome/browser/shuttle/captcha_collector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/string_escape.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/isolated_world_ids.h"
#include "crypto/sha2.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace shuttle {
namespace {

// Mirrors Manager collector.js constants.
constexpr int kMaxRoleImageBytes = 4 * 1024 * 1024;
constexpr double kGeometryTolerancePx = 1.0;
constexpr base::TimeDelta kShotTimeout = base::Seconds(5);
constexpr size_t kMaxInstructionChars = 500;
// Overall cap of one collect/recheck session (review finding 5): a page that
// stops answering scripts or a Viz that never replies must not wedge the
// caller's state machine.
constexpr base::TimeDelta kSessionTimeout = base::Seconds(10);

// The isolated world the captcha page scripts run in.
//
// Justification (verified in this tree):
// - The public API accepts only 1 <= world_id <= ISOLATED_WORLD_ID_MAX:
//   content::ISOLATED_WORLD_ID_CONTENT_END == 1, ISOLATED_WORLD_ID_MAX == 11
//   (content/public/common/isolated_world_ids.h:14-23), enforced by
//   RenderFrameHostImpl::ExecuteJavaScriptInIsolatedWorld
//   (content/browser/renderer_host/render_frame_host_impl.cc:4010-4011). The
//   renderer takes any embedder id in [1, 1<<29) and creates the world on
//   demand (third_party/blink/renderer/core/frame/local_frame_mojo_handler.
//   cc:992-998 and web_local_frame_impl.cc:1258-1264 EnsureIsolatedWorld).
// - Chrome claims 2..6: translate=2, indigo=3, chrome-internal=4,
//   applescript=5 (Mac), and extensions allocate content-script worlds
//   UPWARD from ISOLATED_WORLD_ID_EXTENSIONS with an unbounded counter
//   (chrome/common/chrome_isolated_world_ids.h:12-31;
//   chrome/renderer/extensions/chrome_extensions_renderer_client.cc:73;
//   extensions/renderer/isolated_world_manager.cc:141-150). Any fixed id in
//   7..11 can therefore collide with an extension world in some renderer.
// - ID 1 is the documented first custom embedder id
//   (isolated_world_ids.h:18-19) and nothing in this tree uses it: grep for
//   ISOLATED_WORLD_ID_CONTENT_END matches only the two constant headers.
// So ID 1 is the only value in the API's legal range that cannot collide.
constexpr int32_t kShuttleCaptchaWorldId =
    content::ISOLATED_WORLD_ID_CONTENT_END;

std::string JsonQuote(std::string_view value) {
  return base::GetQuotedJSONString(value);
}

// The binding as a role -> selector map. Empty selectors are not bound.
std::map<std::string, std::string> BindingRoles(
    const CaptchaBindingSpec& binding) {
  std::map<std::string, std::string> roles;
  const std::pair<const char*, std::string> candidates[] = {
      {"background", binding.background},
      {"handle", binding.handle},
      {"instruction", binding.instruction},
      {"piece", binding.piece},
      {"root", binding.root},
      {"track", binding.track},
  };
  for (const auto& [name, selector] : candidates) {
    if (!selector.empty()) {
      roles.emplace(name, selector);
    }
  }
  return roles;
}

// Selectors are DATA: every one reaches the page as a JSON string literal
// passed to querySelector inside a try/catch, never as concatenated code.
std::string RolesToJson(const std::map<std::string, std::string>& roles) {
  std::string out = "{";
  bool first = true;
  for (const auto& [name, selector] : roles) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += JsonQuote(name) + ":" + JsonQuote(selector);
  }
  return out + "}";
}

// ---------------------------------------------------------------- the scripts

// One call: resolve every role, scroll each found element into view ONCE
// (when SCROLL), then measure them all at that single scroll position.
// Mirrors Manager collector.js measureScript().
constexpr char kMeasureScriptHead[] = R"JS((() => { /*captcha-measure*/
  const SEL = )JS";

constexpr char kMeasureScriptBody[] = R"JS(;
  const out = { vw: innerWidth, vh: innerHeight, dpr: devicePixelRatio, scrollX: Math.round(scrollX), scrollY: Math.round(scrollY), roles: {} };
  const found = {};
  for (const name of Object.keys(SEL)) {
    let el = null;
    try { el = document.querySelector(SEL[name]); } catch (e) { out.roles[name] = { bad: true }; continue; }
    if (!el) { out.roles[name] = { missing: true }; continue; }
    found[name] = el;
  }
  if (SCROLL) {
    for (const name of Object.keys(found)) {
      try { found[name].scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' }); }
      catch (e) { try { found[name].scrollIntoView(false); } catch (e2) {} }
    }
  }
  for (const name of Object.keys(found)) {
    const el = found[name];
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    const box = { x: r.left, y: r.top, width: r.width, height: r.height };
    const inView = box.x < out.vw && box.y < out.vh && box.x + box.width > 0 && box.y + box.height > 0;
    out.roles[name] = {
      x: box.x, y: box.y, width: box.width, height: box.height,
      hidden: cs.display === 'none' || cs.visibility === 'hidden' || box.width <= 0 || box.height <= 0 || !inView,
      painted: cs.display !== 'none' && cs.visibility !== 'hidden' && box.width > 0 && box.height > 0,
      tag: el.tagName,
      text: String(el.innerText || el.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 500),
    };
  }
  return out;
})())JS";

std::string MeasureScript(const std::map<std::string, std::string>& roles,
                          bool scroll) {
  return std::string(kMeasureScriptHead) + RolesToJson(roles) +
         (scroll ? ";\n  const SCROLL = true" : ";\n  const SCROLL = false") +
         kMeasureScriptBody;
}

// One role's picture, synchronously. Deviation from Manager sourceScript():
// that script awaits fetch() because CDP awaits the promise; this API does
// not (kDoNotWait), so there is no network fetch here. Sources:
//   img:        a data: URL decoded in-page; otherwise an ALREADY-LOADED img
//               drawn to a canvas (a tainted drawImage throws -> recorded, so
//               the caller takes the element screenshot);
//   canvas:     canvas.toDataURL (tainted throws), never getImageData;
//   background: a url() on the element or its first 400 descendants, decoded
//               only when it is a data: URL (http(s) cannot be fetched
//               synchronously -> `background:no-sync-fetch`);
// then the caller's fallback is the element's own box screenshot. The 4 MB
// decoded cap is refused in-page, like Manager grab() refuses it.
constexpr char kSourceScriptHead[] = R"JS((() => { /*captcha-source*/
  const SEL = )JS";

constexpr char kSourceScriptBody[] = R"JS(;
  const MAX = 4194304;
  const MAXPX = 8388608;
  const why = [];
  let el = null;
  try { el = document.querySelector(SEL); } catch (e) { return { ok: false, why: 'bad-selector' }; }
  if (!el) return { ok: false, why: 'missing' };
  const b64Of = (bytes) => {
    let bin = '';
    for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(bin);
  };
  const fromDataUrl = (u) => {
    const comma = u.indexOf(',');
    if (comma < 0) return null;
    const parts = u.slice(5, comma).split(';');
    const mime = (parts[0] || '').toLowerCase();
    if (parts.slice(1).indexOf('base64') >= 0) return { mime: mime || 'image/png', b64: u.slice(comma + 1) };
    let text;
    try { text = decodeURIComponent(u.slice(comma + 1)); } catch (e) { return null; }
    const bytes = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i) & 0xff;
    return { mime: mime || 'image/png', b64: b64Of(bytes) };
  };
  const decodedLen = (b) => {
    const s = String(b).replace(/[\t\n\r ]+/g, '');
    let p = 0;
    if (s.endsWith('==')) p = 2; else if (s.endsWith('=')) p = 1;
    return Math.floor(s.length * 3 / 4) - p;
  };
  const pick = (tag) => (el.tagName === tag ? el : el.querySelector(tag.toLowerCase()));
  const img = pick('IMG');
  if (img) {
    const raw = img.currentSrc || img.getAttribute('src') || img.src || '';
    let u = '';
    try { u = new URL(raw, location.href).href; } catch (e) { u = ''; }
    if (!u) why.push('img:bad-url');
    else if (u.slice(0, 5) === 'data:') {
      const d = fromDataUrl(u);
      if (d && d.b64 && decodedLen(d.b64) <= MAX) return { ok: true, source: 'img', mime: d.mime, b64: d.b64, src: 'data:' + d.mime };
      why.push(d && d.b64 ? 'img:too-large' : 'img:bad-data-url');
    } else if (u.slice(0, 5) !== 'https' && u.slice(0, 4) !== 'http') why.push('img:unsupported-scheme');
    else if (!img.complete || img.naturalWidth <= 0) why.push('img:not-loaded');
    else {
      try {
        const c = document.createElement('canvas');
        c.width = img.naturalWidth;
        c.height = img.naturalHeight || img.height;
        if (!(c.width > 0 && c.height > 0)) why.push('img:zero-size');
        else if (c.width * c.height > MAXPX) why.push('img:too-large');
        else {
          c.getContext('2d').drawImage(img, 0, 0);
          const d = fromDataUrl(c.toDataURL('image/png'));
          if (d && d.b64 && decodedLen(d.b64) <= MAX) return { ok: true, source: 'img', mime: d.mime, b64: d.b64, src: u };
          if (d && d.b64) why.push('img:too-large'); else why.push('img:unreadable');
        }
      } catch (e) { why.push('img:tainted'); }
    }
  }
  const canvas = pick('CANVAS');
  if (canvas) {
    try {
      const d = fromDataUrl(canvas.toDataURL('image/png'));
      if (d && d.b64 && decodedLen(d.b64) <= MAX) return { ok: true, source: 'canvas', mime: d.mime, b64: d.b64, src: null };
      why.push(d && d.b64 ? 'canvas:too-large' : 'canvas:unreadable');
    } catch (e) { why.push('canvas:tainted'); }
  }
  const bgUrl = (() => {
    const nodes = [el].concat(Array.prototype.slice.call(el.querySelectorAll('*'), 0, 400));
    for (const node of nodes) {
      const s = getComputedStyle(node).backgroundImage || '';
      const m = s.match(/url\((?:"([^"]*)"|'([^']*)'|([^")]*))\)/);
      if (m) return m[1] || m[2] || m[3] || '';
    }
    return '';
  })();
  if (bgUrl) {
    let u = '';
    try { u = new URL(bgUrl, location.href).href; } catch (e) { u = ''; }
    if (u.slice(0, 5) === 'data:') {
      const d = fromDataUrl(u);
      if (d && d.b64 && decodedLen(d.b64) <= MAX) return { ok: true, source: 'background', mime: d.mime, b64: d.b64, src: 'data:' + d.mime };
      else if (d && d.b64) why.push('background:too-large'); else why.push('background:bad-data-url');
    } else {
      why.push('background:no-sync-fetch');
    }
  }
  return { ok: false, why: why.length ? why.join(',') : 'no-image-source' };
})())JS";

std::string SourceScript(const std::string& selector) {
  return std::string(kSourceScriptHead) + JsonQuote(selector) +
         kSourceScriptBody;
}

// The frame's viewport origin inside its OWN widget's viewport, in CSS px:
// walk exactly `depth` embedding frames up (the same-widget ancestor chain
// computed browser-side), reading each iframe element's box. A child viewport
// starts at the iframe element's CONTENT box, so each step adds the
// border-box rect plus the border (clientLeft/clientTop) plus the computed
// padding. Returns null when any step is cross-origin (frameElement
// unreadable), which makes the caller refuse element screenshots for a
// same-process child frame. For the main frame and out-of-process frames
// `depth` is 0 and no offset is needed.
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

// ---------------------------------------------------------------- parsing

// One measured role box (the /*captcha-measure*/ shape).
struct MeasuredRole {
  bool measured = false;  // present and neither bad nor missing
  bool bad = false;
  bool missing = false;
  bool hidden = false;
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  std::string text;
};

struct MeasuredPage {
  bool ok = false;
  double vw = 0;
  double vh = 0;
  std::map<std::string, MeasuredRole> roles;
};

double Or0(const std::optional<double>& v) {
  return v && std::isfinite(*v) ? *v : 0.0;
}

// Page numbers are untrusted: clamp in double space, never cast a NaN or an
// out-of-range double straight to int.
int ClampD(double v, int lo, int hi) {
  if (!std::isfinite(v)) {
    return lo;
  }
  if (v <= static_cast<double>(lo)) {
    return lo;
  }
  if (v >= static_cast<double>(hi)) {
    return hi;
  }
  return static_cast<int>(v);
}

// JS `Math.round(Number(n) || 0)` (Manager int()): half-up, non-finite -> 0,
// clamped before the int cast.
int RoundedJsInt(double v) {
  if (!std::isfinite(v)) {
    return 0;
  }
  return ClampD(std::floor(v + 0.5), -2147483647, 2147483647);
}

// JS `round(n, 2)` from Manager collector.js.
double Rounded2(double v) {
  if (!std::isfinite(v)) {
    return 0.0;
  }
  return std::round(v * 100.0) / 100.0;
}

// Only the known role names are looked up (base::DictValue iteration is
// avoided deliberately).
MeasuredPage ParseMeasured(const base::Value& value,
                           const std::map<std::string, std::string>& roles) {
  MeasuredPage page;
  const base::DictValue* dict = value.GetIfDict();
  if (!dict) {
    return page;
  }
  page.vw = Or0(dict->FindDouble("vw"));
  page.vh = Or0(dict->FindDouble("vh"));
  const base::DictValue* measured = dict->FindDict("roles");
  if (!measured) {
    return page;
  }
  page.ok = true;
  for (const auto& [name, selector] : roles) {
    const base::DictValue* r = measured->FindDict(name);
    MeasuredRole role;
    if (!r) {
      page.roles.emplace(name, role);  // the script never saw this role
      continue;
    }
    role.bad = r->FindBool("bad").value_or(false);
    role.missing = r->FindBool("missing").value_or(false);
    if (role.bad || role.missing) {
      page.roles.emplace(name, role);
      continue;
    }
    role.measured = true;
    role.hidden = r->FindBool("hidden").value_or(false);
    role.x = Or0(r->FindDouble("x"));
    role.y = Or0(r->FindDouble("y"));
    role.width = Or0(r->FindDouble("width"));
    role.height = Or0(r->FindDouble("height"));
    if (const std::string* text = r->FindString("text")) {
      role.text = text->substr(0, kMaxInstructionChars);
    }
    page.roles.emplace(name, role);
  }
  return page;
}

// ---------------------------------------------------------------- bytes

std::string ShaHex(std::string_view bytes) {
  return base::HexEncodeLower(crypto::SHA256HashString(bytes));
}

std::optional<std::string> DecodeBase64(const std::string& b64) {
  std::string out;
  if (!base::Base64Decode(b64, &out)) {
    return std::nullopt;
  }
  return out;
}

// Picture size from the encoded header (Manager's imageSize()): PNG IHDR and
// JPEG SOF markers; false for every other format so the caller falls back to
// the element box. All offsets are bounds-checked before the read.
bool ImageSizeOf(const std::string& bytes, int* width, int* height) {
  const base::span<const uint8_t> b = base::as_byte_span(base::span(bytes));
  const size_t n = b.size();
  static constexpr uint8_t kPngSig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a,
                                        0x1a, 0x0a};
  if (n >= 24 && std::ranges::equal(b.first(8u), kPngSig) &&
      b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
    *width = static_cast<int>(
        (static_cast<uint32_t>(b[16]) << 24) | (b[17] << 16) | (b[18] << 8) |
        b[19]);
    *height = static_cast<int>(
        (static_cast<uint32_t>(b[20]) << 24) | (b[21] << 16) | (b[22] << 8) |
        b[23]);
    return *width > 0 && *height > 0;
  }
  if (n >= 10 && b[0] == 0xff && b[1] == 0xd8) {
    size_t p = 2;
    while (p + 9 < n) {
      if (b[p] != 0xff) {
        ++p;
        continue;
      }
      const uint8_t marker = b[p + 1];
      if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9)) {
        p += 2;  // standalone markers carry no length
        continue;
      }
      if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
          marker != 0xc8 && marker != 0xcc) {
        *height = static_cast<int>((b[p + 5] << 8) | b[p + 6]);
        *width = static_cast<int>((b[p + 7] << 8) | b[p + 8]);
        return *width > 0 && *height > 0;
      }
      const size_t len =
          (static_cast<size_t>(b[p + 2]) << 8) | static_cast<size_t>(b[p + 3]);
      if (len < 2) {
        return false;
      }
      p += 2 + len;
    }
  }
  return false;
}

// The internal state digest, mirroring Manager collector.js digestRoles():
// sha256 over `JSON.stringify({v:1, roles:[{role, image, src, geo}...]})`
// with roles sorted by name (std::map order == JS sort order here) and geo
// rounded to whole CSS px. Deviation: Manager mixes each role's image `src`
// string into the token; the shared CollectedChallenge API carries no src
// (only `source`, which is the wire enum), so src is pinned to null. The
// digest is INTERNAL ONLY and collect/recheck build it identically, so
// freshness detection is unaffected.
std::string ComputeDigest(const std::map<std::string, CaptchaBox>& geometry,
                          const std::map<std::string, std::string>& hashes) {
  std::string canonical = "{\"v\":1,\"roles\":[";
  bool first = true;
  for (const auto& [name, hash] : hashes) {
    if (!first) {
      canonical += ",";
    }
    first = false;
    canonical += "{\"role\":" + JsonQuote(name) + ",\"image\":";
    canonical += hash.empty() ? "null" : JsonQuote(hash);
    canonical += ",\"src\":null,\"geo\":[";
    const auto geo = geometry.find(name);
    if (geo != geometry.end()) {
      canonical += std::to_string(RoundedJsInt(geo->second.x)) + "," +
                   std::to_string(RoundedJsInt(geo->second.y)) + "," +
                   std::to_string(RoundedJsInt(geo->second.width)) + "," +
                   std::to_string(RoundedJsInt(geo->second.height));
    } else {
      canonical += "null";
    }
    canonical += "]}";
  }
  canonical += "]}";
  return ShaHex(canonical);
}

// ---------------------------------------------------------------- sessions

// Shared machinery of one async read of one frame: script execution, the
// frame -> widget coordinate mapping, the element-box screenshot and the
// overall session deadline. Runs on the UI sequence; the derived sessions
// delete themselves in their posted Deliver step.
class FrameReadTask : public content::WebContentsObserver {
 public:
  FrameReadTask(const FrameReadTask&) = delete;
  FrameReadTask& operator=(const FrameReadTask&) = delete;

 protected:
  FrameReadTask(content::WebContents* contents,
                content::RenderFrameHost* frame)
      : content::WebContentsObserver(contents), frame_(frame) {}

  ~FrameReadTask() override = default;

  content::RenderFrameHost* frame() const { return frame_; }

  // content::WebContentsObserver - a dead frame ends every read. Finishing is
  // deferred one task so a session never deletes itself mid-notification.
  void RenderFrameDeleted(
      content::RenderFrameHost* render_frame_host) override {
    if (render_frame_host != frame_) {
      return;
    }
    frame_ = nullptr;
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&FrameReadTask::ReportGone,
                                  base_weak_factory_.GetWeakPtr()));
  }

  void WebContentsDestroyed() override {
    frame_ = nullptr;
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&FrameReadTask::ReportGone,
                                  base_weak_factory_.GetWeakPtr()));
  }

  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override {
    RenderFrameDeleted(old_host);
  }

  // The frame died or the session's overall deadline expired while the page
  // or Viz stopped replying; the derived session maps this to its result.
  virtual void OnFrameGone() = 0;
  virtual void OnTimeout() = 0;

  void ReportGone() {
    if (!frame_) {  // the read may have finished in the meantime
      OnFrameGone();
    }
  }

  void ArmSessionTimeout() {
    session_deadline_.Start(FROM_HERE, kSessionTimeout, this,
                            &FrameReadTask::OnSessionTimeoutDue);
  }

  void StopSessionTimeout() { session_deadline_.Stop(); }

  void OnSessionTimeoutDue() { OnTimeout(); }

  // Runs a script in the captcha isolated world. A dropped result (the frame
  // died before the reply) arrives as a null base::Value through
  // WrapCallbackWithDefaultInvokeIfNotRun - possibly INSIDE the RFH teardown,
  // which is why every handler re-checks the frame first and Finish() posts
  // its answer instead of running it inline.
  void RunScript(const std::string& script,
                 base::OnceCallback<void(base::Value)> callback) {
    if (!frame_) {
      std::move(callback).Run(base::Value());
      return;
    }
    frame_->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                    base::Value()),
        kShuttleCaptchaWorldId);
  }

  // Classifies the frame and computes widget_scale_/offset_. Main frame and
  // out-of-process frames need no page-side offset; a same-process child
  // frame first reads its iframe-chain offset (offset_ready_ stays false when
  // the chain is cross-origin, which makes ShootBox refuse; in-page reads
  // still work).
  void ResolveFrameMapping(base::OnceClosure done) {
    if (!frame_) {
      std::move(done).Run();
      return;
    }
    content::RenderFrameHost* widget_root = frame_;
    int depth = 0;
    while (content::RenderFrameHost* parent = widget_root->GetParent()) {
      if (parent->GetRenderWidgetHost() != frame_->GetRenderWidgetHost()) {
        break;  // crossed into another widget: `frame_` is a guest root
      }
      ++depth;
      widget_root = parent;
    }
    if (depth == 0) {
      // The main frame, or the root of an out-of-process frame tree: the
      // target's own view origin IS the frame viewport.
      offset_ready_ = true;
      widget_scale_ = FrameZoomFactor(frame_);
      std::move(done).Run();
      return;
    }
    // Same-process child: coordinates address the shared root widget, so the
    // iframe-chain offset is needed and the zoom is the widget root's.
    widget_scale_ = FrameZoomFactor(widget_root);
    RunScript(FrameOffsetScript(depth),
              base::BindOnce(&FrameReadTask::OnFrameOffset,
                             base_weak_factory_.GetWeakPtr(), std::move(done)));
  }

  // Copies the element box (frame-viewport CSS px) as PNG base64, or nullopt
  // when the box cannot be captured (no view, empty clipped box, Viz error
  // or timeout - a hidden/occluded tab hits that path).
  void ShootBox(const CaptchaBox& box,
                base::OnceCallback<void(std::optional<std::string>)> callback) {
    if (!frame_ || !offset_ready_) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    content::RenderWidgetHost* widget = frame_->GetRenderWidgetHost();
    content::RenderWidgetHostView* view = widget ? widget->GetView() : nullptr;
    if (!view) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    const double scale = widget_scale_ > 0 ? widget_scale_ : 1.0;
    const double x0 = (box.x + offset_x_) * scale;
    const double y0 = (box.y + offset_y_) * scale;
    const double x1 = (box.x + box.width + offset_x_) * scale;
    const double y1 = (box.y + box.height + offset_y_) * scale;
    // src_rect is in the view's own DIP coordinates (see the file note);
    // clip to the visible viewport, the same way Manager clips to the page.
    // ClampD keeps untrusted page geometry in int range before any cast.
    const gfx::Size viewport = view->GetVisibleViewportSize();
    const int left = ClampD(std::floor(x0), 0, viewport.width());
    const int top = ClampD(std::floor(y0), 0, viewport.height());
    const int right = ClampD(std::ceil(x1), 0, viewport.width());
    const int bottom = ClampD(std::ceil(y1), 0, viewport.height());
    if (right <= left || bottom <= top) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    // The callback may be run off this sequence (render_widget_host_view.h:
    // 244-248): BindPostTask moves it back to the UI runner, where touching
    // the WeakPtr-bound session is legal.
    view->CopyFromSurface(
        gfx::Rect(left, top, right - left, bottom - top), gfx::Size(),
        kShotTimeout,
        base::BindPostTask(
            content::GetUIThreadTaskRunner({}),
            base::BindOnce(&FrameReadTask::OnCopied,
                           base_weak_factory_.GetWeakPtr(),
                           std::move(callback))));
  }

 private:
  void OnFrameOffset(base::OnceClosure done, base::Value value) {
    if (!frame_) {  // died with the frame; done -> the guarded handler
      std::move(done).Run();
      return;
    }
    const base::DictValue* dict = value.GetIfDict();
    const std::optional<double> x = dict ? dict->FindDouble("x") : std::nullopt;
    const std::optional<double> y = dict ? dict->FindDouble("y") : std::nullopt;
    if (x && y && std::isfinite(*x) && std::isfinite(*y)) {
      offset_x_ = *x;
      offset_y_ = *y;
      offset_ready_ = true;
    }
    std::move(done).Run();
  }

  static double FrameZoomFactor(content::RenderFrameHost* frame) {
    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(frame);
    if (!contents) {
      return 1.0;
    }
    const double factor = blink::ZoomLevelToZoomFactor(
        content::HostZoomMap::GetZoomLevel(contents, frame->GetGlobalId()));
    return std::isfinite(factor) && factor > 0 ? factor : 1.0;
  }

  // Runs on the UI sequence (BindPostTask above).
  void OnCopied(base::OnceCallback<void(std::optional<std::string>)> callback,
                const content::CopyFromSurfaceResult& result) {
    std::optional<std::string> b64;
    if (result.has_value() && !result->bitmap.drawsNothing()) {
      std::optional<std::vector<uint8_t>> png =
          gfx::PNGCodec::FastEncodeBGRASkBitmap(result->bitmap, false);
      if (png && png->size() <= static_cast<size_t>(kMaxRoleImageBytes)) {
        b64 = base::Base64Encode(*png);
      }
    }
    std::move(callback).Run(std::move(b64));
  }

  raw_ptr<content::RenderFrameHost> frame_;
  bool offset_ready_ = false;
  double offset_x_ = 0;
  double offset_y_ = 0;
  double widget_scale_ = 1.0;
  base::OneShotTimer session_deadline_;

  base::WeakPtrFactory<FrameReadTask> base_weak_factory_{this};
};

// One challenge out of the page: measure once (with the single scroll), read
// each image role, screenshot what the page cannot hand over, then build the
// geometry map, the instruction and the internal digest.
class CollectSession final : public FrameReadTask {
 public:
  CollectSession(content::WebContents* contents,
                 content::RenderFrameHost* frame,
                 CaptchaBindingSpec binding,
                 CollectCallback callback)
      : FrameReadTask(contents, frame),
        binding_(std::move(binding)),
        callback_(std::move(callback)) {}

  CollectSession(const CollectSession&) = delete;
  CollectSession& operator=(const CollectSession&) = delete;

  void Start() {
    ArmSessionTimeout();
    roles_ = BindingRoles(binding_);
    if (roles_.empty()) {
      // Manager: 'the binding declares no role selectors' -> bad-selector.
      Finish(base::unexpected(CollectError::kBadSelector));
      return;
    }
    // The contract's puzzle-slide binding: these four roles are always
    // required (Manager DEFAULT_REQUIRED_ROLES).
    for (const char* name : kRequiredRoles) {
      if (!roles_.contains(name)) {
        Finish(base::unexpected(CollectError::kCaptureFailed));
        return;
      }
    }
    ResolveFrameMapping(base::BindOnce(&CollectSession::RunMeasure,
                                       session_weak_factory_.GetWeakPtr()));
  }

 private:
  static constexpr const char* kRequiredRoles[] = {"background", "handle",
                                                   "piece", "track"};

  void OnFrameGone() override {
    Finish(base::unexpected(CollectError::kFrameGone));
  }

  void OnTimeout() override {
    // The page/Viz stopped replying: kCaptureFailed (see the file note about
    // the missing detail channel for the capture-timeout distinction).
    Finish(base::unexpected(CollectError::kCaptureFailed));
  }

  void RunMeasure() {
    if (!frame()) {
      Finish(base::unexpected(CollectError::kFrameGone));
      return;
    }
    RunScript(MeasureScript(roles_, /*scroll=*/true),
              base::BindOnce(&CollectSession::OnMeasured,
                             session_weak_factory_.GetWeakPtr()));
  }

  void OnMeasured(base::Value value) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    measured_ = ParseMeasured(value, roles_);
    if (!measured_.ok) {
      // Manager: 'the measure script returned no layout' -> capture-failed.
      Finish(base::unexpected(CollectError::kCaptureFailed));
      return;
    }
    for (const auto& [name, role] : measured_.roles) {
      if (role.bad) {
        Finish(base::unexpected(CollectError::kBadSelector));
        return;
      }
    }
    for (const char* name : kRequiredRoles) {
      const MeasuredRole& role = measured_.roles[name];
      if (!role.measured || role.hidden) {
        // Manager: missing:<role> / hidden:<role> -> capture-failed.
        Finish(base::unexpected(CollectError::kCaptureFailed));
        return;
      }
    }
    for (const auto& [name, role] : measured_.roles) {
      if (role.measured) {
        CaptchaBox box;
        box.x = Rounded2(role.x);
        box.y = Rounded2(role.y);
        box.width = Rounded2(role.width);
        box.height = Rounded2(role.height);
        geometry_.emplace(name, box);
      }
    }
    for (const char* name : kRequiredRoles) {
      image_queue_.push_back(name);
    }
    NextImage();
  }

  void NextImage() {
    if (image_queue_.empty()) {
      FinishCollect();
      return;
    }
    const std::string role = image_queue_.front();
    RunScript(SourceScript(roles_[role]),
              base::BindOnce(&CollectSession::OnSourced,
                             session_weak_factory_.GetWeakPtr(), role));
  }

  void OnSourced(const std::string& role, base::Value value) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    const base::DictValue* dict = value.GetIfDict();
    if (dict && dict->FindBool("ok").value_or(false)) {
      const std::string* b64 = dict->FindString("b64");
      const std::string* source = dict->FindString("source");
      if (b64 && !b64->empty() && source) {
        const std::string* mime = dict->FindString("mime");
        if (!RecordImage(role, *b64, mime ? *mime : std::string(), *source)) {
          Finish(base::unexpected(CollectError::kCaptureFailed));
          return;
        }
        std::erase(image_queue_, role);
        NextImage();
        return;
      }
    }
    // The fallback is the element's own box, never a crop of a bigger
    // picture (Manager collect()).
    ShootBox(geometry_[role],
             base::BindOnce(&CollectSession::OnShot,
                            session_weak_factory_.GetWeakPtr(), role));
  }

  void OnShot(const std::string& role, std::optional<std::string> b64) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    if (!b64) {
      // Every queued role is required (contract §1 binding), so one missing
      // picture fails the whole collect: Manager `no-image:<role>` (a hidden
      // tab's CopyFromSurface timeout lands here too).
      Finish(base::unexpected(CollectError::kCaptureFailed));
      return;
    }
    if (!RecordImage(role, *b64, "image/png", "screenshot")) {
      Finish(base::unexpected(CollectError::kCaptureFailed));
      return;
    }
    std::erase(image_queue_, role);
    NextImage();
  }

  // Parses and stores one role's picture; false = refused (bad base64 or over
  // the 4 MB decoded cap). The caller turns false into the collect failure,
  // because Finish() may delete this session and callers must not touch
  // members afterwards.
  bool RecordImage(const std::string& role,
                   const std::string& b64,
                   const std::string& mime,
                   const std::string& source) {
    std::optional<std::string> bytes = DecodeBase64(b64);
    if (!bytes || bytes->size() > static_cast<size_t>(kMaxRoleImageBytes)) {
      return false;
    }
    CaptchaImage image;
    image.b64 = b64;
    image.mime = mime.empty() ? "image/png" : mime;
    image.source = source;
    int width = 0;
    int height = 0;
    if (!ImageSizeOf(*bytes, &width, &height)) {
      // Only when the header is unknown does the element box stand in for it
      // (Manager imageFrom()).
      const auto geo = geometry_.find(role);
      width = geo != geometry_.end() ? RoundedJsInt(geo->second.width) : 0;
      height = geo != geometry_.end() ? RoundedJsInt(geo->second.height) : 0;
    }
    image.width = width;
    image.height = height;
    hashes_[role] = ShaHex(*bytes);
    images_[role] = std::move(image);
    return true;
  }

  void FinishCollect() {
    CollectedChallenge challenge;
    challenge.images = std::move(images_);
    challenge.geometry = std::move(geometry_);
    const auto instruction = measured_.roles.find("instruction");
    if (instruction != measured_.roles.end() && instruction->second.measured &&
        !instruction->second.text.empty()) {
      challenge.instruction = instruction->second.text;
    }
    challenge.digest = ComputeDigest(challenge.geometry, hashes_);
    Finish(std::move(challenge));
  }

  void Finish(base::expected<CollectedChallenge, CollectError> result) {
    if (done_) {
      return;
    }
    done_ = true;
    StopSessionTimeout();
    // Posted: never run caller code inside an RFH teardown or an observer
    // notification; Deliver() then deletes the session on a clean task.
    pending_ = std::move(result);
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&CollectSession::Deliver,
                                  session_weak_factory_.GetWeakPtr()));
  }

  void Deliver() {
    std::unique_ptr<CollectSession> self(this);
    if (!pending_) {
      return;
    }
    CollectCallback callback = std::move(callback_);
    std::move(callback).Run(std::move(*pending_));
  }

  CaptchaBindingSpec binding_;
  CollectCallback callback_;
  std::optional<base::expected<CollectedChallenge, CollectError>> pending_;
  std::map<std::string, std::string> roles_;
  MeasuredPage measured_;
  std::vector<std::string> image_queue_;
  std::map<std::string, CaptchaImage> images_;
  std::map<std::string, CaptchaBox> geometry_;
  std::map<std::string, std::string> hashes_;
  bool done_ = false;

  base::WeakPtrFactory<CollectSession> session_weak_factory_{this};
};

// Is the page still showing exactly what was captured? Cheap re-read: no
// scroll, 1 CSS px geometry tolerance, reasons in the fixed order
// bad-selector -> missing -> hidden -> moved -> image-changed (a dead frame
// answers frame-gone). A role captured as "screenshot" is re-shot; every
// other source is re-read in-page only, so a canvas PNG and an earlier
// screenshot are never compared. Mirrors Manager collector.js recheck().
class RecheckSession final : public FrameReadTask {
 public:
  RecheckSession(content::WebContents* contents,
                 content::RenderFrameHost* frame,
                 CaptchaBindingSpec binding,
                 CollectedChallenge collected,
                 base::OnceCallback<void(RecheckResult)> callback)
      : FrameReadTask(contents, frame),
        binding_(std::move(binding)),
        collected_(std::move(collected)),
        callback_(std::move(callback)) {}

  RecheckSession(const RecheckSession&) = delete;
  RecheckSession& operator=(const RecheckSession&) = delete;

  void Start() {
    ArmSessionTimeout();
    // Only roles the challenge actually captured (Manager recheck filter).
    for (const auto& [name, selector] : BindingRoles(binding_)) {
      if (collected_.geometry.contains(name) ||
          collected_.images.contains(name)) {
        roles_.emplace(name, selector);
      }
    }
    if (roles_.empty()) {
      Finish("missing");  // 'the binding no longer names a captured role'
      return;
    }
    for (const auto& [name, image] : collected_.images) {
      std::optional<std::string> bytes = DecodeBase64(image.b64);
      if (!bytes) {
        Finish("image-changed");  // the stored picture is unreadable
        return;
      }
      old_hashes_[name] = ShaHex(*bytes);
    }
    ResolveFrameMapping(base::BindOnce(&RecheckSession::RunMeasure,
                                       session_weak_factory_.GetWeakPtr()));
  }

 private:
  void OnFrameGone() override { Finish("frame-gone"); }

  void OnTimeout() override {
    // The page stopped answering the re-read: refuse to call the plan fresh.
    // "image-changed" is the least misleading existing reason while the
    // frame is alive (see the file note); a dead frame is frame-gone anyway
    // through ReportGone.
    Finish(frame() ? "image-changed" : "frame-gone");
  }

  void RunMeasure() {
    if (!frame()) {
      Finish("frame-gone");
      return;
    }
    RunScript(MeasureScript(roles_, /*scroll=*/false),
              base::BindOnce(&RecheckSession::OnMeasured,
                             session_weak_factory_.GetWeakPtr()));
  }

  void OnMeasured(base::Value value) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    MeasuredPage page = ParseMeasured(value, roles_);
    if (!page.ok) {
      Finish("missing");  // Manager: 'the page gave no layout'
      return;
    }
    // The fixed reason order, role order = map order within each category.
    for (const auto& [name, role] : page.roles) {
      if (role.bad) {
        Finish("bad-selector");
        return;
      }
    }
    for (const auto& [name, role] : page.roles) {
      if (collected_.geometry.contains(name) && !role.measured) {
        Finish("missing");
        return;
      }
    }
    for (const auto& [name, role] : page.roles) {
      if (collected_.geometry.contains(name) && role.measured && role.hidden) {
        Finish("hidden");
        return;
      }
    }
    for (const auto& [name, role] : page.roles) {
      const auto before = collected_.geometry.find(name);
      if (before == collected_.geometry.end() || !role.measured) {
        continue;
      }
      const CaptchaBox& old_box = before->second;
      const bool moved =
          std::abs(role.x - old_box.x) > kGeometryTolerancePx ||
          std::abs(role.y - old_box.y) > kGeometryTolerancePx ||
          std::abs(role.width - old_box.width) > kGeometryTolerancePx ||
          std::abs(role.height - old_box.height) > kGeometryTolerancePx;
      if (moved) {
        Finish("moved");
        return;
      }
    }
    for (const auto& [name, role] : page.roles) {
      if (role.measured) {
        CaptchaBox box;
        box.x = Rounded2(role.x);
        box.y = Rounded2(role.y);
        box.width = Rounded2(role.width);
        box.height = Rounded2(role.height);
        fresh_boxes_.emplace(name, box);
      }
    }
    for (const auto& [name, image] : collected_.images) {
      image_queue_.push_back(name);
    }
    NextImage();
  }

  void NextImage() {
    if (image_queue_.empty()) {
      // Every captured role re-read to the same picture hash. (Manager also
      // rebuilds the digest as a last check; with `src` pinned to null here
      // - see ComputeDigest - a role-by-role hash match makes the rebuilt
      // digest necessarily equal, so the extra pass is dropped.)
      Finish("");
      return;
    }
    const std::string role = image_queue_.front();
    const auto selector = roles_.find(role);
    if (selector == roles_.end()) {
      Finish("missing");
      return;
    }
    const auto stored = collected_.images.find(role);
    if (stored != collected_.images.end() &&
        stored->second.source == "screenshot") {
      // Re-shoot the element box; an in-page canvas read of the same region
      // would be a different capture path and could never hash-match the
      // stored PNG (finding 9).
      const auto box = fresh_boxes_.find(role);
      if (box == fresh_boxes_.end()) {
        Finish("missing");
        return;
      }
      ShootBox(box->second, base::BindOnce(&RecheckSession::OnShot,
                                          session_weak_factory_.GetWeakPtr(),
                                          role));
      return;
    }
    RunScript(SourceScript(selector->second),
              base::BindOnce(&RecheckSession::OnSourced,
                             session_weak_factory_.GetWeakPtr(), role));
  }

  void OnSourced(const std::string& role, base::Value value) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    // In-page source only (the screenshot-stored roles never arrive here).
    // "the picture can no longer be re-read through the source it came from"
    // is, like Manager's fallback refusal, `image-changed`: a plan is never
    // trusted on doubt.
    const base::DictValue* dict = value.GetIfDict();
    if (!dict || !dict->FindBool("ok").value_or(false)) {
      Finish("image-changed");
      return;
    }
    const std::string* b64 = dict->FindString("b64");
    if (!b64 || b64->empty()) {
      Finish("image-changed");
      return;
    }
    std::optional<std::string> bytes = DecodeBase64(*b64);
    if (!bytes ||
        bytes->size() > static_cast<size_t>(kMaxRoleImageBytes)) {
      Finish("image-changed");
      return;
    }
    CompareHash(role, ShaHex(*bytes));
  }

  void OnShot(const std::string& role, std::optional<std::string> b64) {
    if (done_) {
      return;
    }
    if (!frame()) {
      OnFrameGone();
      return;
    }
    std::optional<std::string> bytes = b64 ? DecodeBase64(*b64) : std::nullopt;
    if (!bytes) {
      // Manager: stale('image-changed', '<role> (the picture can no longer
      // be read)').
      Finish("image-changed");
      return;
    }
    CompareHash(role, ShaHex(*bytes));
  }

  void CompareHash(const std::string& role, const std::string& hash) {
    if (done_) {
      return;
    }
    const auto old = old_hashes_.find(role);
    if (old == old_hashes_.end() || old->second != hash) {
      Finish("image-changed");
      return;
    }
    std::erase(image_queue_, role);
    NextImage();
  }

  void Finish(const std::string& reason) {
    if (done_) {
      return;
    }
    done_ = true;
    StopSessionTimeout();
    // Posted, never inline (see CollectSession::Finish).
    RecheckResult result;
    result.fresh = reason.empty();
    result.reason = reason;
    pending_ = std::move(result);
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(&RecheckSession::Deliver,
                                  session_weak_factory_.GetWeakPtr()));
  }

  void Deliver() {
    std::unique_ptr<RecheckSession> self(this);
    if (!pending_) {
      return;
    }
    base::OnceCallback<void(RecheckResult)> callback = std::move(callback_);
    std::move(callback).Run(std::move(*pending_));
  }

  CaptchaBindingSpec binding_;
  CollectedChallenge collected_;
  base::OnceCallback<void(RecheckResult)> callback_;
  std::optional<RecheckResult> pending_;
  std::map<std::string, std::string> roles_;
  std::map<std::string, std::string> old_hashes_;
  std::map<std::string, CaptchaBox> fresh_boxes_;
  std::vector<std::string> image_queue_;
  bool done_ = false;

  base::WeakPtrFactory<RecheckSession> session_weak_factory_{this};
};

void PostCollectFailure(CollectCallback callback, CollectError error) {
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](CollectCallback cb, CollectError err) {
            std::move(cb).Run(base::unexpected(err));
          },
          std::move(callback), error));
}

void PostRecheckResult(base::OnceCallback<void(RecheckResult)> callback,
                       const std::string& reason) {
  RecheckResult result;
  result.fresh = false;
  result.reason = reason;
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::OnceCallback<void(RecheckResult)> cb, RecheckResult r) {
            std::move(cb).Run(std::move(r));
          },
          std::move(callback), std::move(result)));
}

}  // namespace

CaptchaBindingSpec::CaptchaBindingSpec() = default;
CaptchaBindingSpec::CaptchaBindingSpec(const CaptchaBindingSpec&) = default;
CaptchaBindingSpec& CaptchaBindingSpec::operator=(const CaptchaBindingSpec&) = default;
CaptchaBindingSpec::CaptchaBindingSpec(CaptchaBindingSpec&&) = default;
CaptchaBindingSpec& CaptchaBindingSpec::operator=(CaptchaBindingSpec&&) = default;
CaptchaBindingSpec::~CaptchaBindingSpec() = default;

CaptchaImage::CaptchaImage() = default;
CaptchaImage::CaptchaImage(const CaptchaImage&) = default;
CaptchaImage& CaptchaImage::operator=(const CaptchaImage&) = default;
CaptchaImage::CaptchaImage(CaptchaImage&&) = default;
CaptchaImage& CaptchaImage::operator=(CaptchaImage&&) = default;
CaptchaImage::~CaptchaImage() = default;

CollectedChallenge::CollectedChallenge() = default;
CollectedChallenge::CollectedChallenge(const CollectedChallenge&) = default;
CollectedChallenge& CollectedChallenge::operator=(const CollectedChallenge&) = default;
CollectedChallenge::CollectedChallenge(CollectedChallenge&&) = default;
CollectedChallenge& CollectedChallenge::operator=(CollectedChallenge&&) = default;
CollectedChallenge::~CollectedChallenge() = default;

void CollectChallenge(content::RenderFrameHost* frame,
                      const CaptchaBindingSpec& binding,
                      CollectCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  if (!frame || !contents || !frame->IsRenderFrameLive()) {
    PostCollectFailure(std::move(callback), CollectError::kFrameGone);
    return;
  }
  auto* session =
      new CollectSession(contents, frame, binding, std::move(callback));
  session->Start();
}

void RecheckChallenge(content::RenderFrameHost* frame,
                      const CaptchaBindingSpec& binding,
                      const CollectedChallenge& collected,
                      base::OnceCallback<void(RecheckResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  if (!frame || !contents || !frame->IsRenderFrameLive()) {
    PostRecheckResult(std::move(callback), "frame-gone");
    return;
  }
  auto* session = new RecheckSession(contents, frame, binding, collected,
                                     std::move(callback));
  session->Start();
}

}  // namespace shuttle

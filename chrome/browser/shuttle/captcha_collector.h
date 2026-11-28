// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_COLLECTOR_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_COLLECTOR_H_

#include <map>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/types/expected.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace shuttle {

// Reads one puzzle-slide challenge out of a frame WITHOUT DevTools/CDP: the
// page scripts run through content::RenderFrameHost::ExecuteJavaScriptInIso-
// latedWorld (synchronous IIFEs; that API does not await promises) and the
// image fallback is a browser-side content::RenderWidgetHostView::CopyFrom-
// Surface of the element's own box, PNG-encoded with gfx::PNGCodec. Behaviour
// mirrors shuttle-manager captcha/collector.js; the reason slugs and the state
// digest follow the shared contract (captcha-contract.md §2).
//
// Selectors are DATA: every selector reaches the page only as a JSON string
// literal (base::GetQuotedJSONString) handed to querySelector inside a
// try/catch, never concatenated as code.

// A rule's role -> CSS selector map (contract §1 `binding`, plus the `detect`
// root so geometry can be reported for the widget container). Empty string =
// the role is not bound.
struct CaptchaBindingSpec {
  CaptchaBindingSpec();
  CaptchaBindingSpec(const CaptchaBindingSpec&);
  CaptchaBindingSpec& operator=(const CaptchaBindingSpec&);
  CaptchaBindingSpec(CaptchaBindingSpec&&);
  CaptchaBindingSpec& operator=(CaptchaBindingSpec&&);
  ~CaptchaBindingSpec();
  std::string root;
  std::string background;
  std::string piece;
  std::string handle;
  std::string track;
  std::string instruction;
};

// One role's picture. `source` records where it came from:
// "img" | "canvas" | "background" | "screenshot".
struct CaptchaImage {
  CaptchaImage();
  CaptchaImage(const CaptchaImage&);
  CaptchaImage& operator=(const CaptchaImage&);
  CaptchaImage(CaptchaImage&&);
  CaptchaImage& operator=(CaptchaImage&&);
  ~CaptchaImage();
  std::string mime;
  std::string b64;
  int width = 0;
  int height = 0;
  std::string source;
};

// One role's box, CSS px in the frame's viewport at capture time.
struct CaptchaBox {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// Everything one collect read out of the page. `digest` is a hex sha256 over
// the per-role {image bytes digest, rounded geometry} ingredients, computed
// exactly like Manager collector.js digestRoles() (with `src` pinned to null,
// see captcha_collector.cc; the value is INTERNAL ONLY and never leaves
// Shuttle on the wire).
struct CollectedChallenge {
  CollectedChallenge();
  CollectedChallenge(const CollectedChallenge&);
  CollectedChallenge& operator=(const CollectedChallenge&);
  CollectedChallenge(CollectedChallenge&&);
  CollectedChallenge& operator=(CollectedChallenge&&);
  ~CollectedChallenge();
  std::map<std::string, CaptchaImage> images;
  std::map<std::string, CaptchaBox> geometry;
  std::optional<std::string> instruction;
  std::string digest;
};

enum class CollectError {
  kCaptureFailed,  // required role missing/hidden/unreadable, or no layout
  kBadSelector,    // a selector threw in querySelector (not valid CSS)
  kFrameGone,      // the frame or its tab died mid-capture
};

using CollectCallback =
    base::OnceCallback<void(base::expected<CollectedChallenge, CollectError>)>;

// Reads the challenge from `frame` (a live, committed frame; UI thread). One
// scrollIntoView pass, then all role boxes measured at that single scroll
// position. `callback` always runs exactly once, asynchronously on the UI
// task runner (never inline, so a frame-death teardown cannot be re-entered),
// and the session has an overall 10 s deadline that answers kCaptureFailed
// when the page or Viz stops replying (a hidden/occluded tab commonly times
// out in CopyFromSurface - that path is also kCaptureFailed; there is no
// detail channel in the enum, see .cc).
void CollectChallenge(content::RenderFrameHost* frame,
                      const CaptchaBindingSpec& binding,
                      CollectCallback callback);

struct RecheckResult {
  bool fresh = false;
  // "" when fresh; otherwise the first failing reason, in the fixed order
  // bad-selector -> missing -> hidden -> moved -> image-changed
  // ("frame-gone" when the frame or tab dies mid-recheck).
  std::string reason;
};

// Is the challenge still what the page shows? Cheap re-read: no scroll, the
// geometry compared with a 1 CSS px tolerance, each captured role's picture
// re-read and hashed like the collector does: a role stored as
// "screenshot" is re-shot through CopyFromSurface, every other source is
// re-read in-page only (the two capture paths are never compared against
// each other). On any difference the first failing reason wins, in the fixed
// order bad-selector -> missing -> hidden -> moved -> image-changed; a dead
// frame reports "frame-gone". A session that stops replying hits an overall
// 10 s deadline and answers "image-changed" while the frame is alive
// (Manager: "a plan is never trusted on doubt"), "frame-gone" when it is
// not. `callback` always runs exactly once, asynchronously on the UI task
// runner (never inline, so a frame-death teardown cannot be re-entered).
void RecheckChallenge(content::RenderFrameHost* frame,
                      const CaptchaBindingSpec& binding,
                      const CollectedChallenge& collected,
                      base::OnceCallback<void(RecheckResult)> callback);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_COLLECTOR_H_

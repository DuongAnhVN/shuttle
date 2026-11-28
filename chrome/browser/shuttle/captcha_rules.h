// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_RULES_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_RULES_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace shuttle {

// The CAPTCHA rule snapshot of the native-detection contract
// (captcha-contract.md section 1): the strict parser, the host/path matching,
// the puzzle-slide plan maths and the JSON string-literal escaping that keeps
// rule selectors data (never code) when they reach the page. Pure data: no
// Profile, no content, no collector. Unit tested alone
// (captcha_rules_unittest.cc).

// Contract limits.
inline constexpr size_t kMaxCaptchaRules = 32;
inline constexpr size_t kMaxCaptchaRulesJsonBytes = 64 * 1024;
inline constexpr size_t kMaxCaptchaRuleIdLen = 63;    // [a-z0-9][a-z0-9-]{0,62}
inline constexpr size_t kMaxCaptchaRuleNameLen = 80;
inline constexpr size_t kMaxCaptchaSelectorLen = 200;
inline constexpr size_t kMaxCaptchaHosts = 8;
inline constexpr size_t kMaxCaptchaPathPrefixes = 8;
inline constexpr size_t kMaxCaptchaSelectors = 8;
inline constexpr size_t kMaxCaptchaTextLen = 200;

// One page/frame URL selector: hosts (exact or "*." suffix) and optional
// path prefixes. At least one host (contract: hosts is 1..8).
struct CaptchaUrlMatch {
  CaptchaUrlMatch();
  CaptchaUrlMatch(const CaptchaUrlMatch&);
  CaptchaUrlMatch& operator=(const CaptchaUrlMatch&);
  CaptchaUrlMatch(CaptchaUrlMatch&&);
  CaptchaUrlMatch& operator=(CaptchaUrlMatch&&);
  ~CaptchaUrlMatch();

  std::vector<std::string> hosts;
  std::vector<std::string> path_prefixes;
};

// The in-frame readiness check (contract `detect`).
struct CaptchaDetectSpec {
  CaptchaDetectSpec();
  CaptchaDetectSpec(const CaptchaDetectSpec&);
  CaptchaDetectSpec& operator=(const CaptchaDetectSpec&);
  CaptchaDetectSpec(CaptchaDetectSpec&&);
  CaptchaDetectSpec& operator=(CaptchaDetectSpec&&);
  ~CaptchaDetectSpec();

  std::string root;
  std::vector<std::string> required_selectors;
  std::vector<std::string> excluded_selectors;
  bool require_visible = true;
};

// The widget bindings. Field-for-field the same aggregate as
// `CaptchaBindingSpec` in captcha_collector.h; the type is duplicated here on
// purpose so this header keeps no dependency on the collector (and the rules
// unit tests link without content). captcha_registry.cc converts field by
// field.
struct CaptchaBinding {
  CaptchaBinding();
  CaptchaBinding(const CaptchaBinding&);
  CaptchaBinding& operator=(const CaptchaBinding&);
  CaptchaBinding(CaptchaBinding&&);
  CaptchaBinding& operator=(CaptchaBinding&&);
  ~CaptchaBinding();

  std::string root;
  std::string background;
  std::string piece;
  std::string handle;
  std::string track;
  std::string instruction;
};

// A success/failure marker (contract `success` / `failure`). All fields
// optional; the `url` field exists for success only.
struct CaptchaMarkerRule {
  CaptchaMarkerRule();
  CaptchaMarkerRule(const CaptchaMarkerRule&);
  CaptchaMarkerRule& operator=(const CaptchaMarkerRule&);
  CaptchaMarkerRule(CaptchaMarkerRule&&);
  CaptchaMarkerRule& operator=(CaptchaMarkerRule&&);
  ~CaptchaMarkerRule();

  std::string text;
  std::string selector;
  std::string url;
  bool empty() const {
    return text.empty() && selector.empty() && url.empty();
  }
};

// One validated rule. Everything a later stage needs is copied into it: the
// snapshot is immutable once applied.
struct CaptchaRule {
  CaptchaRule();
  CaptchaRule(const CaptchaRule&);
  CaptchaRule& operator=(const CaptchaRule&);
  CaptchaRule(CaptchaRule&&);
  CaptchaRule& operator=(CaptchaRule&&);
  ~CaptchaRule();

  std::string id;
  std::string name;
  bool enabled = true;
  bool has_page = false;
  CaptchaUrlMatch page;
  bool has_frame = false;
  CaptchaUrlMatch frame;
  // frame.scope: true = "main", false = "iframe".
  bool frame_scope_main = true;
  CaptchaDetectSpec detect;
  CaptchaBinding binding;
  bool has_success = false;
  CaptchaMarkerRule success;
  bool has_failure = false;
  CaptchaMarkerRule failure;
  // travelMode: false = "ratio" (default), true = "direct".
  bool travel_direct = false;
};

// Whether `pattern` matches `host` (both already lowercase; the pattern is an
// exact host or a "*." suffix of one label or more).
bool CaptchaHostMatches(std::string_view pattern, std::string_view host);

// Whether any host of `match` hits `host` and (when path_prefixes is non-
// empty) `path` starts with one of its prefixes. Empty path_prefixes = any
// path.
bool CaptchaUrlMatchHits(const CaptchaUrlMatch& match,
                         std::string_view host,
                         std::string_view path);

// Whether `rule` asks the detector to look inside one frame:
//  - page.match (when present) must hit the page's main-frame URL;
//  - a rule with page only checks the main frame;
//  - frame.match checks frames of its scope against its hosts/paths;
//  - with both, both must match (frame.scope picks the frame).
// `frame_host`/`frame_path` name the frame under consideration;
// `page_host`/`page_path` the tab's main-frame URL.
bool CaptchaRuleAppliesToFrame(const CaptchaRule& rule,
                               bool is_main_frame,
                               std::string_view frame_host,
                               std::string_view frame_path,
                               std::string_view page_host,
                               std::string_view page_path);

// One invalid entry of a rejected snapshot (contract details[{ruleId?,error}]).
struct CaptchaRulesError {
  std::string rule_id;  // empty when the problem is not one rule's
  std::string error;
};

// Strict parse of `set-captcha-rules` arguments (contract section 1):
//  - args keys exactly {revision, rules};
//  - revision an integer >= 1;
//  - rules: array of 0..32 objects; the serialized snapshot <= 64 KB;
//  - every unknown key rejected; id/name/match/detect/binding/success/
//    failure/type validated as documented in captcha-contract.md;
//  - selectors are data: <=200 chars, no control characters, no NUL.
// On false, `errors` carries the per-rule problems (never empty).
bool ParseCaptchaRules(const base::DictValue& args,
                       int* revision,
                       std::vector<CaptchaRule>* rules,
                       std::vector<CaptchaRulesError>* errors);

// The rules re-serialized in canonical order (the snapshot identity used for
// the "same content at a lower revision" idempotence check).
std::string SerializeCaptchaRules(const std::vector<CaptchaRule>& rules);
// Canonical JSON serialization of one rule, for snapshot identity checks.
std::string CanonicalRuleJson(const CaptchaRule& rule);

// The JSON array the page script consumes: one object per rule with the
// detect spec and the binding selectors. It reaches the renderer only inside
// a JSON string literal (QuoteAsJsonLiteral), never concatenated as code.
std::string CaptchaCheckSpecJson(const std::vector<const CaptchaRule*>& rules);

// `value` as a quoted JSON string literal (with its surrounding quotes):
// valid JS, and the rule text stays inert data wherever it is embedded.
std::string QuoteAsJsonLiteral(std::string_view value);

// The puzzle-slide travel of a solution, contract section 2:
//   pieceTargetX = p * background.width
//   direct: travel = pieceTargetX
//   ratio:  travel = pieceTargetX * (track.width - handle.width)
//                    / (background.width - piece.width)   (direct when <= 0)
//   travel clamped to [0, track.width - handle.width];
// drag from the handle centre to centre + (travel, 0).
// Field-for-field `CaptchaSlidePlan` in captcha_executor.h (again so this
// header pulls no content dependency); `moves`/`hold_ms` carry the contract
// defaults (14, 120). nullopt on non-finite or negative input sizes.
struct CaptchaSlideMath {
  double from_x = 0;
  double from_y = 0;
  double to_x = 0;
  double to_y = 0;
  int moves = 14;
  int hold_ms = 120;
};
std::optional<CaptchaSlideMath> BuildSlidePlan(double slide_x_proportion,
                                               double background_width,
                                               double piece_width,
                                               double handle_width,
                                               double handle_center_x,
                                               double handle_center_y,
                                               double track_width,
                                               bool travel_direct);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_RULES_H_

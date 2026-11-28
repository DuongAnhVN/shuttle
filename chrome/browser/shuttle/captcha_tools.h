// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_CAPTCHA_TOOLS_H_
#define CHROME_BROWSER_SHUTTLE_CAPTCHA_TOOLS_H_

#include "base/values.h"
#include "chrome/browser/shuttle/mcp_tools.h"

namespace shuttle {
namespace captcha_tools {

// The seven /mcp tools of the native CAPTCHA detection (contract section 2;
// docs: tools/shuttle/docs/MCP.md). Same access class as create-agent and
// set-fingerprint: they ride the MCP endpoint's own authorization (the
// access token; over the network, admin) and are registered in the kTools
// table of mcp_tools.cc. All argument validation happens in the handlers and
// in CaptchaRegistry / ParseCaptchaRules; the schemas describe the contract.

// Schema JSONs (inline constexpr: mcp_tools.cc's constexpr kTools table
// needs complete arrays).

inline constexpr char kSetCaptchaRulesArgs[] = R"json({"type":"object","properties":{
  "revision":{"type":"integer","minimum":1,"description":"Monotonic snapshot revision. A revision below the applied one is refused with stale-revision unless the content is identical (idempotent retry). The snapshot replaces the previous one atomically; an invalid snapshot leaves the old one in place."},
  "rules":{"type":"array","maxItems":32,"description":"The whole snapshot (0..32 rules, <= 64 KB JSON). A rule: {id: '^[a-z0-9][a-z0-9-]{0,62}$', name?: <=80, enabled: bool, match: {page?: {hosts: 1..8, pathPrefixes?: <=8}, frame?: {scope: 'main'|'iframe', hosts: 1..8, pathPrefixes?: <=8}} (at least one), detect: {root: css, requiredSelectors?: <=8, excludedSelectors?: <=8, requireVisible?: true}, type: 'puzzle-slide', binding: {background, piece, handle, track, instruction?}, success?: {text?|selector?|url?}, failure?: {text?|selector?}, travelMode?: 'ratio'|'direct'}. Selectors are data (<= 200 chars, no control characters), never JavaScript; unknown keys are rejected. Manager policy (mode, provider, maxAttempts) stays in the Manager and is never sent here."}},
  "required":["revision","rules"],"additionalProperties":false})json";

inline constexpr char kGetCaptchaRulesArgs[] =
    R"json({"type":"object","properties":{},"additionalProperties":false})json";

inline constexpr char kProbeCaptchaArgs[] = R"json({"type":"object","properties":{
  "targetId":{"type":"string","description":"A leased tab (the targetId of allocate-tab and the page tools)."}},"required":["targetId"],"additionalProperties":false})json";

inline constexpr char kWaitCaptchaEventsArgs[] = R"json({"type":"object","properties":{
  "cursor":{"type":"integer","minimum":0,"description":"Last seq returned; 0 or absent reads from the oldest buffered event (the browser keeps the last 256)."},
  "waitMs":{"type":"integer","minimum":0,"maximum":30000,"description":"Wait up to this long for a new event (default 20000). Answers at once when events are already buffered."}},
  "additionalProperties":false})json";

inline constexpr char kChallengeIdArgs[] = R"json({"type":"object","properties":{
  "challengeId":{"type":"string","description":"The opaque ch_<hex> id from a challenge-ready event."}},
  "required":["challengeId"],"additionalProperties":false})json";

inline constexpr char kSolveCaptchaArgs[] = R"json({"type":"object","properties":{
  "challengeId":{"type":"string"},
  "stateRevision":{"type":"integer","minimum":1,"description":"The revision of the fetched Challenge. An older one is answered stale with no input sent; so is a fresh digest that differs from the stored one."},
  "solution":{"type":"object","properties":{"slideXProportion":{"type":"number","minimum":0,"maximum":1,"description":"Target x along the background, as in the Manager puzzle-slide maths."}},"required":["slideXProportion"],"additionalProperties":false},
  "verifyTimeoutMs":{"type":"integer","minimum":0,"maximum":20000,"description":"Verdict wait after the drag (default 6000)."},
   "deadlineMs":{"type":"integer","minimum":500,"maximum":15000,"description":"Hard stop for the whole attempt (default 15000)."}},
  "required":["challengeId","stateRevision","solution"],"additionalProperties":false})json";

// Handlers (the kTools signature). Registration lives in mcp_tools.cc.
void SetCaptchaRulesTool(const ToolContext& context,
                         const base::DictValue& args,
                         ToolCallback callback);
void GetCaptchaRulesTool(const ToolContext& context,
                         const base::DictValue& args,
                         ToolCallback callback);
void ProbeCaptchaTool(const ToolContext& context,
                      const base::DictValue& args,
                      ToolCallback callback);
void WaitCaptchaEventsTool(const ToolContext& context,
                           const base::DictValue& args,
                           ToolCallback callback);
void GetCaptchaChallengeTool(const ToolContext& context,
                             const base::DictValue& args,
                             ToolCallback callback);
void SolveCaptchaChallengeTool(const ToolContext& context,
                               const base::DictValue& args,
                               ToolCallback callback);
void CancelCaptchaChallengeTool(const ToolContext& context,
                                const base::DictValue& args,
                                ToolCallback callback);

}  // namespace captcha_tools
}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_CAPTCHA_TOOLS_H_

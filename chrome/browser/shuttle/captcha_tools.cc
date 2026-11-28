// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/captcha_tools.h"

#include <utility>
#include <memory>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/shuttle/captcha_registry.h"
#include "chrome/browser/shuttle/session_manager.h"
#include "chrome/browser/shuttle/shuttle_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

namespace shuttle {
namespace captcha_tools {

namespace {

ToolResult BadArgument(std::string_view message) {
  return ToolResult::Error(base::StrCat(
      {"Unexpected or malformed arguments: ", message}));
}

}  // namespace

void SetCaptchaRulesTool(const ToolContext& context,
                         const base::DictValue& args,
                         ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  CaptchaRegistry::SetOutcome outcome = CaptchaRegistry::Get().SetRules(args);
  if (outcome.ok) {
    // The tabs already open in this service's sessions become observable at
    // once (the same hook runs for every later MCP request).
    CaptchaRegistry::Get().ObserveProfile(context.service);
  }
  base::DictValue value;
  value.Set("ok", outcome.ok);
  if (!outcome.ok) {
    value.Set("error", outcome.error);
    if (!outcome.details.empty()) {
      value.Set("details", std::move(outcome.details));
    }
  } else {
    value.Set("revision", outcome.revision);
    value.Set("applied", outcome.applied);
  }
  std::move(callback).Run(ToolResult::Json(base::Value(std::move(value))));
}

void GetCaptchaRulesTool(const ToolContext& context,
                         const base::DictValue& args,
                         ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!args.empty()) {
    std::move(callback).Run(BadArgument("get-captcha-rules takes no arguments"));
    return;
  }
  std::move(callback).Run(ToolResult::Json(
      base::Value(CaptchaRegistry::Get().RulesStatusValue())));
}

void ProbeCaptchaTool(const ToolContext& context,
                      const base::DictValue& args,
                      ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::string* target_id = args.FindString("targetId");
  if (!target_id || target_id->empty()) {
    std::move(callback).Run(BadArgument("targetId is required"));
    return;
  }
  content::WebContents* contents = SessionManager::FindTab(*target_id);
  if (!contents) {
    std::move(callback).Run(
        ToolResult::Error("No such tab: " + *target_id));
    return;
  }
  auto pending_callback = std::make_shared<std::optional<ToolCallback>>(
      std::move(callback));
  const bool started = CaptchaRegistry::Get().ProbeTab(
      contents,
      base::BindOnce(
          [](std::shared_ptr<std::optional<ToolCallback>> pending,
             std::string target_id,
             base::ListValue frames) {
             if (!pending->has_value()) return;
             ToolCallback callback = std::move(pending->value());
             pending->reset();
             std::move(callback).Run(ToolResult::Json(base::Value(
                base::DictValue()
                    .Set("targetId", std::move(target_id))
                    .Set("frames", std::move(frames)))));
          },
          pending_callback, *target_id));
  if (!started) {
    ToolCallback pending = std::move(pending_callback->value());
    pending_callback->reset();
    std::move(pending).Run(
        ToolResult::Error("Busy: too many CAPTCHA probes are running"));
  }
}

void WaitCaptchaEventsTool(const ToolContext& context,
                           const base::DictValue& args,
                           ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  int cursor = 0;
  if (const std::optional<int> given = args.FindInt("cursor")) {
    if (given.value() < 0) {
      std::move(callback).Run(BadArgument("cursor must be >= 0"));
      return;
    }
    cursor = given.value();
  }
  int wait_ms = 20000;
  if (const std::optional<int> given = args.FindInt("waitMs")) {
    if (given.value() < 0 || given.value() > 30000) {
      std::move(callback).Run(
          BadArgument("waitMs must be 0..30000 (integer)"));
      return;
    }
    wait_ms = given.value();
  }
  CaptchaRegistry::Get().WaitForEvents(cursor, base::Milliseconds(wait_ms),
                                       std::move(callback));
}

void GetCaptchaChallengeTool(const ToolContext& context,
                             const base::DictValue& args,
                             ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::string* challenge_id = args.FindString("challengeId");
  if (!challenge_id || challenge_id->empty()) {
    std::move(callback).Run(BadArgument("challengeId is required"));
    return;
  }
  CaptchaRegistry::Get().GetChallenge(*challenge_id, std::move(callback));
}

void SolveCaptchaChallengeTool(const ToolContext& context,
                               const base::DictValue& args,
                               ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  CaptchaRegistry::Get().SolveChallenge(args, std::move(callback));
}

void CancelCaptchaChallengeTool(const ToolContext& context,
                                const base::DictValue& args,
                                ToolCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::string* challenge_id = args.FindString("challengeId");
  if (!challenge_id || challenge_id->empty()) {
    std::move(callback).Run(BadArgument("challengeId is required"));
    return;
  }
  CaptchaRegistry::Get().CancelChallenge(*challenge_id, std::move(callback));
}

}  // namespace captcha_tools
}  // namespace shuttle

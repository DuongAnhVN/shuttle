// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/captcha_rules.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace shuttle {

namespace {

bool IsLowerHexishLabelChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// ^[a-z0-9][a-z0-9-]{0,62}$ without a regex engine.
bool IsValidRuleId(std::string_view id) {
  if (id.empty() || id.size() > kMaxCaptchaRuleIdLen) {
    return false;
  }
  const char first = id.front();
  if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
    return false;
  }
  for (char c : id) {
    if (!IsLowerHexishLabelChar(c)) {
      return false;
    }
  }
  return true;
}

// Selectors are DATA (contract section 1): length <= 200, no control
// characters, no NUL. They only ever reach the page through a JSON string
// literal handed to querySelector inside try/catch.
bool IsValidSelector(std::string_view selector) {
  if (selector.empty() || selector.size() > kMaxCaptchaSelectorLen) {
    return false;
  }
  for (char c : selector) {
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
      return false;
    }
  }
  return true;
}

// A text marker (success/failure text): length bound, no NUL; other control
// characters are meaningless for matching but harmless as data.
bool IsValidText(std::string_view text, size_t max_len) {
  return !text.empty() && text.size() <= max_len && text.find('\0') ==
                                                             std::string::npos;
}

// Hosts: exact host or "*." suffix. No scheme, no port, no path (any ':',
// '/' or '@' is refused), labels of [a-z0-9-] with no empty or hyphen-edge
// label. Already lowercased by the caller.
bool IsValidHost(std::string_view host) {
  std::string_view body = host;
  if (base::StartsWith(body, "*.")) {
    body = body.substr(2);
  }
  if (body.empty() || body.find_first_of(":/@[]") != std::string_view::npos) {
    return false;
  }
  size_t labels = 0;
  size_t start = 0;
  while (start <= body.size()) {
    const size_t dot = body.find('.', start);
    const std::string_view label =
        body.substr(start, dot == std::string_view::npos
                              ? std::string_view::npos
                              : dot - start);
    // Every label (including the last one) is character-checked.
    if (label.empty() || label.front() == '-' || label.back() == '-') {
      return false;
    }
    for (char c : label) {
      if (!IsLowerHexishLabelChar(c)) {
        return false;
      }
    }
    ++labels;
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return labels >= 1;
}

bool ParsePathPrefix(const std::string& prefix, std::string* out) {
  if (prefix.empty() || prefix.front() != '/' ||
      prefix.find('\0') != std::string::npos) {
    return false;
  }
  *out = prefix;
  return true;
}

// `{hosts: string[1..8], pathPrefixes?: string[0..8]}`, unknown keys refused.
bool ParseUrlMatch(const base::DictValue& dict,
                   const char* what,
                   bool allow_scope,
                   CaptchaUrlMatch* out,
                   std::vector<CaptchaRulesError>* errors) {
  for (const auto [key, value] : dict) {
    if (key != "hosts" && key != "pathPrefixes" &&
        !(allow_scope && key == "scope")) {
      errors->push_back({{}, base::StrCat({what, ": unknown key ", key})});
      return false;
    }
  }
  const base::ListValue* hosts = dict.FindList("hosts");
  if (!hosts || hosts->empty() || hosts->size() > kMaxCaptchaHosts) {
    errors->push_back({{}, base::StrCat({what, ": hosts needs 1..8 entries"})});
    return false;
  }
  for (const base::Value& value : *hosts) {
    const std::string* host = value.GetIfString();
    if (!host) {
      errors->push_back({{}, base::StrCat({what, ": hosts must be strings"})});
      return false;
    }
    std::string lower = base::ToLowerASCII(*host);
    if (!IsValidHost(lower)) {
      errors->push_back({{}, base::StrCat({what, ": bad host ", *host})});
      return false;
    }
    out->hosts.push_back(std::move(lower));
  }
  if (const base::ListValue* prefixes = dict.FindList("pathPrefixes")) {
    if (prefixes->size() > kMaxCaptchaPathPrefixes) {
      errors->push_back(
          {{}, base::StrCat({what, ": too many pathPrefixes"})});
      return false;
    }
    for (const base::Value& value : *prefixes) {
      const std::string* prefix = value.GetIfString();
      std::string parsed;
      if (!prefix || !ParsePathPrefix(*prefix, &parsed)) {
        errors->push_back(
            {{},
             base::StrCat({what, ": every pathPrefix starts with /"})});
        return false;
      }
      out->path_prefixes.push_back(std::move(parsed));
    }
  }
  return true;
}

bool ParseMarker(const base::DictValue& dict,
                 bool allow_url,
                 const char* what,
                 CaptchaMarkerRule* out,
                 std::vector<CaptchaRulesError>* errors) {
  for (const auto [key, value] : dict) {
    const bool known = key == "text" || key == "selector" ||
                       (allow_url && key == "url");
    if (!known) {
      errors->push_back({{}, base::StrCat({what, ": unknown key ", key})});
      return false;
    }
  }
  if (const std::string* text = dict.FindString("text")) {
    if (!IsValidText(*text, kMaxCaptchaTextLen)) {
      errors->push_back(
          {{}, base::StrCat({what, ": text needs 1..200 chars, no NUL"})});
      return false;
    }
    out->text = *text;
  }
  if (const std::string* selector = dict.FindString("selector")) {
    if (!IsValidSelector(*selector)) {
      errors->push_back(
          {{}, base::StrCat({what, ": bad selector"})});
      return false;
    }
    out->selector = *selector;
  }
  if (const std::string* url = dict.FindString("url")) {
    if (!IsValidText(*url, kMaxCaptchaTextLen)) {
      errors->push_back(
          {{}, base::StrCat({what, ": url needs 1..200 chars, no NUL"})});
      return false;
    }
    out->url = *url;
  }
  if (out->empty()) {
    errors->push_back({{}, base::StrCat({what, ": needs one of text, selector",
                                         allow_url ? ", url" : ""})});
    return false;
  }
  return true;
}

// One rule object; on false `errors` got the reason (with the rule id when
// the id itself parsed).
bool ParseOneRule(const base::Value& value,
                  CaptchaRule* out,
                  std::vector<CaptchaRulesError>* errors) {
  const base::DictValue* dict = value.GetIfDict();
  if (!dict) {
    errors->push_back({{}, "every rule is an object"});
    return false;
  }
  auto fail = [&](std::string message) {
    errors->push_back({IsValidRuleId(out->id) ? out->id : std::string(),
                       std::move(message)});
    return false;
  };
  // The id first: every later message names the rule it belongs to.
  const std::string* id = dict->FindString("id");
  if (!id || !IsValidRuleId(*id)) {
    errors->push_back({{}, "bad rule id"});
    return false;
  }
  out->id = *id;
  for (const auto [key, val] : *dict) {
    if (key != "id" && key != "name" && key != "enabled" && key != "match" &&
        key != "detect" && key != "type" && key != "binding" &&
        key != "success" && key != "failure" && key != "travelMode") {
      return fail(base::StrCat({"unknown key ", key}));
    }
  }
  if (const std::string* name = dict->FindString("name")) {
    if (name->size() > kMaxCaptchaRuleNameLen ||
        name->find('\0') != std::string::npos) {
      return fail("name is at most 80 chars, no NUL");
    }
    out->name = *name;
  }
  const std::optional<bool> enabled = dict->FindBool("enabled");
  if (!enabled.has_value()) {
    return fail("enabled is required");
  }
  out->enabled = *enabled;
  const std::string* type = dict->FindString("type");
  if (!type || *type != "puzzle-slide") {
    return fail("type must be \"puzzle-slide\"");
  }
  if (const std::string* mode = dict->FindString("travelMode")) {
    if (*mode == "ratio") {
      out->travel_direct = false;
    } else if (*mode == "direct") {
      out->travel_direct = true;
    } else {
      return fail("travelMode is ratio or direct");
    }
  }

  const base::DictValue* match = dict->FindDict("match");
  if (!match) {
    return fail("match is required");
  }
  for (const auto [key, val] : *match) {
    if (key != "page" && key != "frame") {
      return fail(base::StrCat({"match: unknown key ", key}));
    }
  }
  const base::DictValue* page = match->FindDict("page");
  const base::DictValue* frame = match->FindDict("frame");
  if (!page && !frame) {
    return fail("match needs page and/or frame");
  }
  if (page) {
    out->has_page = true;
    if (!ParseUrlMatch(*page, "match.page", /*allow_scope=*/false, &out->page,
                       errors)) {
      return fail("bad match.page");
    }
  }
  if (frame) {
    out->has_frame = true;
    const std::string* scope = frame->FindString("scope");
    if (!scope || (*scope != "main" && *scope != "iframe")) {
      return fail("match.frame.scope is main or iframe");
    }
    out->frame_scope_main = *scope == "main";
    if (!ParseUrlMatch(*frame, "match.frame", /*allow_scope=*/true,
                       &out->frame, errors)) {
      return fail("bad match.frame");
    }
  }

  const base::DictValue* detect = dict->FindDict("detect");
  if (!detect) {
    return fail("detect is required");
  }
  for (const auto [key, val] : *detect) {
    if (key != "root" && key != "requiredSelectors" &&
        key != "excludedSelectors" && key != "requireVisible") {
      return fail(base::StrCat({"detect: unknown key ", key}));
    }
  }
  const std::string* root = detect->FindString("root");
  if (!root || !IsValidSelector(*root)) {
    return fail("detect.root needs a selector of at most 200 chars");
  }
  out->detect.root = *root;
  if (const base::ListValue* req = detect->FindList("requiredSelectors")) {
    if (req->size() > kMaxCaptchaSelectors) {
      return fail("detect.requiredSelectors: at most 8");
    }
    for (const base::Value& item : *req) {
      const std::string* selector = item.GetIfString();
      if (!selector || !IsValidSelector(*selector)) {
        return fail("detect.requiredSelectors: bad selector");
      }
      out->detect.required_selectors.push_back(*selector);
    }
  }
  if (const base::ListValue* exc = detect->FindList("excludedSelectors")) {
    if (exc->size() > kMaxCaptchaSelectors) {
      return fail("detect.excludedSelectors: at most 8");
    }
    for (const base::Value& item : *exc) {
      const std::string* selector = item.GetIfString();
      if (!selector || !IsValidSelector(*selector)) {
        return fail("detect.excludedSelectors: bad selector");
      }
      out->detect.excluded_selectors.push_back(*selector);
    }
  }
  if (const std::optional<bool> visible = detect->FindBool("requireVisible")) {
    out->detect.require_visible = *visible;
  }

  const base::DictValue* binding = dict->FindDict("binding");
  if (!binding) {
    return fail("binding is required");
  }
  for (const auto [key, val] : *binding) {
    if (key != "background" && key != "piece" && key != "handle" &&
        key != "track" && key != "instruction") {
      return fail(base::StrCat({"binding: unknown key ", key}));
    }
  }
  struct {
    const char* key;
    std::string CaptchaBinding::*dest;
  } kBindingRoles[] = {
      {"background", &CaptchaBinding::background},
      {"piece", &CaptchaBinding::piece},
      {"handle", &CaptchaBinding::handle},
      {"track", &CaptchaBinding::track},
  };
  for (const auto& role : kBindingRoles) {
    const std::string* selector = binding->FindString(role.key);
    if (!selector || !IsValidSelector(*selector)) {
      return fail(base::StrCat({"binding.", role.key, ": bad selector"}));
    }
    out->binding.*(role.dest) = *selector;
  }
  if (const std::string* instruction = binding->FindString("instruction")) {
    if (!IsValidSelector(*instruction)) {
      return fail("binding.instruction: bad selector");
    }
    out->binding.instruction = *instruction;
  }

  if (const base::DictValue* success = dict->FindDict("success")) {
    out->has_success = true;
    if (!ParseMarker(*success, /*allow_url=*/true, "success", &out->success,
                     errors)) {
      return fail("bad success");
    }
  }
  if (const base::DictValue* failure = dict->FindDict("failure")) {
    out->has_failure = true;
    if (!ParseMarker(*failure, /*allow_url=*/false, "failure", &out->failure,
                     errors)) {
      return fail("bad failure");
    }
  }
  return true;
}

base::ListValue ToStringList(const std::vector<std::string>& values) {
  base::ListValue list;
  for (const std::string& value : values) {
    list.Append(value);
  }
  return list;
}

}  // namespace

// Chromium style: classes with non-trivial members declared in a header get
// their special members defined out of line.
CaptchaUrlMatch::CaptchaUrlMatch() = default;
CaptchaUrlMatch::CaptchaUrlMatch(const CaptchaUrlMatch&) = default;
CaptchaUrlMatch& CaptchaUrlMatch::operator=(const CaptchaUrlMatch&) = default;
CaptchaUrlMatch::CaptchaUrlMatch(CaptchaUrlMatch&&) = default;
CaptchaUrlMatch& CaptchaUrlMatch::operator=(CaptchaUrlMatch&&) = default;
CaptchaUrlMatch::~CaptchaUrlMatch() = default;

CaptchaDetectSpec::CaptchaDetectSpec() = default;
CaptchaDetectSpec::CaptchaDetectSpec(const CaptchaDetectSpec&) = default;
CaptchaDetectSpec& CaptchaDetectSpec::operator=(
    const CaptchaDetectSpec&) = default;
CaptchaDetectSpec::CaptchaDetectSpec(CaptchaDetectSpec&&) = default;
CaptchaDetectSpec& CaptchaDetectSpec::operator=(CaptchaDetectSpec&&) = default;
CaptchaDetectSpec::~CaptchaDetectSpec() = default;

CaptchaBinding::CaptchaBinding() = default;
CaptchaBinding::CaptchaBinding(const CaptchaBinding&) = default;
CaptchaBinding& CaptchaBinding::operator=(const CaptchaBinding&) = default;
CaptchaBinding::CaptchaBinding(CaptchaBinding&&) = default;
CaptchaBinding& CaptchaBinding::operator=(CaptchaBinding&&) = default;
CaptchaBinding::~CaptchaBinding() = default;

CaptchaMarkerRule::CaptchaMarkerRule() = default;
CaptchaMarkerRule::CaptchaMarkerRule(const CaptchaMarkerRule&) = default;
CaptchaMarkerRule& CaptchaMarkerRule::operator=(
    const CaptchaMarkerRule&) = default;
CaptchaMarkerRule::CaptchaMarkerRule(CaptchaMarkerRule&&) = default;
CaptchaMarkerRule& CaptchaMarkerRule::operator=(CaptchaMarkerRule&&) = default;
CaptchaMarkerRule::~CaptchaMarkerRule() = default;

CaptchaRule::CaptchaRule() = default;
CaptchaRule::CaptchaRule(const CaptchaRule&) = default;
CaptchaRule& CaptchaRule::operator=(const CaptchaRule&) = default;
CaptchaRule::CaptchaRule(CaptchaRule&&) = default;
CaptchaRule& CaptchaRule::operator=(CaptchaRule&&) = default;
CaptchaRule::~CaptchaRule() = default;

bool CaptchaHostMatches(std::string_view pattern, std::string_view host) {
  if (pattern.starts_with("*.")) {
    const std::string_view rest = pattern.substr(2);  // "example.com"
    // Suffix only, with one label or more before it: "*.example.com" matches
    // "a.example.com", never the apex "example.com" (contract section 1).
    return host.size() > rest.size() + 1 && host.ends_with(rest) &&
           host[host.size() - rest.size() - 1] == '.';
  }
  return pattern == host;
}

bool CaptchaUrlMatchHits(const CaptchaUrlMatch& match,
                         std::string_view host,
                         std::string_view path) {
  if (!match.path_prefixes.empty()) {
    std::string_view normalized = path.empty() ? std::string_view("/") : path;
    bool path_hit = false;
    for (const std::string& prefix : match.path_prefixes) {
      if (normalized.starts_with(prefix)) {
        path_hit = true;
        break;
      }
    }
    if (!path_hit) {
      return false;
    }
  }
  for (const std::string& pattern : match.hosts) {
    if (CaptchaHostMatches(pattern, host)) {
      return true;
    }
  }
  return false;
}

bool CaptchaRuleAppliesToFrame(const CaptchaRule& rule,
                               bool is_main_frame,
                               std::string_view frame_host,
                               std::string_view frame_path,
                               std::string_view page_host,
                               std::string_view page_path) {
  if (rule.has_page &&
      !CaptchaUrlMatchHits(rule.page, page_host, page_path)) {
    return false;
  }
  if (is_main_frame) {
    if (rule.has_frame) {
      return rule.frame_scope_main &&
             CaptchaUrlMatchHits(rule.frame, frame_host, frame_path);
    }
    return rule.has_page;
  }
  if (rule.has_frame) {
    return !rule.frame_scope_main &&
           CaptchaUrlMatchHits(rule.frame, frame_host, frame_path);
  }
  // page-only rules check the main frame only.
  return false;
}

bool ParseCaptchaRules(const base::DictValue& args,
                       int* revision,
                       std::vector<CaptchaRule>* rules,
                       std::vector<CaptchaRulesError>* errors) {
  for (const auto [key, value] : args) {
    if (key != "revision" && key != "rules") {
      errors->push_back({{}, base::StrCat({"unknown key ", key})});
      return false;
    }
  }
  const std::optional<int> parsed_revision = args.FindInt("revision");
  const base::ListValue* list = args.FindList("rules");
  if (!parsed_revision || parsed_revision.value() < 1 || !list) {
    errors->push_back(
        {{}, "revision needs an integer >= 1 and rules a list"});
    return false;
  }
  if (list->size() > kMaxCaptchaRules) {
    errors->push_back({{}, "at most 32 rules per snapshot"});
    return false;
  }
  // `Value` has no const-ListValue ctor; `ValueView` (json_writer.h:46,
  // values.h:976) accepts the list directly.
  const std::optional<std::string> json = base::WriteJson(*list);
  if (!json || json->size() > kMaxCaptchaRulesJsonBytes) {
    errors->push_back({{}, "the rules JSON is over 64 KB"});
    return false;
  }
  std::vector<CaptchaRule> parsed;
  for (const base::Value& value : *list) {
    CaptchaRule rule;
    const size_t errors_before = errors->size();
    if (!ParseOneRule(value, &rule, errors)) {
      if (errors->size() == errors_before) {
        errors->push_back({{}, "bad rule"});
      }
      continue;
    }
    if (std::any_of(parsed.begin(), parsed.end(),
                    [&](const CaptchaRule& other) {
                      return other.id == rule.id;
                    })) {
      errors->push_back({rule.id, "duplicate rule id"});
      rule.id.clear();
      continue;
    }
    parsed.push_back(std::move(rule));
  }
  if (!errors->empty()) {
    return false;
  }
  *revision = parsed_revision.value();
  *rules = std::move(parsed);
  return true;
}

std::string SerializeCaptchaRules(const std::vector<CaptchaRule>& rules) {
  base::ListValue list;
  for (const CaptchaRule& rule : rules) {
    base::DictValue dict;
    dict.Set("id", rule.id);
    if (!rule.name.empty()) {
      dict.Set("name", rule.name);
    }
    dict.Set("enabled", rule.enabled);
    base::DictValue match;
    if (rule.has_page) {
      base::DictValue page;
      page.Set("hosts", ToStringList(rule.page.hosts));
      if (!rule.page.path_prefixes.empty()) {
        page.Set("pathPrefixes", ToStringList(rule.page.path_prefixes));
      }
      match.Set("page", std::move(page));
    }
    if (rule.has_frame) {
      base::DictValue frame;
      frame.Set("scope", rule.frame_scope_main ? "main" : "iframe");
      frame.Set("hosts", ToStringList(rule.frame.hosts));
      if (!rule.frame.path_prefixes.empty()) {
        frame.Set("pathPrefixes", ToStringList(rule.frame.path_prefixes));
      }
      match.Set("frame", std::move(frame));
    }
    dict.Set("match", std::move(match));
    base::DictValue detect;
    detect.Set("root", rule.detect.root);
    detect.Set("requiredSelectors",
               ToStringList(rule.detect.required_selectors));
    detect.Set("excludedSelectors",
               ToStringList(rule.detect.excluded_selectors));
    detect.Set("requireVisible", rule.detect.require_visible);
    dict.Set("detect", std::move(detect));
    dict.Set("type", "puzzle-slide");
    base::DictValue binding;
    binding.Set("background", rule.binding.background);
    binding.Set("piece", rule.binding.piece);
    binding.Set("handle", rule.binding.handle);
    binding.Set("track", rule.binding.track);
    if (!rule.binding.instruction.empty()) {
      binding.Set("instruction", rule.binding.instruction);
    }
    dict.Set("binding", std::move(binding));
    if (rule.has_success) {
      base::DictValue success;
      if (!rule.success.text.empty()) {
        success.Set("text", rule.success.text);
      }
      if (!rule.success.selector.empty()) {
        success.Set("selector", rule.success.selector);
      }
      if (!rule.success.url.empty()) {
        success.Set("url", rule.success.url);
      }
      dict.Set("success", std::move(success));
    }
    if (rule.has_failure) {
      base::DictValue failure;
      if (!rule.failure.text.empty()) {
        failure.Set("text", rule.failure.text);
      }
      if (!rule.failure.selector.empty()) {
        failure.Set("selector", rule.failure.selector);
      }
      dict.Set("failure", std::move(failure));
    }
    if (rule.travel_direct) {
      dict.Set("travelMode", "direct");
    }
    list.Append(std::move(dict));
  }
  return base::WriteJson(list).value_or("[]");
}

std::string CanonicalRuleJson(const CaptchaRule& rule) {
  return SerializeCaptchaRules(std::vector<CaptchaRule>{rule});
}

std::string CaptchaCheckSpecJson(
    const std::vector<const CaptchaRule*>& rules) {
  base::ListValue list;
  for (const CaptchaRule* rule : rules) {
    base::DictValue dict;
    dict.Set("id", rule->id);
    dict.Set("root", rule->detect.root);
    dict.Set("req", ToStringList(rule->detect.required_selectors));
    dict.Set("exc", ToStringList(rule->detect.excluded_selectors));
    dict.Set("vis", rule->detect.require_visible);
    base::DictValue binding;
    binding.Set("background", rule->binding.background);
    binding.Set("piece", rule->binding.piece);
    binding.Set("handle", rule->binding.handle);
    binding.Set("track", rule->binding.track);
    binding.Set("instruction", rule->binding.instruction);
    dict.Set("b", std::move(binding));
    list.Append(std::move(dict));
  }
  return base::WriteJson(list).value_or("[]");
}

std::string QuoteAsJsonLiteral(std::string_view value) {
  return base::GetQuotedJSONString(value);
}

std::optional<CaptchaSlideMath> BuildSlidePlan(double slide_x_proportion,
                                               double background_width,
                                               double piece_width,
                                               double handle_width,
                                               double handle_center_x,
                                               double handle_center_y,
                                               double track_width,
                                               bool travel_direct) {
  if (!std::isfinite(slide_x_proportion) || slide_x_proportion < 0.0 ||
      slide_x_proportion > 1.0 || !std::isfinite(background_width) ||
      background_width <= 0.0 || !std::isfinite(piece_width) ||
      piece_width < 0.0 || !std::isfinite(handle_width) ||
      handle_width < 0.0 || !std::isfinite(track_width) ||
      track_width <= 0.0 || !std::isfinite(handle_center_x) ||
      !std::isfinite(handle_center_y)) {
    return std::nullopt;
  }
  const double piece_target_x = slide_x_proportion * background_width;
  double travel = piece_target_x;  // "direct", and the ratio fallback
  if (!travel_direct) {
    const double denominator = background_width - piece_width;
    if (denominator > 0.0) {
      travel = piece_target_x * (track_width - handle_width) / denominator;
    }
  }
  const double max_travel = track_width - handle_width;
  travel = std::clamp(travel, 0.0, std::max(0.0, max_travel));
  CaptchaSlideMath plan;
  plan.from_x = handle_center_x;
  plan.from_y = handle_center_y;
  plan.to_x = handle_center_x + travel;
  plan.to_y = handle_center_y;
  return plan;
}

}  // namespace shuttle

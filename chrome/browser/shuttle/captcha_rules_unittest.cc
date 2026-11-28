// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/captcha_rules.h"

#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace shuttle {
namespace {

base::DictValue ParseDict(const std::string& json) {
  std::optional<base::Value> value =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  CHECK(value && value->is_dict());
  return std::move(value->GetDict());
}

const char kGoodRuleJson[] = R"({
  "id": "example-slide",
  "name": "Example puzzle",
  "enabled": true,
  "match": {
    "page": {"hosts": ["verify.example.com", "*.captcha.example.org"],
             "pathPrefixes": ["/check"]},
    "frame": {"scope": "iframe", "hosts": ["*.example.com"]}
  },
  "detect": {
    "root": "div.pz",
    "requiredSelectors": [".bg", ".piece"],
    "excludedSelectors": [".ok"],
    "requireVisible": true
  },
  "type": "puzzle-slide",
  "binding": {"background": ".bg", "piece": ".piece", "handle": ".hd",
              "track": ".tr", "instruction": ".tip"},
  "success": {"text": "ok", "url": "/done"},
  "failure": {"selector": ".no"},
  "travelMode": "ratio"
})";

base::DictValue PageOnlyArgs(const char* hosts_json) {
  return ParseDict(base::StrCat(
      {R"({"revision":1,"rules":[{"id":"x","enabled":true,"match":{"page":{)",
       R"("hosts":)", hosts_json,
       R"(}},"detect":{"root":"x"},"type":"puzzle-slide","binding":{)",
       R"("background":"b","piece":"p","handle":"h","track":"t"}}}]})"}));
}

// --- ParseCaptchaRules ----------------------------------------------------------

TEST(CaptchaRulesParseTest, ValidSnapshot) {
  base::DictValue args =
      base::DictValue().Set("revision", 7).Set("rules", base::ListValue());
  args.FindList("rules")->Append(base::Value(ParseDict(kGoodRuleJson)));
  int revision = 0;
  std::vector<CaptchaRule> rules;
  std::vector<CaptchaRulesError> errors;
  ASSERT_TRUE(ParseCaptchaRules(args, &revision, &rules, &errors))
      << (errors.empty() ? "" : errors.front().error);
  EXPECT_EQ(revision, 7);
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_EQ(rules[0].id, "example-slide");
  EXPECT_TRUE(rules[0].enabled);
  EXPECT_TRUE(rules[0].has_page);
  ASSERT_EQ(rules[0].page.hosts.size(), 2u);
  EXPECT_TRUE(rules[0].has_frame);
  EXPECT_FALSE(rules[0].frame_scope_main);
  ASSERT_EQ(rules[0].detect.required_selectors.size(), 2u);
  EXPECT_EQ(rules[0].binding.handle, ".hd");
  EXPECT_EQ(rules[0].success.url, "/done");
  EXPECT_FALSE(rules[0].travel_direct);
  // Canonical serialization round-trips through the parser.
  const std::string canonical = SerializeCaptchaRules(rules);
  base::DictValue again = base::DictValue()
                              .Set("revision", 8)
                              .Set("rules", base::ListValue());
  std::optional<base::Value> list =
      base::JSONReader::Read(canonical, base::JSON_PARSE_RFC);
  ASSERT_TRUE(list && list->is_list());
  again.FindList("rules")->Append(std::move(list->GetList()[0]));
  int revision2 = 0;
  std::vector<CaptchaRule> rules2;
  std::vector<CaptchaRulesError> errors2;
  EXPECT_TRUE(ParseCaptchaRules(again, &revision2, &rules2, &errors2));
}

TEST(CaptchaRulesParseTest, UnknownKeysRejected) {
  base::DictValue rule = ParseDict(kGoodRuleJson);
  rule.Set("policy", base::DictValue().Set("mode", "auto-solve"));
  base::DictValue args = base::DictValue().Set("revision", 1);
  base::ListValue rules;
  rules.Append(std::move(rule));
  args.Set("rules", std::move(rules));
  int revision = 0;
  std::vector<CaptchaRule> parsed;
  std::vector<CaptchaRulesError> errors;
  EXPECT_FALSE(ParseCaptchaRules(args, &revision, &parsed, &errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_EQ(errors.front().rule_id, "example-slide");
}

TEST(CaptchaRulesParseTest, BadIdsAndSelectorsRejected) {
  struct {
    const char* json;
  } cases[] = {
      // id: uppercase.
      {R"({"id":"Bad","enabled":true,"match":{"page":{"hosts":["a.com"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // id: leading hyphen.
      {R"({"id":"-x","enabled":true,"match":{"page":{"hosts":["a.com"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // selector with a control character (escaped NUL).
      {R"({"id":"x","enabled":true,"match":{"page":{"hosts":["a.com"]}},
          "detect":{"root":"a\u0000b"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // host with a port.
      {R"({"id":"x","enabled":true,
          "match":{"page":{"hosts":["a.com:8443"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
           "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // Host label has an invalid character in the final label.
      {R"({"id":"x","enabled":true,
          "match":{"page":{"hosts":["a.co_m"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // Final label ends with a hyphen.
      {R"({"id":"x","enabled":true,
          "match":{"page":{"hosts":["a.com-"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // path prefix without a leading slash.
      {R"({"id":"x","enabled":true,
          "match":{"page":{"hosts":["a.com"],"pathPrefixes":["check"]}},
          "detect":{"root":"x"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // wrong type.
      {R"({"id":"x","enabled":true,"match":{"page":{"hosts":["a.com"]}},
          "detect":{"root":"x"},"type":"image-click",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
      // a "javascript" field is not a rule key at all.
      {R"({"id":"x","enabled":true,"match":{"page":{"hosts":["a.com"]}},
          "detect":{"root":"x","javascript":"alert(1)"},"type":"puzzle-slide",
          "binding":{"background":"b","piece":"p","handle":"h","track":"t"}})"},
  };
  for (const auto& test : cases) {
    base::DictValue args = base::DictValue().Set("revision", 1);
    base::ListValue rules;
    rules.Append(base::Value(ParseDict(test.json)));
    args.Set("rules", std::move(rules));
    int revision = 0;
    std::vector<CaptchaRule> parsed;
    std::vector<CaptchaRulesError> errors;
    EXPECT_FALSE(ParseCaptchaRules(args, &revision, &parsed, &errors))
        << test.json;
  }
}

TEST(CaptchaRulesParseTest, DuplicateIdsAndBadRevision) {
  base::DictValue args = base::DictValue().Set("revision", 0);
  base::ListValue rules;
  args.Set("rules", std::move(rules));
  int revision = 0;
  std::vector<CaptchaRule> parsed;
  std::vector<CaptchaRulesError> errors;
  EXPECT_FALSE(ParseCaptchaRules(args, &revision, &parsed, &errors));

  base::DictValue rule = ParseDict(kGoodRuleJson);
  base::DictValue twice = base::DictValue().Set("revision", 1);
  base::ListValue list;
  list.Append(base::Value(rule.Clone()));
  list.Append(std::move(rule));
  twice.Set("rules", std::move(list));
  errors.clear();
  EXPECT_FALSE(ParseCaptchaRules(twice, &revision, &parsed, &errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_EQ(errors.front().error, "duplicate rule id");
}

// --- Host and URL matching --------------------------------------------------------

TEST(CaptchaRulesMatchTest, Hosts) {
  EXPECT_TRUE(CaptchaHostMatches("verify.example.com", "verify.example.com"));
  EXPECT_FALSE(
      CaptchaHostMatches("verify.example.com", "xverify.example.com"));
  EXPECT_TRUE(CaptchaHostMatches("*.example.com", "a.example.com"));
  EXPECT_TRUE(CaptchaHostMatches("*.example.com", "a.b.example.com"));
  EXPECT_FALSE(CaptchaHostMatches("*.example.com", "example.com"));  // no apex
  EXPECT_FALSE(CaptchaHostMatches("*.example.com", "notexample.com"));
  EXPECT_FALSE(CaptchaHostMatches("*.example.com", "axexample.com"));
  EXPECT_FALSE(CaptchaHostMatches("*.example.com", "example.org"));
}

TEST(CaptchaRulesMatchTest, InvalidHostLabelsRejected) {
  for (const char* host : {"a.co_m", "a.com-", "a.-com", "a..com", "a."}) {
    base::DictValue args = PageOnlyArgs(base::StrCat({"[\"", host, "\"]"}).c_str());
    int revision = 0;
    std::vector<CaptchaRule> rules;
    std::vector<CaptchaRulesError> errors;
    EXPECT_FALSE(ParseCaptchaRules(args, &revision, &rules, &errors)) << host;
  }
}

TEST(CaptchaRulesMatchTest, Paths) {
  CaptchaUrlMatch match;
  match.hosts = {"a.com"};
  EXPECT_TRUE(CaptchaUrlMatchHits(match, "a.com", ""));  // "" reads as "/"
  match.path_prefixes = {"/check"};
  EXPECT_TRUE(CaptchaUrlMatchHits(match, "a.com", "/check"));
  EXPECT_TRUE(CaptchaUrlMatchHits(match, "a.com", "/check?id=1"));
  EXPECT_FALSE(CaptchaUrlMatchHits(match, "a.com", "/other"));
  EXPECT_FALSE(CaptchaUrlMatchHits(match, "a.com", "/notcheck"));
}

TEST(CaptchaRulesMatchTest, RuleAppliesToFrame) {
  int revision = 0;
  std::vector<CaptchaRule> rules;
  std::vector<CaptchaRulesError> errors;
  ASSERT_TRUE(ParseCaptchaRules(PageOnlyArgs(R"(["a.com"])"), &revision,
                                &rules, &errors));
  ASSERT_EQ(rules.size(), 1u);
  // page-only rules address the main frame only.
  EXPECT_TRUE(CaptchaRuleAppliesToFrame(rules[0], true, "a.com", "/", "a.com",
                                        "/"));
  EXPECT_FALSE(CaptchaRuleAppliesToFrame(rules[0], false, "a.com", "/", "a.com",
                                         "/"));
  EXPECT_FALSE(CaptchaRuleAppliesToFrame(rules[0], true, "b.com", "/", "a.com",
                                         "/"));
}

// --- QuoteAsJsonLiteral -------------------------------------------------------------

TEST(CaptchaRulesEscapeTest, QuoteAsJsonLiteral) {
  // The quote and the backslash are escaped, so the value can never close the
  // literal it is placed into.
  EXPECT_EQ(QuoteAsJsonLiteral("\"alert(1)//\""),
            "\"\\\"alert(1)//\\\"\"");
  EXPECT_EQ(QuoteAsJsonLiteral("a\nb"), "\"a\\nb\"");
  // The check spec keeps every selector inside one literal (JSON.parse input):
  std::vector<const CaptchaRule*> refs;
  CaptchaRule rule;
  rule.id = "x";
  rule.detect.root = "div['\"]";
  refs.push_back(&rule);
  const std::string spec = CaptchaCheckSpecJson(refs);
  const std::string literal = QuoteAsJsonLiteral(spec);
  EXPECT_EQ(literal.front(), '"');
  EXPECT_EQ(literal.back(), '"');
  EXPECT_EQ(literal.find('\n'), std::string::npos);  // no raw newlines
}

// --- BuildSlidePlan ------------------------------------------------------------------

TEST(CaptchaRulesPlanTest, ContractMaths) {
  // bg 300 wide, piece 50, handle 40, track 300: ratio travel for p=0.5 is
  // 150 * (300 - 40) / (300 - 50) = 156; from the handle centre (140, 20).
  const std::optional<CaptchaSlideMath> ratio =
      BuildSlidePlan(0.5, 300.0, 50.0, 40.0, 140.0, 20.0, 300.0, false);
  ASSERT_TRUE(ratio.has_value());
  EXPECT_DOUBLE_EQ(ratio->from_x, 140.0);
  EXPECT_DOUBLE_EQ(ratio->from_y, 20.0);
  EXPECT_DOUBLE_EQ(ratio->to_x, 140.0 + 156.0);
  EXPECT_EQ(ratio->moves, 14);
  EXPECT_EQ(ratio->hold_ms, 120);

  // direct: travel = 150, clamped into [0, track - handle] = [0, 260].
  const std::optional<CaptchaSlideMath> direct =
      BuildSlidePlan(0.5, 300.0, 50.0, 40.0, 140.0, 20.0, 300.0, true);
  ASSERT_TRUE(direct.has_value());
  EXPECT_DOUBLE_EQ(direct->to_x, 290.0);

  // p = 1 ratio: 300 * 260 / 250 = 312 -> clamp to 260.
  const std::optional<CaptchaSlideMath> maxed =
      BuildSlidePlan(1.0, 300.0, 50.0, 40.0, 140.0, 20.0, 300.0, false);
  ASSERT_TRUE(maxed.has_value());
  EXPECT_DOUBLE_EQ(maxed->to_x, 140.0 + 260.0);

  // Degenerate background == piece: ratio falls back to direct.
  const std::optional<CaptchaSlideMath> fallback =
      BuildSlidePlan(0.5, 300.0, 300.0, 40.0, 140.0, 20.0, 300.0, false);
  ASSERT_TRUE(fallback.has_value());
  EXPECT_DOUBLE_EQ(fallback->to_x, 290.0);

  // Out of range or non-finite input: no plan.
  EXPECT_FALSE(BuildSlidePlan(1.5, 300.0, 50.0, 40.0, 140.0, 20.0, 300.0,
                              false)
                   .has_value());
  EXPECT_FALSE(
      BuildSlidePlan(0.5, 0.0, 50.0, 40.0, 140.0, 20.0, 300.0, false)
          .has_value());
}

}  // namespace
}  // namespace shuttle

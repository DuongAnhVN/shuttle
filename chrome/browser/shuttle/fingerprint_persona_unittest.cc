#include "chrome/browser/shuttle/fingerprint_persona.h"

#include "base/version_info/version_info.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace shuttle {
namespace {

TEST(FingerprintPersonaTest, GeneratedVersionMatchesEngine) {
  auto persona = GenerateFingerprintPersona("windows-chrome", 123);
  EXPECT_EQ(persona.browser_version, version_info::GetVersionNumber());
  EXPECT_TRUE(CheckFingerprintPersona(persona).empty());
}

TEST(FingerprintPersonaTest, MigratesStoredVersionAndPreservesIdentity) {
  auto original = GenerateFingerprintPersona("windows-chrome", 123);
  auto dict = FingerprintPersonaToPrefValue(original);
  dict.FindDict("browser")->Set("version", "99.0.1234.0");
  dict.Set("user_agent",
           "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/99.0.0.0 Safari/537.36");
  base::Value pref(std::move(dict));
  auto result = FingerprintPersonaFromPrefValue(&pref);
  ASSERT_TRUE(result.persona);
  EXPECT_TRUE(result.version_migrated);
  EXPECT_EQ(original, *result.persona);
}

TEST(FingerprintPersonaTest, InconsistentStoredVersionRemainsCorrupt) {
  auto dict = FingerprintPersonaToPrefValue(
      GenerateFingerprintPersona("windows-chrome", 123));
  dict.FindDict("browser")->Set("version", "99.0.1234.0");
  dict.Set("user_agent", "invalid");
  base::Value pref(std::move(dict));
  auto result = FingerprintPersonaFromPrefValue(&pref);
  EXPECT_EQ(StoredFingerprintPersona::State::kCorrupt, result.state);
  EXPECT_FALSE(result.version_migrated);
  EXPECT_FALSE(result.persona);
}

TEST(FingerprintPersonaTest, RejectsMalformedAndForeignInputVersions) {
  for (const char* version : {"abc", "99.0.1234.0"}) {
    base::Value input(base::DictValue().Set("preset", "windows-chrome")
                                       .Set("browserVersion", version));
    FingerprintPersona out;
    EXPECT_FALSE(ParseFingerprintPersonaInput(input, nullptr, 123, &out).empty());
  }
}

}  // namespace
}  // namespace shuttle

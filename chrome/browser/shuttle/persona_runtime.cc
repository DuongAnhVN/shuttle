// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/persona_runtime.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <memory>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "components/embedder_support/user_agent_utils.h"
#include "net/http/http_util.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/common/user_agent/user_agent_brand_version_type.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

namespace shuttle {

namespace {

// The brand a persona's "Chrome" is announced as in Sec-CH-UA (the persona
// schema has no other brand). "Chromium" and a GREASE brand are added by
// embedder_support::GenerateBrandVersionList, as in a stock browser.
constexpr char kClientHintBrand[] = "Google Chrome";

int MajorVersion(const std::string& version) {
  int major = 0;
  base::StringToInt(version.substr(0, version.find('.')), &major);
  return major;
}

}  // namespace

blink::UserAgentOverride UserAgentOverrideForPersona(
    const FingerprintPersona& persona) {
  const int major = MajorVersion(persona.browser_version);
  const std::string major_text = base::NumberToString(major);

  blink::UserAgentMetadata metadata;
  // The seed (the major version, as in a stock browser) orders the brand list
  // and picks the GREASE brand, so the list is stable for a persona.
  metadata.brand_version_list = embedder_support::GenerateBrandVersionList(
      major, kClientHintBrand, major_text,
      blink::UserAgentBrandVersionType::kMajorVersion);
  metadata.brand_full_version_list = embedder_support::GenerateBrandVersionList(
      major, kClientHintBrand, persona.browser_version,
      blink::UserAgentBrandVersionType::kFullVersion);
  metadata.full_version = persona.browser_version;
  metadata.platform = persona.client_hints.platform;
  metadata.platform_version = persona.client_hints.platform_version;
  metadata.architecture = persona.client_hints.architecture;
  metadata.model = std::string();
  metadata.mobile = persona.client_hints.mobile;
  metadata.bitness = persona.client_hints.bitness;
  metadata.wow64 = false;
  metadata.form_factors = {blink::kDesktopFormFactor};

  blink::UserAgentOverride override_value;
  override_value.ua_string_override = persona.user_agent;
  override_value.ua_metadata_override = std::move(metadata);
  return override_value;
}

bool IsKnownTimezoneId(const std::string& timezone) {
  // The same test Blink's TimeZoneController applies before it accepts a zone
  // (an unknown id yields ICU's "Etc/Unknown"): a zone Blink would refuse is
  // refused here, so the persona never stores a zone the page cannot show.
  if (timezone.empty()) {
    return false;
  }
  std::unique_ptr<icu::TimeZone> zone(icu::TimeZone::createTimeZone(
      icu::UnicodeString(timezone.c_str(), -1, US_INV)));
  return zone && !(*zone == icu::TimeZone::getUnknown());
}

std::string AcceptLanguageHeaderForPersona(const FingerprintPersona& persona) {
  // Same derivation as ProfileNetworkContextService::ComputeAcceptLanguage:
  // add the bare languages ("en" after "en-US" is already there) and the
  // q-values.
  return net::HttpUtil::GenerateAcceptLanguageHeader(
      net::HttpUtil::ExpandLanguageList(
          RendererAcceptLanguagesForPersona(persona)));
}

std::string RendererAcceptLanguagesForPersona(
    const FingerprintPersona& persona) {
  return base::JoinString(persona.languages, ",");
}

const char* HostPersonaOs() {
#if BUILDFLAG(IS_WIN)
  return "windows";
#elif BUILDFLAG(IS_MAC)
  return "macos";
#elif BUILDFLAG(IS_LINUX)
  return "linux";
#else
  // Android, iOS, Fuchsia, BSD...: personas only describe windows, macos and
  // linux (IsKnownFingerprintPreset), so none can match; WebGPU is blocked
  // there (fail closed) rather than exposing an adapter that could contradict
  // the persona.
  return "";
#endif
}

bool IsWebGpuBlockedForPersona(const FingerprintPersona& persona) {
  // The host adapter is real: its vendor, architecture and description
  // (GPUAdapterInfo), limits and features can't be edited. It is exposed only
  // where it can't contradict the persona; otherwise requestAdapter() gives
  // null. Incompatible: the persona is for another OS than the host (the
  // adapter's backend and GPU class belong to the host), or it carries a
  // WebGL GPU string (it claims a GPU the adapter is not known to be).
  return persona.os != HostPersonaOs() || !persona.webgl.empty();
}

void ApplyPersonaToRendererPrefs(const FingerprintPersona& persona,
                                 blink::RendererPreferences* prefs) {
  prefs->user_agent_override = UserAgentOverrideForPersona(persona);
  prefs->accept_languages = RendererAcceptLanguagesForPersona(persona);
  prefs->navigator_platform_override = persona.platform;
  prefs->hardware_concurrency_override =
      persona.hardware_concurrency > 0
          ? static_cast<uint32_t>(persona.hardware_concurrency)
          : 0;
  prefs->device_memory_override = static_cast<float>(persona.device_memory);
  prefs->screen_width_override = persona.screen.width;
  prefs->screen_height_override = persona.screen.height;
  prefs->screen_color_depth_override = persona.screen.color_depth;
  prefs->device_pixel_ratio_override =
      static_cast<float>(persona.screen.device_pixel_ratio);
  // Applied by Blink as the renderer process' ICU default time zone and locale
  // (Page::SetRendererPreferences); Date, Intl.* and the process' dedicated
  // workers follow.
  prefs->timezone_override = persona.timezone;
  prefs->locale_override = persona.locale;
  // Optional: both strings or neither (CheckFingerprintPersona), so the
  // renderer never reports a half persona GPU. Always assigned, so a persona
  // without them clears the previous one's.
  prefs->webgl_unmasked_vendor_override = persona.webgl.vendor;
  prefs->webgl_unmasked_renderer_override = persona.webgl.renderer;
  // Always assigned, so a persona the host adapter can match clears the
  // previous persona's block.
  prefs->webgpu_blocked = IsWebGpuBlockedForPersona(persona);
}

void ClearPersonaFromRendererPrefs(blink::RendererPreferences* prefs) {
  prefs->user_agent_override = blink::UserAgentOverride();
  prefs->navigator_platform_override.clear();
  prefs->hardware_concurrency_override = 0;
  prefs->device_memory_override = 0;
  prefs->screen_width_override = 0;
  prefs->screen_height_override = 0;
  prefs->screen_color_depth_override = 0;
  prefs->device_pixel_ratio_override = 0;
  prefs->timezone_override.clear();
  prefs->locale_override.clear();
  prefs->webgl_unmasked_vendor_override.clear();
  prefs->webgl_unmasked_renderer_override.clear();
  prefs->webgpu_blocked = false;
}

}  // namespace shuttle

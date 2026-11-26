// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/fingerprint_persona.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/version.h"
#include "base/version_info/version_info.h"

namespace shuttle {

namespace {

// The browser versions a persona may claim. Chrome's version scheme is
// "major.0.build.patch" (the minor is always 0). A persona reports the Chrome
// it runs on: TLS, HTTP/2 and the web features a page can detect all show the
// real engine, so its major and branch build are this browser's
// (version_info, from chrome/VERSION). Only the patch may differ, as between
// releases of one branch.
constexpr unsigned kMaxBrowserPatch = 999;
constexpr char kBrowserBrand[] = "Chrome";

// This browser's version, major and branch build ("158.0.8127.18": 158, 8127).
const base::Version& EngineVersion() {
  return version_info::GetVersion();
}
uint32_t EngineMajor() {
  return EngineVersion().components()[0];
}
uint32_t EngineBuild() {
  return EngineVersion().components()[2];
}

std::string BrowserVersionError() {
  return base::StrCat(
      {"browserVersion must be this browser's version ",
       version_info::GetVersionNumber(), " (only the patch may differ: ",
       base::NumberToString(EngineMajor()), ".0.",
       base::NumberToString(EngineBuild()), ".<0 to 999>)"});
}

// Chrome stopped supporting macOS 11 with this major: a persona on a newer
// Chrome cannot report an older macOS.
constexpr int kMinBrowserMajorForMacos12 = 140;

// Desktop screens (the only kind of persona there is).
constexpr int kMinDesktopScreenWidth = 1024;
constexpr int kMinDesktopScreenHeight = 600;
// Chrome reports at most 8 GB (navigator.deviceMemory is capped); a desktop
// that runs this Chrome has at least 2 GB.
constexpr double kMinDesktopDeviceMemory = 2;
// Apple Silicon Macs: at least 8 logical cores and at least 8 GB of memory.
constexpr int kMinAppleSiliconCores = 8;
constexpr int kLargeCoreCount = 16;
constexpr double kLargeCoreCountMinMemory = 8;

constexpr char kPresetWindows[] = "windows-chrome";
constexpr char kPresetMacos[] = "macos-chrome";
constexpr char kPresetLinux[] = "linux-chrome";

constexpr char kOsWindows[] = "windows";
constexpr char kOsMacos[] = "macos";
constexpr char kOsLinux[] = "linux";

constexpr size_t kMaxLanguages = 8;
constexpr size_t kMaxTimezoneLength = 64;
constexpr size_t kMaxPlatformVersionLength = 32;

// Pref keys (snake_case) of a stored persona.
constexpr char kVersion[] = "version";
constexpr char kSeed[] = "seed";
constexpr char kPreset[] = "preset";
constexpr char kOs[] = "os";
constexpr char kPlatform[] = "platform";
constexpr char kBrowser[] = "browser";
constexpr char kBrand[] = "brand";
constexpr char kUserAgent[] = "user_agent";
constexpr char kClientHints[] = "client_hints";
constexpr char kPlatformVersion[] = "platform_version";
constexpr char kArchitecture[] = "architecture";
constexpr char kBitness[] = "bitness";
constexpr char kMobile[] = "mobile";
constexpr char kLocale[] = "locale";
constexpr char kLanguages[] = "languages";
constexpr char kTimezone[] = "timezone";
constexpr char kHardwareConcurrency[] = "hardware_concurrency";
constexpr char kDeviceMemory[] = "device_memory";
constexpr char kScreen[] = "screen";
constexpr char kWidth[] = "width";
constexpr char kHeight[] = "height";
constexpr char kDevicePixelRatio[] = "device_pixel_ratio";
constexpr char kColorDepth[] = "color_depth";
constexpr char kWebgl[] = "webgl";
constexpr char kVendor[] = "vendor";
constexpr char kRenderer[] = "renderer";
constexpr size_t kMaxWebglStringLength = 200;
// "version" is shared between the top level and "browser".

constexpr std::array<double, 6> kDeviceMemories = {0.25, 0.5, 1, 2, 4, 8};

// base::Contains was removed from this Chromium; the standard ranges do it.
template <typename Range, typename Value>
bool Contains(const Range& range, const Value& value) {
  return std::ranges::find(range, value) != std::ranges::end(range);
}

// splitmix64: the next 64 random bits after `state`, advancing it.
uint64_t NextRandom(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

template <typename T, size_t N>
const T& Pick(const std::array<T, N>& items, uint64_t& state) {
  return items[NextRandom(state) % N];
}

struct ScreenChoice {
  int width;
  int height;
  double device_pixel_ratio;
};

const char* OsForPreset(std::string_view preset) {
  if (preset == kPresetWindows) {
    return kOsWindows;
  }
  if (preset == kPresetMacos) {
    return kOsMacos;
  }
  if (preset == kPresetLinux) {
    return kOsLinux;
  }
  return nullptr;
}

const char* PlatformForOs(std::string_view os) {
  if (os == kOsWindows) {
    return "Win32";
  }
  if (os == kOsMacos) {
    return "MacIntel";
  }
  if (os == kOsLinux) {
    return "Linux x86_64";
  }
  return nullptr;
}

const char* ClientHintPlatformForOs(std::string_view os) {
  if (os == kOsWindows) {
    return "Windows";
  }
  if (os == kOsMacos) {
    return "macOS";
  }
  if (os == kOsLinux) {
    return "Linux";
  }
  return nullptr;
}

const char* UserAgentPlatformToken(std::string_view os) {
  if (os == kOsWindows) {
    return "Windows NT 10.0; Win64; x64";
  }
  if (os == kOsMacos) {
    return "Macintosh; Intel Mac OS X 10_15_7";
  }
  if (os == kOsLinux) {
    return "X11; Linux x86_64";
  }
  return nullptr;
}

// Whether `text` is 1-`max_digits` ASCII digits, without a leading zero.
bool IsNumber(std::string_view text, size_t max_digits = 9) {
  if (text.empty() || text.size() > max_digits ||
      (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  for (char c : text) {
    if (!base::IsAsciiDigit(c)) {
      return false;
    }
  }
  return true;
}

// Whether `version` has the shape of a Chrome version: four numbers,
// "major.0.build.patch", patch 0 to 999. Says nothing about which Chrome.
bool ParseBrowserVersionShape(std::string_view version,
                              uint32_t* major,
                              uint32_t* build) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      version, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 4) {
    return false;
  }
  for (std::string_view part : parts) {
    if (!IsNumber(part, 6)) {
      return false;
    }
  }
  unsigned major_number = 0;
  unsigned build_number = 0;
  unsigned patch = 0;
  if (!base::StringToUint(parts[0], &major_number) ||
      !base::StringToUint(parts[2], &build_number) ||
      !base::StringToUint(parts[3], &patch)) {
    return false;
  }
  if (parts[1] != "0" || patch > kMaxBrowserPatch) {
    return false;
  }
  *major = major_number;
  *build = build_number;
  return true;
}

// A version this browser may report: Chrome's shape with this browser's major
// and branch build (see kMaxBrowserPatch). `major` gets the first number.
bool ParseBrowserVersion(std::string_view version, std::string* major) {
  uint32_t major_number = 0;
  uint32_t build = 0;
  if (!ParseBrowserVersionShape(version, &major_number, &build) ||
      major_number != EngineMajor() || build != EngineBuild()) {
    return false;
  }
  if (major) {
    *major = base::NumberToString(major_number);
  }
  return true;
}

// "10.0.0": one to three numbers.
bool IsPlatformVersion(std::string_view version, int* major) {
  if (version.empty() || version.size() > kMaxPlatformVersionLength) {
    return false;
  }
  std::vector<std::string_view> parts = base::SplitStringPiece(
      version, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.empty() || parts.size() > 3) {
    return false;
  }
  for (std::string_view part : parts) {
    if (!IsNumber(part, 6)) {
      return false;
    }
  }
  return base::StringToInt(parts[0], major);
}

std::string BuildUserAgent(std::string_view os, std::string_view major) {
  const char* token = UserAgentPlatformToken(os);
  return base::StrCat({"Mozilla/5.0 (", token ? token : "",
                       ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/",
                       major, ".0.0.0 Safari/537.36"});
}

// The base languages Chrome offers as accept-languages (ISO 639-1, plus
// "fil"). A persona language outside this list is not something a Chrome
// user can have chosen.
constexpr auto kKnownLanguages = std::to_array<std::string_view>({
    "af", "am", "ar", "az", "be", "bg", "bn", "bs", "ca", "cs", "cy", "da",
    "de", "el", "en", "es", "et", "eu", "fa", "fi", "fil", "fr", "ga", "gl",
    "gu", "he", "hi", "hr", "hu", "hy", "id", "is", "it", "ja", "ka", "kk",
    "km", "kn", "ko", "ky", "lo", "lt", "lv", "mk", "ml", "mn", "mr", "ms",
    "my", "nb", "ne", "nl", "no", "pa", "pl", "pt", "ro", "ru", "si", "sk",
    "sl", "sq", "sr", "sv", "sw", "ta", "te", "th", "tr", "uk", "ur", "uz",
    "vi", "zh", "zu",
});

// "en", "en-US": a known lower case language and an optional two letter upper
// case region.
bool IsLocale(std::string_view locale) {
  const size_t dash = locale.find('-');
  std::string_view language = locale.substr(0, dash);
  if (language.size() < 2 || language.size() > 3) {
    return false;
  }
  for (char c : language) {
    if (!base::IsAsciiLower(c)) {
      return false;
    }
  }
  if (!Contains(kKnownLanguages, language)) {
    return false;
  }
  if (dash == std::string_view::npos) {
    return true;
  }
  std::string_view region = locale.substr(dash + 1);
  if (region.size() != 2) {
    return false;
  }
  for (char c : region) {
    if (!base::IsAsciiUpper(c)) {
      return false;
    }
  }
  return true;
}

// The syntax of an IANA time zone id ("UTC", "Europe/Paris",
// "America/Argentina/Buenos_Aires"). Whether the zone exists is not checked.
bool IsTimezone(std::string_view timezone) {
  if (timezone.empty() || timezone.size() > kMaxTimezoneLength ||
      !base::IsAsciiAlpha(timezone.front()) ||
      !(base::IsAsciiAlpha(timezone.back()) ||
        base::IsAsciiDigit(timezone.back()))) {
    return false;
  }
  int slashes = 0;
  char previous = 0;
  for (char c : timezone) {
    if (c == '/') {
      if (previous == '/' || ++slashes > 2) {
        return false;
      }
    } else if (!base::IsAsciiAlpha(c) && !base::IsAsciiDigit(c) && c != '_' &&
               c != '+' && c != '-') {
      return false;
    }
    previous = c;
  }
  return true;
}

// Printable ASCII only, 1-kMaxWebglStringLength characters.
bool IsWebglString(std::string_view text) {
  if (text.empty() || text.size() > kMaxWebglStringLength) {
    return false;
  }
  for (char c : text) {
    if (!base::IsAsciiPrintable(c)) {
      return false;
    }
  }
  return true;
}

constexpr char kWebglVendorPrefix[] = "Google Inc. (";
constexpr char kWebglRendererPrefix[] = "ANGLE (";
constexpr char kAppleGpuVendor[] = "Apple";

// The inside of "<prefix>...)": what is between the prefix and the final
// parenthesis. The caller has checked the prefix and the suffix.
std::string_view InsideParentheses(std::string_view text,
                                   std::string_view prefix) {
  return text.substr(prefix.size(), text.size() - prefix.size() - 1);
}

// Splits the inside of an ANGLE renderer string at the commas outside of
// parentheses ("NVIDIA", "NVIDIA GeForce RTX 3060 (0x2503) Direct3D11 ...",
// "D3D11"). Returns false when the parentheses do not balance or a part is
// empty.
bool SplitAngleParts(std::string_view inside,
                     std::vector<std::string_view>* parts) {
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= inside.size(); ++i) {
    const bool end = i == inside.size();
    const char c = end ? ',' : inside[i];
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (--depth < 0) {
        return false;
      }
    } else if (c == ',' && depth == 0) {
      std::string_view part = inside.substr(start, i - start);
      while (!part.empty() && part.front() == ' ') {
        part.remove_prefix(1);
      }
      if (part.empty()) {
        return false;
      }
      parts->push_back(part);
      start = i + 1;
    }
  }
  return depth == 0;
}

bool Mentions(std::string_view text, std::string_view token) {
  return text.find(token) != std::string_view::npos;
}

// Whether the WebGL strings look like what Chrome reports on `os` (ANGLE
// strings: "Google Inc. (vendor)" and "ANGLE (vendor, gpu, backend)"): the
// vendor of both agrees, the graphics backend is the one of the os (Direct3D
// on Windows, Metal on macOS, OpenGL or Vulkan on Linux, never a mix) and the
// GPU is one the os and the architecture can have (Apple GPUs only on a Mac,
// and every Apple Silicon Mac has one). Returns an error or "". An empty
// `webgl` (no override) is always fine.
std::string CheckWebgl(std::string_view os,
                       std::string_view architecture,
                       const FingerprintPersona::WebGL& webgl) {
  if (webgl.empty()) {
    return std::string();
  }
  if (webgl.vendor.empty() || webgl.renderer.empty()) {
    return "webgl needs both vendor and renderer";
  }
  if (!IsWebglString(webgl.vendor) || !IsWebglString(webgl.renderer)) {
    return "webgl vendor and renderer must be printable ASCII of 1 to 200 "
           "characters";
  }
  if (!base::StartsWith(webgl.vendor, kWebglVendorPrefix) ||
      !base::EndsWith(webgl.vendor, ")")) {
    return "webgl vendor must look like Google Inc. (NVIDIA)";
  }
  if (!base::StartsWith(webgl.renderer, kWebglRendererPrefix) ||
      !base::EndsWith(webgl.renderer, ")")) {
    return "webgl renderer must look like ANGLE (vendor, gpu, backend)";
  }
  std::vector<std::string_view> parts;
  if (!SplitAngleParts(InsideParentheses(webgl.renderer, kWebglRendererPrefix),
                       &parts) ||
      parts.size() < 3) {
    return "webgl renderer must look like ANGLE (vendor, gpu, backend)";
  }
  const std::string_view vendor =
      InsideParentheses(webgl.vendor, kWebglVendorPrefix);
  if (vendor.empty() || vendor != parts.front()) {
    return "webgl vendor does not match the vendor of the renderer";
  }

  const std::string_view renderer = webgl.renderer;
  const bool direct3d = Mentions(renderer, "Direct3D") ||
                        Mentions(renderer, "D3D");
  const bool metal = Mentions(renderer, "Metal");
  const bool opengl = Mentions(renderer, "OpenGL");
  const bool vulkan = Mentions(renderer, "Vulkan");
  if (os == kOsWindows) {
    if (!direct3d) {
      return "webgl renderer does not match the os (Windows reports Direct3D)";
    }
    if (metal || opengl || vulkan) {
      return "webgl renderer mixes graphics backends (Windows reports only "
             "Direct3D)";
    }
  } else if (os == kOsMacos) {
    if (!metal) {
      return "webgl renderer does not match the os (macOS reports Metal)";
    }
    if (direct3d || opengl || vulkan) {
      return "webgl renderer mixes graphics backends (macOS reports only "
             "Metal)";
    }
  } else if (os == kOsLinux) {
    if (!opengl && !vulkan) {
      return "webgl renderer does not match the os (Linux reports OpenGL or "
             "Vulkan)";
    }
    if (direct3d || metal) {
      return "webgl renderer mixes graphics backends (Linux reports OpenGL or "
             "Vulkan, not Direct3D or Metal)";
    }
  }

  if (vendor == kAppleGpuVendor && os != kOsMacos) {
    return "webgl reports an Apple GPU, which only a Mac has";
  }
  if (os == kOsMacos && architecture == "arm" && vendor != kAppleGpuVendor) {
    return "the persona is an Apple Silicon Mac (architecture arm), whose "
           "webgl vendor is Google Inc. (Apple)";
  }
  return std::string();
}

struct GpuChoice {
  const char* vendor;
  const char* renderer;
};

// The GPUs generated personas can report, as Chrome's ANGLE writes them for
// each os (these are the WEBGL_debug_renderer_info strings only: they say
// nothing about WebGL limits or extensions, which stay the real ones).
constexpr std::array<GpuChoice, 5> kWindowsGpus = {{
    {"Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002503) Direct3D11 vs_5_0 "
     "ps_5_0, D3D11)"},
    {"Google Inc. (NVIDIA)",
     "ANGLE (NVIDIA, NVIDIA GeForce GTX 1660 (0x000021C4) Direct3D11 vs_5_0 "
     "ps_5_0, D3D11)"},
    {"Google Inc. (Intel)",
     "ANGLE (Intel, Intel(R) UHD Graphics 630 (0x00003E9B) Direct3D11 vs_5_0 "
     "ps_5_0, D3D11)"},
    {"Google Inc. (Intel)",
     "ANGLE (Intel, Intel(R) UHD Graphics 620 (0x00005917) Direct3D11 vs_5_0 "
     "ps_5_0, D3D11)"},
    {"Google Inc. (AMD)",
     "ANGLE (AMD, AMD Radeon RX 580 (0x000067DF) Direct3D11 vs_5_0 ps_5_0, "
     "D3D11)"},
}};
constexpr std::array<GpuChoice, 4> kMacAppleSiliconGpus = {{
    {"Google Inc. (Apple)",
     "ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)"},
    {"Google Inc. (Apple)",
     "ANGLE (Apple, ANGLE Metal Renderer: Apple M1 Pro, Unspecified Version)"},
    {"Google Inc. (Apple)",
     "ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)"},
    {"Google Inc. (Apple)",
     "ANGLE (Apple, ANGLE Metal Renderer: Apple M3, Unspecified Version)"},
}};
constexpr std::array<GpuChoice, 2> kMacIntelGpus = {{
    {"Google Inc. (Intel Inc.)",
     "ANGLE (Intel Inc., ANGLE Metal Renderer: Intel(R) UHD Graphics 630, "
     "Unspecified Version)"},
    {"Google Inc. (AMD)",
     "ANGLE (AMD, ANGLE Metal Renderer: AMD Radeon Pro 5500M, Unspecified "
     "Version)"},
}};
constexpr std::array<GpuChoice, 3> kLinuxGpus = {{
    {"Google Inc. (NVIDIA Corporation)",
     "ANGLE (NVIDIA Corporation, NVIDIA GeForce GTX 1660/PCIe/SSE2, OpenGL "
     "4.5.0 NVIDIA 535.54.03)"},
    {"Google Inc. (Intel)",
     "ANGLE (Intel, Mesa Intel(R) UHD Graphics 630 (CFL GT2), OpenGL 4.6 "
     "(Core Profile) Mesa 23.2.1)"},
    {"Google Inc. (Intel)",
     "ANGLE (Intel, Mesa Intel(R) UHD Graphics 620 (KBL GT2), OpenGL 4.6 "
     "(Core Profile) Mesa 23.2.1)"},
}};

// One GPU string pair for `os` and `architecture`, picked with `seed` (its own
// random stream: it never moves the draws of GenerateFingerprintPersona).
FingerprintPersona::WebGL PickWebgl(std::string_view os,
                                    std::string_view architecture,
                                    uint64_t seed) {
  uint64_t state = seed ^ 0x57ebd6c0a1f4b3e9ULL;
  const GpuChoice* choice = nullptr;
  if (os == kOsWindows) {
    choice = &Pick(kWindowsGpus, state);
  } else if (os == kOsMacos) {
    choice = architecture == "arm" ? &Pick(kMacAppleSiliconGpus, state)
                                   : &Pick(kMacIntelGpus, state);
  } else {
    choice = &Pick(kLinuxGpus, state);
  }
  FingerprintPersona::WebGL webgl;
  webgl.vendor = choice->vendor;
  webgl.renderer = choice->renderer;
  return webgl;
}

// The locales a persona can be generated for, with the time zone people who
// have that locale mostly live in. Any valid locale can still be set by hand;
// this only makes a generated persona agree with itself.
struct LocaleProfile {
  const char* locale;
  const char* timezone;
};
constexpr std::array<LocaleProfile, 25> kLocaleProfiles = {{
    {"en-US", "America/New_York"},
    {"en-GB", "Europe/London"},
    {"en-CA", "America/Toronto"},
    {"en-AU", "Australia/Sydney"},
    {"de-DE", "Europe/Berlin"},
    {"fr-FR", "Europe/Paris"},
    {"es-ES", "Europe/Madrid"},
    {"es-MX", "America/Mexico_City"},
    {"it-IT", "Europe/Rome"},
    {"pt-BR", "America/Sao_Paulo"},
    {"nl-NL", "Europe/Amsterdam"},
    {"pl-PL", "Europe/Warsaw"},
    {"sv-SE", "Europe/Stockholm"},
    {"tr-TR", "Europe/Istanbul"},
    {"ru-RU", "Europe/Moscow"},
    {"ja-JP", "Asia/Tokyo"},
    {"ko-KR", "Asia/Seoul"},
    {"zh-CN", "Asia/Shanghai"},
    {"zh-TW", "Asia/Taipei"},
    {"vi-VN", "Asia/Ho_Chi_Minh"},
    {"id-ID", "Asia/Jakarta"},
    {"th-TH", "Asia/Bangkok"},
    {"hi-IN", "Asia/Kolkata"},
    {"ar-SA", "Asia/Riyadh"},
    {"he-IL", "Asia/Jerusalem"},
}};

const LocaleProfile* FindLocaleProfile(std::string_view locale) {
  for (const LocaleProfile& profile : kLocaleProfiles) {
    if (locale == profile.locale) {
      return &profile;
    }
  }
  return nullptr;
}

std::vector<std::string> DefaultLanguages(const std::string& locale) {
  std::vector<std::string> languages = {locale};
  const std::string language = locale.substr(0, locale.find('-'));
  if (language != locale) {
    languages.push_back(language);
  }
  return languages;
}

// 16 lower case hex digits.
std::string HexSeed(uint64_t seed) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex(16, '0');
  for (int i = 15; i >= 0; --i) {
    hex[i] = kDigits[seed & 0xf];
    seed >>= 4;
  }
  return hex;
}

bool ParseHexSeed(std::string_view text, uint64_t* seed) {
  if (text.size() != 16) {
    return false;
  }
  uint64_t value = 0;
  for (char c : text) {
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    value = (value << 4) | static_cast<uint64_t>(digit);
  }
  *seed = value;
  return true;
}

// Strict readers over a dictionary. Each returns false when the key is
// missing or has another type.
bool ReadString(const base::DictValue& dict,
                std::string_view key,
                std::string* out) {
  const std::string* value = dict.FindString(key);
  if (!value) {
    return false;
  }
  *out = *value;
  return true;
}

bool ReadInt(const base::DictValue& dict, std::string_view key, int* out) {
  std::optional<int> value = dict.FindInt(key);
  if (!value) {
    return false;
  }
  *out = *value;
  return true;
}

// A JSON number as a double: an int or a double, finite. An integral number
// (`8`, `2`) is parsed as an int by the JSON readers and by prefs, so this
// does not rely on GetIfDouble()/FindDouble() accepting ints. The caller still
// range-checks the value.
std::optional<double> GetFiniteNumber(const base::Value& value) {
  double number = 0;
  if (value.is_int()) {
    number = value.GetInt();
  } else if (value.is_double()) {
    number = value.GetDouble();
  } else {
    return std::nullopt;
  }
  if (!std::isfinite(number)) {
    return std::nullopt;
  }
  return number;
}

// The number at `key`: missing or any other type returns false.
bool ReadNumber(const base::DictValue& dict,
                std::string_view key,
                double* out) {
  const base::Value* value = dict.Find(key);
  std::optional<double> number =
      value ? GetFiniteNumber(*value) : std::nullopt;
  if (!number) {
    return false;
  }
  *out = *number;
  return true;
}

// Whether `dict` has no key outside `allowed`.
bool OnlyKeys(const base::DictValue& dict,
              std::initializer_list<std::string_view> allowed) {
  for (auto [key, value] : dict) {
    if (!Contains(allowed, std::string_view(key))) {
      return false;
    }
  }
  return true;
}

// The first key of `dict` outside `allowed`, or "".
std::string UnknownKey(const base::DictValue& dict,
                       std::initializer_list<std::string_view> allowed) {
  for (auto [key, value] : dict) {
    if (!Contains(allowed, std::string_view(key))) {
      return key;
    }
  }
  return std::string();
}

// A readable form of a key for error messages: never long, never control
// characters.
std::string ShowKey(const std::string& key) {
  std::string shown;
  for (char c : key.substr(0, 40)) {
    shown.push_back(base::IsAsciiPrintable(c) ? c : '?');
  }
  return shown;
}

}  // namespace

FingerprintPersona::ClientHints::ClientHints() = default;
FingerprintPersona::ClientHints::ClientHints(const ClientHints&) = default;
FingerprintPersona::ClientHints& FingerprintPersona::ClientHints::operator=(
    const ClientHints&) = default;
FingerprintPersona::ClientHints::ClientHints(ClientHints&&) = default;
FingerprintPersona::ClientHints& FingerprintPersona::ClientHints::operator=(
    ClientHints&&) = default;
FingerprintPersona::ClientHints::~ClientHints() = default;

FingerprintPersona::WebGL::WebGL() = default;
FingerprintPersona::WebGL::WebGL(const WebGL&) = default;
FingerprintPersona::WebGL& FingerprintPersona::WebGL::operator=(
    const WebGL&) = default;
FingerprintPersona::WebGL::WebGL(WebGL&&) = default;
FingerprintPersona::WebGL& FingerprintPersona::WebGL::operator=(WebGL&&) =
    default;
FingerprintPersona::WebGL::~WebGL() = default;

StoredFingerprintPersona::StoredFingerprintPersona() = default;
StoredFingerprintPersona::StoredFingerprintPersona(
    const StoredFingerprintPersona&) = default;
StoredFingerprintPersona& StoredFingerprintPersona::operator=(
    const StoredFingerprintPersona&) = default;
StoredFingerprintPersona::StoredFingerprintPersona(StoredFingerprintPersona&&) =
    default;
StoredFingerprintPersona& StoredFingerprintPersona::operator=(
    StoredFingerprintPersona&&) = default;
StoredFingerprintPersona::~StoredFingerprintPersona() = default;

FingerprintPersona::FingerprintPersona() = default;
FingerprintPersona::FingerprintPersona(const FingerprintPersona&) = default;
FingerprintPersona& FingerprintPersona::operator=(const FingerprintPersona&) =
    default;
FingerprintPersona::FingerprintPersona(FingerprintPersona&&) = default;
FingerprintPersona& FingerprintPersona::operator=(FingerprintPersona&&) =
    default;
FingerprintPersona::~FingerprintPersona() = default;

bool IsKnownFingerprintPreset(std::string_view preset) {
  return OsForPreset(preset) != nullptr;
}

bool IsKnownFingerprintLocale(std::string_view locale) {
  return FindLocaleProfile(locale) != nullptr;
}

FingerprintPersona GenerateFingerprintPersona(std::string_view preset,
                                              uint64_t seed) {
  return GenerateFingerprintPersona(preset, seed,
                                    FingerprintGenerationOptions());
}

FingerprintPersona GenerateFingerprintPersona(
    std::string_view preset,
    uint64_t seed,
    const FingerprintGenerationOptions& options) {
  const char* os = OsForPreset(preset);
  CHECK(os);
  const LocaleProfile* locale_profile =
      options.locale.empty() ? FindLocaleProfile("en-US")
                             : FindLocaleProfile(options.locale);
  CHECK(locale_profile);
  FingerprintPersona persona;
  persona.version = kFingerprintPersonaVersion;
  persona.seed = seed;
  persona.preset = std::string(preset);
  persona.os = os;
  persona.platform = PlatformForOs(os);
  persona.browser_brand = kBrowserBrand;
  persona.browser_version = std::string(version_info::GetVersionNumber());
  persona.user_agent =
      BuildUserAgent(os, version_info::GetMajorVersionNumber());
  persona.client_hints.platform = ClientHintPlatformForOs(os);
  persona.client_hints.mobile = false;
  persona.client_hints.bitness = "64";
  persona.locale = locale_profile->locale;
  persona.languages = DefaultLanguages(persona.locale);
  persona.timezone = locale_profile->timezone;
  persona.screen.color_depth = 24;

  // Each draw advances the state, so the order of the draws is part of the
  // format: keep it when adding fields (new draws go last).
  uint64_t state = seed;
  static constexpr std::array<int, 5> kCores = {4, 6, 8, 12, 16};
  persona.hardware_concurrency = Pick(kCores, state);
  static constexpr std::array<double, 2> kMemories = {4, 8};
  persona.device_memory = Pick(kMemories, state);
  if (persona.os == kOsWindows) {
    static constexpr std::array<ScreenChoice, 4> kScreens = {{
        {1920, 1080, 1}, {1366, 768, 1}, {2560, 1440, 1}, {1536, 864, 1.25},
    }};
    const ScreenChoice& screen = Pick(kScreens, state);
    persona.screen.width = screen.width;
    persona.screen.height = screen.height;
    persona.screen.device_pixel_ratio = screen.device_pixel_ratio;
    static constexpr std::array<const char*, 2> kVersions = {"10.0.0",
                                                             "15.0.0"};
    persona.client_hints.platform_version = Pick(kVersions, state);
    persona.client_hints.architecture = "x86";
  } else if (persona.os == kOsMacos) {
    static constexpr std::array<ScreenChoice, 4> kScreens = {{
        {1440, 900, 2}, {1512, 982, 2}, {1728, 1117, 2}, {1280, 800, 2},
    }};
    const ScreenChoice& screen = Pick(kScreens, state);
    persona.screen.width = screen.width;
    persona.screen.height = screen.height;
    persona.screen.device_pixel_ratio = screen.device_pixel_ratio;
    static constexpr std::array<const char*, 2> kVersions = {"14.5.0",
                                                             "15.1.0"};
    persona.client_hints.platform_version = Pick(kVersions, state);
    static constexpr std::array<const char*, 2> kArchitectures = {"arm", "x86"};
    persona.client_hints.architecture = Pick(kArchitectures, state);
  } else {
    static constexpr std::array<ScreenChoice, 3> kScreens = {{
        {1920, 1080, 1}, {2560, 1440, 1}, {1366, 768, 1},
    }};
    const ScreenChoice& screen = Pick(kScreens, state);
    persona.screen.width = screen.width;
    persona.screen.height = screen.height;
    persona.screen.device_pixel_ratio = screen.device_pixel_ratio;
    persona.client_hints.platform_version = "6.8.0";
    persona.client_hints.architecture = "x86";
  }

  // The draws above are independent. What the platform makes impossible is
  // raised to the smallest value it can have, so a seed that was already
  // coherent keeps its persona (all Windows and Linux ones, and the Macs whose
  // draws were fine). Apple Silicon Macs have at least 8 cores and 8 GB.
  if (persona.os == kOsMacos && persona.client_hints.architecture == "arm") {
    persona.hardware_concurrency =
        std::max(persona.hardware_concurrency, kMinAppleSiliconCores);
    persona.device_memory = std::max(persona.device_memory, 8.0);
  }
  if (options.webgl) {
    persona.webgl = PickWebgl(persona.os, persona.client_hints.architecture,
                              seed);
  }
  DCHECK(CheckFingerprintPersona(persona).empty())
      << CheckFingerprintPersona(persona);
  return persona;
}

std::string CheckFingerprintPersona(const FingerprintPersona& persona) {
  if (persona.version != kFingerprintPersonaVersion) {
    return "unsupported version";
  }
  const char* preset_os = OsForPreset(persona.preset);
  if (!preset_os) {
    return "unknown preset (windows-chrome, macos-chrome, linux-chrome)";
  }
  if (persona.os != preset_os) {
    return "os does not match the preset";
  }
  if (persona.platform != PlatformForOs(persona.os)) {
    return "platform does not match the os";
  }
  if (persona.browser_brand != kBrowserBrand) {
    return "unsupported browser brand";
  }
  std::string major;
  if (!ParseBrowserVersion(persona.browser_version, &major)) {
    return BrowserVersionError();
  }
  if (persona.user_agent != BuildUserAgent(persona.os, major)) {
    return "user agent does not match the os and the browser version";
  }
  int browser_major = 0;
  if (!base::StringToInt(major, &browser_major)) {
    return BrowserVersionError();
  }

  const FingerprintPersona::ClientHints& hints = persona.client_hints;
  if (hints.platform != ClientHintPlatformForOs(persona.os)) {
    return "client hints platform does not match the os";
  }
  int platform_major = 0;
  if (!IsPlatformVersion(hints.platform_version, &platform_major)) {
    return "client hints platformVersion must be one to three dotted numbers";
  }
  if (persona.os == kOsWindows &&
      (platform_major < 10 || platform_major > 25)) {
    return "client hints platformVersion does not match the user agent "
           "(Windows NT 10.0: 10.0.0 for Windows 10, 13.0.0 and later for "
           "Windows 11)";
  }
  if (persona.os == kOsMacos &&
      (platform_major < 10 || platform_major > 30 ||
       (browser_major >= kMinBrowserMajorForMacos12 && platform_major < 12))) {
    return "client hints platformVersion is not a macOS this browser version "
           "runs on (macOS 12 or later, e.g. 14.5.0)";
  }
  if (persona.os == kOsLinux && (platform_major < 3 || platform_major > 9)) {
    return "client hints platformVersion must be a Linux kernel version (e.g. "
           "6.8.0)";
  }
  if (hints.architecture != "x86" && hints.architecture != "arm") {
    return "client hints architecture must be x86 or arm";
  }
  if (persona.os != kOsMacos && hints.architecture != "x86") {
    return "client hints architecture does not match the user agent (x86_64)";
  }
  if (hints.bitness != "64") {
    return "client hints bitness does not match the user agent (64-bit)";
  }
  if (hints.mobile) {
    return "mobile personas are not supported";
  }

  if (!IsLocale(persona.locale)) {
    return "locale must look like en-US";
  }
  if (persona.languages.empty() || persona.languages.size() > kMaxLanguages) {
    return "languages must have 1 to 8 entries";
  }
  std::set<std::string> seen;
  for (const std::string& language : persona.languages) {
    if (!IsLocale(language)) {
      return "languages must look like en-US or en";
    }
    if (!seen.insert(language).second) {
      return "languages must not repeat";
    }
  }
  if (persona.languages.front() != persona.locale) {
    return "the first language must be the locale";
  }
  // Chrome lists a region language before its base language ("en-US", "en"),
  // never "en" ahead of "en-US".
  for (size_t i = 0; i < persona.languages.size(); ++i) {
    const std::string& language = persona.languages[i];
    if (language.find('-') != std::string::npos) {
      continue;
    }
    for (size_t j = i + 1; j < persona.languages.size(); ++j) {
      if (base::StartsWith(persona.languages[j], language + "-")) {
        return "languages must list a region language (en-US) before its "
               "base language (en)";
      }
    }
  }
  if (!IsTimezone(persona.timezone)) {
    return "timezone must be an IANA id like America/New_York";
  }

  if (persona.hardware_concurrency < 2 || persona.hardware_concurrency > 64) {
    return "hardwareConcurrency must be an integer from 2 to 64";
  }
  if (!Contains(kDeviceMemories, persona.device_memory)) {
    return "deviceMemory must be 0.25, 0.5, 1, 2, 4 or 8";
  }
  // Desktop machines only (hints.mobile is false).
  if (persona.device_memory < kMinDesktopDeviceMemory) {
    return "deviceMemory must be at least 2 for a desktop persona";
  }
  if (persona.hardware_concurrency > kLargeCoreCount &&
      persona.device_memory < kLargeCoreCountMinMemory) {
    return "hardwareConcurrency above 16 needs deviceMemory 8";
  }
  const bool apple_silicon =
      persona.os == kOsMacos && hints.architecture == "arm";
  if (apple_silicon && persona.hardware_concurrency < kMinAppleSiliconCores) {
    return "hardwareConcurrency must be at least 8 for an Apple Silicon Mac "
           "(architecture arm)";
  }
  if (apple_silicon && persona.device_memory < 8) {
    return "deviceMemory must be 8 for an Apple Silicon Mac (architecture "
           "arm)";
  }
  const FingerprintPersona::Screen& screen = persona.screen;
  if (screen.width < 320 || screen.width > 7680 || screen.height < 240 ||
      screen.height > 4320) {
    return "screen width must be 320-7680 and height 240-4320";
  }
  if (screen.width < screen.height) {
    return "screen must be landscape (width >= height)";
  }
  if (screen.width < kMinDesktopScreenWidth ||
      screen.height < kMinDesktopScreenHeight) {
    return "screen must be at least 1024x600 for a desktop persona";
  }
  if (!(screen.device_pixel_ratio >= 1 && screen.device_pixel_ratio <= 4) ||
      std::fmod(screen.device_pixel_ratio * 4, 1.0) != 0) {
    return "screen devicePixelRatio must be 1 to 4 in steps of 0.25";
  }
  if (persona.os == kOsMacos) {
    if (screen.device_pixel_ratio != 1 && screen.device_pixel_ratio != 2) {
      return "screen devicePixelRatio must be 1 or 2 on macOS";
    }
  } else if (screen.device_pixel_ratio > 3) {
    return "screen devicePixelRatio must be 1 to 3 on a desktop";
  }
  if (screen.color_depth != 24 && screen.color_depth != 30) {
    return "screen colorDepth must be 24 or 30";
  }
  return CheckWebgl(persona.os, hints.architecture, persona.webgl);
}

base::DictValue FingerprintPersonaToPrefValue(
    const FingerprintPersona& persona) {
  base::ListValue languages;
  for (const std::string& language : persona.languages) {
    languages.Append(language);
  }
  base::DictValue value = base::DictValue()
      .Set(kVersion, persona.version)
      .Set(kSeed, HexSeed(persona.seed))
      .Set(kPreset, persona.preset)
      .Set(kOs, persona.os)
      .Set(kPlatform, persona.platform)
      .Set(kBrowser, base::DictValue()
                         .Set(kBrand, persona.browser_brand)
                         .Set(kVersion, persona.browser_version))
      .Set(kUserAgent, persona.user_agent)
      .Set(kClientHints,
           base::DictValue()
               .Set(kPlatform, persona.client_hints.platform)
               .Set(kPlatformVersion, persona.client_hints.platform_version)
               .Set(kArchitecture, persona.client_hints.architecture)
               .Set(kBitness, persona.client_hints.bitness)
               .Set(kMobile, persona.client_hints.mobile))
      .Set(kLocale, persona.locale)
      .Set(kLanguages, std::move(languages))
      .Set(kTimezone, persona.timezone)
      .Set(kHardwareConcurrency, persona.hardware_concurrency)
      .Set(kDeviceMemory, persona.device_memory)
      .Set(kScreen, base::DictValue()
                        .Set(kWidth, persona.screen.width)
                        .Set(kHeight, persona.screen.height)
                        .Set(kDevicePixelRatio,
                             persona.screen.device_pixel_ratio)
                        .Set(kColorDepth, persona.screen.color_depth));
  if (!persona.webgl.empty()) {
    value.Set(kWebgl, base::DictValue()
                          .Set(kVendor, persona.webgl.vendor)
                          .Set(kRenderer, persona.webgl.renderer));
  }
  return value;
}

StoredFingerprintPersona FingerprintPersonaFromPrefValue(
    const base::Value* value) {
  StoredFingerprintPersona stored;
  if (!value) {
    return stored;
  }
  stored.state = StoredFingerprintPersona::State::kCorrupt;
  const base::DictValue* dict = value->GetIfDict();
  if (!dict ||
      !OnlyKeys(*dict, {kVersion, kSeed, kPreset, kOs, kPlatform, kBrowser,
                        kUserAgent, kClientHints, kLocale, kLanguages,
                        kTimezone, kHardwareConcurrency, kDeviceMemory,
                        kScreen, kWebgl})) {
    return stored;
  }
  FingerprintPersona persona;
  std::string seed;
  if (!ReadInt(*dict, kVersion, &persona.version) ||
      persona.version != kFingerprintPersonaVersion ||
      !ReadString(*dict, kSeed, &seed) || !ParseHexSeed(seed, &persona.seed) ||
      !ReadString(*dict, kPreset, &persona.preset) ||
      !ReadString(*dict, kOs, &persona.os) ||
      !ReadString(*dict, kPlatform, &persona.platform) ||
      !ReadString(*dict, kUserAgent, &persona.user_agent) ||
      !ReadString(*dict, kLocale, &persona.locale) ||
      !ReadString(*dict, kTimezone, &persona.timezone) ||
      !ReadInt(*dict, kHardwareConcurrency, &persona.hardware_concurrency) ||
      !ReadNumber(*dict, kDeviceMemory, &persona.device_memory)) {
    return stored;
  }

  const base::DictValue* browser = dict->FindDict(kBrowser);
  if (!browser || !OnlyKeys(*browser, {kBrand, kVersion}) ||
      !ReadString(*browser, kBrand, &persona.browser_brand) ||
      !ReadString(*browser, kVersion, &persona.browser_version)) {
    return stored;
  }

  const base::DictValue* hints = dict->FindDict(kClientHints);
  std::optional<bool> mobile = hints ? hints->FindBool(kMobile) : std::nullopt;
  if (!hints ||
      !OnlyKeys(*hints,
                {kPlatform, kPlatformVersion, kArchitecture, kBitness,
                 kMobile}) ||
      !ReadString(*hints, kPlatform, &persona.client_hints.platform) ||
      !ReadString(*hints, kPlatformVersion,
                  &persona.client_hints.platform_version) ||
      !ReadString(*hints, kArchitecture, &persona.client_hints.architecture) ||
      !ReadString(*hints, kBitness, &persona.client_hints.bitness) || !mobile) {
    return stored;
  }
  persona.client_hints.mobile = *mobile;

  const base::ListValue* languages = dict->FindList(kLanguages);
  if (!languages) {
    return stored;
  }
  for (const base::Value& language : *languages) {
    if (!language.is_string()) {
      return stored;
    }
    persona.languages.push_back(language.GetString());
  }

  const base::DictValue* screen = dict->FindDict(kScreen);
  if (!screen ||
      !OnlyKeys(*screen, {kWidth, kHeight, kDevicePixelRatio, kColorDepth}) ||
      !ReadInt(*screen, kWidth, &persona.screen.width) ||
      !ReadInt(*screen, kHeight, &persona.screen.height) ||
      !ReadNumber(*screen, kDevicePixelRatio,
                  &persona.screen.device_pixel_ratio) ||
      !ReadInt(*screen, kColorDepth, &persona.screen.color_depth)) {
    return stored;
  }

  // Optional (absent in values stored before it existed, and when unset); when
  // present it is exactly {vendor, renderer}, both non-empty (checked below).
  if (const base::Value* webgl_value = dict->Find(kWebgl)) {
    const base::DictValue* webgl = webgl_value->GetIfDict();
    if (!webgl || !OnlyKeys(*webgl, {kVendor, kRenderer}) ||
        !ReadString(*webgl, kVendor, &persona.webgl.vendor) ||
        !ReadString(*webgl, kRenderer, &persona.webgl.renderer) ||
        persona.webgl.empty()) {
      return stored;
    }
  }

  // A persona stored by a Shuttle on another Chrome branch moves onto this
  // one (see NormalizeBrowserVersionToEngine); the caller writes it back.
  stored.version_migrated = NormalizeBrowserVersionToEngine(&persona);
  if (!CheckFingerprintPersona(persona).empty()) {
    stored.version_migrated = false;
    return stored;
  }
  stored.state = StoredFingerprintPersona::State::kPersona;
  stored.persona = std::move(persona);
  return stored;
}

void DropForeignBrowserVersion(base::DictValue& input) {
  const std::string* version = input.FindString("browserVersion");
  uint32_t major = 0;
  uint32_t build = 0;
  if (version && ParseBrowserVersionShape(*version, &major, &build) &&
      !ParseBrowserVersion(*version, nullptr)) {
    input.Remove("browserVersion");
  }
}

bool NormalizeBrowserVersionToEngine(FingerprintPersona* persona) {
  uint32_t major = 0;
  uint32_t build = 0;
  if (!ParseBrowserVersionShape(persona->browser_version, &major, &build) ||
      (major == EngineMajor() && build == EngineBuild())) {
    // Not a version at all (left to CheckFingerprintPersona: corrupt), or
    // already this branch.
    return false;
  }
  // A version migration must not turn an inconsistent stored record into
  // a valid one by overwriting the evidence of corruption.
  if (persona->user_agent !=
      BuildUserAgent(persona->os, base::NumberToString(major))) {
    return false;
  }
  persona->browser_version = std::string(version_info::GetVersionNumber());
  persona->user_agent =
      BuildUserAgent(persona->os, version_info::GetMajorVersionNumber());
  return true;
}

base::DictValue FingerprintPersonaToMcpValue(
    const FingerprintPersona& persona) {
  base::ListValue languages;
  for (const std::string& language : persona.languages) {
    languages.Append(language);
  }
  base::DictValue value = base::DictValue()
      .Set("version", persona.version)
      .Set("preset", persona.preset)
      .Set("os", persona.os)
      .Set("platform", persona.platform)
      .Set("browser", base::DictValue()
                          .Set("brand", persona.browser_brand)
                          .Set("version", persona.browser_version))
      .Set("userAgent", persona.user_agent)
      .Set("clientHints",
           base::DictValue()
               .Set("platform", persona.client_hints.platform)
               .Set("platformVersion", persona.client_hints.platform_version)
               .Set("architecture", persona.client_hints.architecture)
               .Set("bitness", persona.client_hints.bitness)
               .Set("mobile", persona.client_hints.mobile))
      .Set("locale", persona.locale)
      .Set("languages", std::move(languages))
      .Set("timezone", persona.timezone)
      .Set("hardwareConcurrency", persona.hardware_concurrency)
      .Set("deviceMemory", persona.device_memory)
      .Set("screen", base::DictValue()
                         .Set("width", persona.screen.width)
                         .Set("height", persona.screen.height)
                         .Set("devicePixelRatio",
                              persona.screen.device_pixel_ratio)
                         .Set("colorDepth", persona.screen.color_depth));
  if (!persona.webgl.empty()) {
    value.Set("webgl", base::DictValue()
                           .Set("vendor", persona.webgl.vendor)
                           .Set("renderer", persona.webgl.renderer));
  }
  return value;
}

std::string ParseFingerprintPersonaInput(const base::Value& value,
                                         const FingerprintPersona* current,
                                         uint64_t seed,
                                         FingerprintPersona* out) {
  const base::DictValue* dict = value.GetIfDict();
  if (!dict) {
    return "must be an object (or null to clear)";
  }
  std::string unknown = UnknownKey(
      *dict, {"preset", "browserVersion", "locale", "languages", "timezone",
              "hardwareConcurrency", "deviceMemory", "screen", "webgl"});
  if (!unknown.empty()) {
    return base::StrCat({"unknown field: ", ShowKey(unknown)});
  }

  FingerprintPersona persona;
  if (const base::Value* preset = dict->Find("preset")) {
    if (!preset->is_string() ||
        !IsKnownFingerprintPreset(preset->GetString())) {
      return "preset must be windows-chrome, macos-chrome or linux-chrome";
    }
    // A locale the generator knows brings its usual time zone (an explicit
    // `timezone` below still wins); any other valid locale only replaces the
    // default one below.
    FingerprintGenerationOptions options;
    const base::Value* locale = dict->Find("locale");
    if (locale && locale->is_string() &&
        IsKnownFingerprintLocale(locale->GetString())) {
      options.locale = locale->GetString();
    }
    persona = GenerateFingerprintPersona(preset->GetString(), seed, options);
  } else if (current) {
    persona = *current;
  } else {
    return "preset is required (windows-chrome, macos-chrome or linux-chrome)";
  }

  if (const base::Value* version = dict->Find("browserVersion")) {
    std::string major;
    if (!version->is_string() ||
        !ParseBrowserVersion(version->GetString(), &major)) {
      return BrowserVersionError();
    }
    persona.browser_version = version->GetString();
    persona.user_agent = BuildUserAgent(persona.os, major);
  }

  bool locale_changed = false;
  if (const base::Value* locale = dict->Find("locale")) {
    if (!locale->is_string() || !IsLocale(locale->GetString())) {
      return "locale must look like en-US";
    }
    persona.locale = locale->GetString();
    locale_changed = true;
  }
  if (const base::Value* languages = dict->Find("languages")) {
    const base::ListValue* list = languages->GetIfList();
    if (!list || list->empty() || list->size() > kMaxLanguages) {
      return "languages must be a list of 1 to 8 strings like en-US or en";
    }
    persona.languages.clear();
    for (const base::Value& language : *list) {
      if (!language.is_string() || !IsLocale(language.GetString())) {
        return "languages must be a list of 1 to 8 strings like en-US or en";
      }
      persona.languages.push_back(language.GetString());
    }
    // Only the locale was left out: it is the first language.
    if (!dict->Find("locale")) {
      persona.locale = persona.languages.front();
    }
  } else if (locale_changed) {
    persona.languages = DefaultLanguages(persona.locale);
  }
  if (const base::Value* timezone = dict->Find("timezone")) {
    if (!timezone->is_string() || !IsTimezone(timezone->GetString())) {
      return "timezone must be an IANA id like America/New_York";
    }
    persona.timezone = timezone->GetString();
  }
  if (const base::Value* cores = dict->Find("hardwareConcurrency")) {
    if (!cores->is_int() || cores->GetInt() < 2 || cores->GetInt() > 64) {
      return "hardwareConcurrency must be an integer from 2 to 64";
    }
    persona.hardware_concurrency = cores->GetInt();
  }
  if (const base::Value* memory = dict->Find("deviceMemory")) {
    std::optional<double> gb = GetFiniteNumber(*memory);
    if (!gb || !Contains(kDeviceMemories, *gb)) {
      return "deviceMemory must be 0.25, 0.5, 1, 2, 4 or 8";
    }
    persona.device_memory = *gb;
  }
  if (const base::Value* screen_value = dict->Find("screen")) {
    const base::DictValue* screen = screen_value->GetIfDict();
    if (!screen) {
      return "screen must be an object";
    }
    unknown = UnknownKey(
        *screen, {"width", "height", "devicePixelRatio", "colorDepth"});
    if (!unknown.empty()) {
      return base::StrCat({"unknown screen field: ", ShowKey(unknown)});
    }
    if (const base::Value* width = screen->Find("width")) {
      if (!width->is_int()) {
        return "screen width must be an integer";
      }
      persona.screen.width = width->GetInt();
    }
    if (const base::Value* height = screen->Find("height")) {
      if (!height->is_int()) {
        return "screen height must be an integer";
      }
      persona.screen.height = height->GetInt();
    }
    if (const base::Value* ratio = screen->Find("devicePixelRatio")) {
      std::optional<double> number = GetFiniteNumber(*ratio);
      if (!number) {
        return "screen devicePixelRatio must be a number";
      }
      persona.screen.device_pixel_ratio = *number;
    }
    if (const base::Value* depth = screen->Find("colorDepth")) {
      if (!depth->is_int()) {
        return "screen colorDepth must be an integer";
      }
      persona.screen.color_depth = depth->GetInt();
    }
  }
  if (const base::Value* webgl_value = dict->Find("webgl")) {
    if (webgl_value->is_none()) {
      persona.webgl = FingerprintPersona::WebGL();
    } else {
      const base::DictValue* webgl = webgl_value->GetIfDict();
      if (!webgl) {
        return "webgl must be an object {vendor, renderer} (or null to clear)";
      }
      unknown = UnknownKey(*webgl, {"vendor", "renderer"});
      if (!unknown.empty()) {
        return base::StrCat({"unknown webgl field: ", ShowKey(unknown)});
      }
      const std::string* vendor = webgl->FindString("vendor");
      const std::string* renderer = webgl->FindString("renderer");
      if (!vendor || !renderer) {
        return "webgl needs both vendor and renderer (strings)";
      }
      persona.webgl.vendor = *vendor;
      persona.webgl.renderer = *renderer;
    }
  }

  std::string error = CheckFingerprintPersona(persona);
  if (!error.empty()) {
    return error;
  }
  *out = std::move(persona);
  return std::string();
}

}  // namespace shuttle

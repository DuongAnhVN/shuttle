// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_FINGERPRINT_PERSONA_H_
#define CHROME_BROWSER_SHUTTLE_FINGERPRINT_PERSONA_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace shuttle {

// The device a Chromium profile presents to the sites it visits (every tab,
// agent and storage partition of the profile alike): one coherent set of
// values (OS, browser, user agent, client hints, locale, timezone, hardware,
// screen) plus an internal seed for later per-profile noise. Design:
// tools/shuttle/docs/FINGERPRINT.md.
//
// This is the persistence/schema layer. How the values reach renderers and the
// network stack is persona_runtime.h (and which fields are applied, which are
// not yet, is FINGERPRINT.md).
//
// The seed never leaves the browser: it is in the pref value, never in MCP
// output (FingerprintPersonaToMcpValue).
struct FingerprintPersona {
  struct ClientHints {
    ClientHints();
    ClientHints(const ClientHints&);
    ClientHints& operator=(const ClientHints&);
    ClientHints(ClientHints&&);
    ClientHints& operator=(ClientHints&&);
    ~ClientHints();

    // "Windows", "macOS" or "Linux" (the Sec-CH-UA-Platform value).
    std::string platform;
    // Dotted numbers, "10.0.0".
    std::string platform_version;
    // "x86" or "arm".
    std::string architecture;
    // "32" or "64".
    std::string bitness;
    // Desktop personas only: always false.
    bool mobile = false;
    bool operator==(const ClientHints&) const = default;
  };

  struct Screen {
    int width = 0;
    int height = 0;
    double device_pixel_ratio = 1;
    int color_depth = 24;
    bool operator==(const Screen&) const = default;
  };

  // What WebGL's WEBGL_debug_renderer_info reports as UNMASKED_VENDOR_WEBGL /
  // UNMASKED_RENDERER_WEBGL (the GPU strings). Optional: both empty means no
  // override (the real GPU strings); a persona never has only one of them.
  // Nothing else of WebGL is covered (no limits, extensions, GLSL/version
  // strings or rendering output): see FINGERPRINT.md.
  struct WebGL {
    WebGL();
    WebGL(const WebGL&);
    WebGL& operator=(const WebGL&);
    WebGL(WebGL&&);
    WebGL& operator=(WebGL&&);
    ~WebGL();

    // "Google Inc. (NVIDIA)".
    std::string vendor;
    // "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)".
    std::string renderer;
    bool empty() const { return vendor.empty() && renderer.empty(); }
    bool operator==(const WebGL&) const = default;
  };

  FingerprintPersona();
  FingerprintPersona(const FingerprintPersona&);
  FingerprintPersona& operator=(const FingerprintPersona&);
  FingerprintPersona(FingerprintPersona&&);
  FingerprintPersona& operator=(FingerprintPersona&&);
  ~FingerprintPersona();
  bool operator==(const FingerprintPersona&) const = default;

  // Schema version of the stored value (kFingerprintPersonaVersion).
  int version = 1;
  // Internal: stable per-profile randomness. Never returned by MCP, logs or
  // errors.
  uint64_t seed = 0;
  // "windows-chrome", "macos-chrome" or "linux-chrome": the baseline the
  // other fields were generated from (see IsKnownFingerprintPreset).
  std::string preset;
  // "windows", "macos" or "linux".
  std::string os;
  // navigator.platform: "Win32", "MacIntel" or "Linux x86_64".
  std::string platform;
  // "Chrome" (the only brand in version 1).
  std::string browser_brand;
  // "major.0.build.patch", four dotted numbers in Chrome's scheme (see
  // CheckFingerprintPersona).
  std::string browser_version;
  // The reduced user agent for `os` and the major of `browser_version`.
  std::string user_agent;
  ClientHints client_hints;
  // BCP 47, "en-US".
  std::string locale;
  // navigator.languages: 1-8 entries, the first one is `locale`.
  std::vector<std::string> languages;
  // IANA time zone id, "America/New_York".
  std::string timezone;
  int hardware_concurrency = 8;
  // navigator.deviceMemory (GB): 0.25, 0.5, 1, 2, 4 or 8.
  double device_memory = 8;
  Screen screen;
  // Empty unless set explicitly with `webgl` (or generated on request with
  // FingerprintGenerationOptions::webgl; MCP presets never set it).
  WebGL webgl;
};

inline constexpr int kFingerprintPersonaVersion = 1;

// Result of reading a stored persona. A value that can't be read (not a
// dictionary, unknown version, missing, unknown or mistyped field, incoherent
// values) is kCorrupt, NOT kNone: it is reported as unreadable and never
// applied in part.
struct StoredFingerprintPersona {
  StoredFingerprintPersona();
  StoredFingerprintPersona(const StoredFingerprintPersona&);
  StoredFingerprintPersona& operator=(const StoredFingerprintPersona&);
  StoredFingerprintPersona(StoredFingerprintPersona&&);
  StoredFingerprintPersona& operator=(StoredFingerprintPersona&&);
  ~StoredFingerprintPersona();

  enum class State { kNone, kPersona, kCorrupt };
  State state = State::kNone;
  std::optional<FingerprintPersona> persona;  // set iff state == kPersona
  // kPersona only: the stored value claimed another Chrome branch and
  // `persona` was moved onto this browser's version
  // (NormalizeBrowserVersionToEngine). The owner should store it back.
  bool version_migrated = false;
};

// A persona reports the Chrome it runs on (major and branch build of this
// browser; see CheckFingerprintPersona). A persona whose browser version has
// Chrome's shape but another major or build (stored by a Shuttle on another
// branch) gets this browser's version, and its user agent follows. Returns
// whether it changed. A malformed version is left alone (it stays an error).
bool NormalizeBrowserVersionToEngine(FingerprintPersona* persona);

// For a set-fingerprint input that was stored before (a managed profile's
// persona intent in the registry), not for new input: a `browserVersion` of
// another Chrome branch is removed, so the persona gets this browser's version
// the way a stored persona does (NormalizeBrowserVersionToEngine). New input
// with such a version is refused by ParseFingerprintPersonaInput.
void DropForeignBrowserVersion(base::DictValue& input);

// "windows-chrome", "macos-chrome", "linux-chrome".
bool IsKnownFingerprintPreset(std::string_view preset);

// What a generated persona is built around, besides the preset and the seed.
// The defaults are what GenerateFingerprintPersona(preset, seed) has always
// produced.
struct FingerprintGenerationOptions {
  // A locale of IsKnownFingerprintLocale (its languages and usual time zone
  // follow), or empty for "en-US" / America/New_York.
  std::string_view locale;
  // Also pick the WebGL GPU strings (`webgl`) from a catalog of GPUs of the
  // preset's os (and, on macOS, of the persona's architecture). Off by
  // default: no preset has ever set `webgl`. The catalog is only the
  // UNMASKED_VENDOR/RENDERER strings; limits and extensions are not modelled.
  bool webgl = false;
};

// "en-US", "de-DE", ...: a locale GenerateFingerprintPersona can be asked for
// (the others can still be set by hand).
bool IsKnownFingerprintLocale(std::string_view locale);

// A complete persona for `preset` (which must be known) generated as one
// coherent device, not from independent fields: the hardware, screen, platform
// version and architecture are drawn from `seed` among the combinations the
// os has, and anything the draws made impossible together (an Apple Silicon
// Mac with 4 cores) is raised to the nearest possible value. The result always
// passes CheckFingerprintPersona. Deterministic in (preset, seed, options).
FingerprintPersona GenerateFingerprintPersona(std::string_view preset,
                                              uint64_t seed);
// `options.locale` must be empty or known.
FingerprintPersona GenerateFingerprintPersona(
    std::string_view preset,
    uint64_t seed,
    const FingerprintGenerationOptions& options);

// Whether the values of `persona` agree with each other: version, preset/os/
// platform, browser version against this browser (same major and branch
// build, only the patch may differ), user agent against os and browser
// version, client hints against os (platform version range, architecture,
// bitness, desktop only), languages against locale (known language, locale
// first, region before base), hardware and screen against os and architecture
// (Apple Silicon minimums, desktop minimums, pixel ratios), and WebGL against
// os and architecture (vendor, backend, GPU). Returns an error message safe to
// show to an agent (never the seed) or "".
std::string CheckFingerprintPersona(const FingerprintPersona& persona);

// Storage format (the value of the profile pref prefs::kFingerprintPersona):
// every field, snake_case, the seed as 16 hex digits; the optional "webgl"
// ({vendor, renderer}) only when set, so a value stored before it existed
// still reads. The persona must pass CheckFingerprintPersona.
base::DictValue FingerprintPersonaToPrefValue(const FingerprintPersona& persona);
// Strict inverse: exactly the keys above, no others. `value` is the pref's
// user value, or null if absent (kNone).
StoredFingerprintPersona FingerprintPersonaFromPrefValue(
    const base::Value* value);

// Output for MCP/UI, camelCase, same fields as the pref value without the
// seed: {version, preset, os, platform, browser: {brand, version}, userAgent,
// clientHints: {platform, platformVersion, architecture, bitness, mobile},
// locale, languages, timezone, hardwareConcurrency, deviceMemory,
// screen: {width, height, devicePixelRatio, colorDepth}, webgl?: {vendor,
// renderer}} (webgl only when set).
base::DictValue FingerprintPersonaToMcpValue(const FingerprintPersona& persona);

// The `fingerprint` argument of set-fingerprint (profile-level):
//   {preset?, browserVersion?, locale?, languages?, timezone?,
//    hardwareConcurrency?, deviceMemory?,
//    screen?: {width?, height?, devicePixelRatio?, colorDepth?},
//    webgl?: {vendor, renderer} | null}
// (`webgl: null` clears the WebGL override.) Unknown keys are an error (seed, userAgent, os, platform and clientHints
// are not settable: they follow from preset and browserVersion).
//
// With `preset`, the persona is generated from it (with `seed`) and the other
// fields override the generated ones; a `locale` the generator knows also
// brings its usual time zone unless `timezone` is given. Without `preset`,
// `current` (the profile's readable persona) is patched; without `current` a
// preset is required. `languages` follows `locale` unless given. The result is
// checked with CheckFingerprintPersona, so a patch that makes the device
// impossible (a 4-core Apple Silicon Mac, Direct3D WebGL on macOS, a
// browserVersion the platform version can't run) is an error and nothing is
// written. Returns an error message or "".
std::string ParseFingerprintPersonaInput(
    const base::Value& value,
    const FingerprintPersona* current,
    uint64_t seed,
    FingerprintPersona* out);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_FINGERPRINT_PERSONA_H_

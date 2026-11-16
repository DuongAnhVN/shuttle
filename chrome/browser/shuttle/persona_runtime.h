// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PERSONA_RUNTIME_H_
#define CHROME_BROWSER_SHUTTLE_PERSONA_RUNTIME_H_

#include <string>

#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"

namespace blink {
struct RendererPreferences;
}

namespace shuttle {

// How a FingerprintPersona reaches the renderer and the network stack. Pure
// functions of the persona: the same persona always gives the same values, so
// the navigation headers (user agent, Sec-CH-UA-*, Accept-Language) and the
// values scripts read (navigator.*, screen.*) agree. Design:
// tools/shuttle/docs/FINGERPRINT.md, "Runtime".

// The persona's user agent string and client hints (UserAgentMetadata:
// brands, full version list, platform, platform version, architecture,
// bitness, mobile, form factors). Always has metadata, so Sec-CH-UA-* are
// sent (and navigator.userAgentData is filled) from the persona rather than
// disabled by a custom user agent.
blink::UserAgentOverride UserAgentOverrideForPersona(
    const FingerprintPersona& persona);

// Whether ICU knows the IANA time zone id (what the renderer needs to apply
// it as its default time zone). The persona schema only checks the syntax, so
// callers that write a persona (set-fingerprint) check this too.
bool IsKnownTimezoneId(const std::string& timezone);

// The Accept-Language header value of the persona ("en-US,en;q=0.9"): what
// the profile's NetworkContexts send.
std::string AcceptLanguageHeaderForPersona(const FingerprintPersona& persona);

// The comma-separated language list the renderer reports as navigator.languages
// ("en-US,en", RendererPreferences::accept_languages).
std::string RendererAcceptLanguagesForPersona(const FingerprintPersona& persona);

// The persona os the host is, from the build flags: "windows" (IS_WIN), "macos"
// (IS_MAC) or "linux" (IS_LINUX, which includes ChromeOS builds); "" for any
// other platform, which no persona describes (so nothing matches there).
const char* HostPersonaOs();

// Native WebGPU policy: whether WebGPU is blocked (GPU.requestAdapter()
// resolves null) while `persona` is applied. The real host adapter is exposed
// only when it can't contradict the persona: blocked if persona.os is not the
// host os, or if the persona overrides the WebGL GPU strings. No adapter
// value is ever fabricated. Pure function of the persona and the host os.
bool IsWebGpuBlockedForPersona(const FingerprintPersona& persona);

// Sets the persona's fields of `prefs`: user_agent_override, accept_languages,
// navigator_platform_override, hardware_concurrency_override,
// device_memory_override, screen_*_override, device_pixel_ratio_override,
// timezone_override and locale_override (the process-wide ICU default time zone
// and locale of the renderer: Date, Intl.*; see FINGERPRINT.md) and
// webgl_unmasked_{vendor,renderer}_override (empty unless the persona has a
// webgl; no other WebGL value is set) and webgpu_blocked
// (IsWebGpuBlockedForPersona).
void ApplyPersonaToRendererPrefs(const FingerprintPersona& persona,
                                 blink::RendererPreferences* prefs);

// Resets the fields ApplyPersonaToRendererPrefs sets, except accept_languages
// (the caller derives it from the profile again): the renderer reports real
// values.
void ClearPersonaFromRendererPrefs(blink::RendererPreferences* prefs);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PERSONA_RUNTIME_H_

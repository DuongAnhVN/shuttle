# Fingerprint persona

A Chromium **profile** carries at most one **fingerprint persona**: one coherent description of the device the profile
presents to sites (operating system, browser, user agent, client hints, locale, time zone, cores, memory, screen). It is
shared by **every tab, agent and storage partition** of the profile. There is no per-agent persona: two agents of the
same profile show the same device, a throwaway agent shows it too, and a tab that is not an agent's shows it as well.

> **Status: schema (step 1) and runtime application of a subset (step 2), at profile level.** See "Runtime" for exactly
> which fields reach pages and requests and which do not. A field that is stored and returned over MCP but not listed as
> applied has **no effect on what sites see**.

Code: `chrome/browser/shuttle/fingerprint_persona.{h,cc}` (pure data, no service dependency); set and read through the
`set-fingerprint` / `get-fingerprint` tools (`mcp_tools.cc`). Runtime: `persona_runtime.{h,cc}`,
`profile_persona_service{,_factory}.{h,cc}` (`agent_persona` source_set: what the network stack needs; it depends on
none of `agent_proxy`, `agent_partition`, `ShuttleService`, the registry or any WebContents code), `persona_tab_prefs.{h,cc}`
(`persona_tab_prefs` source_set: WebContents, observer and worker preferences; used by `proxy_auth` in `agent_proxy_auth`
and by the content browser client), schema in `fingerprint_persona`, pref name in `pref_names`. Probes: `smoke-fingerprint-persona.mjs` (schema, storage, MCP),
`smoke-fingerprint-runtime.mjs` (what pages and requests see).

## Schema (version 1)

Stored in the **profile pref** `prefs::kFingerprintPersona` (`"shuttle.fingerprint_persona"`), registered by
`ProfilePersonaServiceFactory`; not nested in `shuttle.profiles`, not synced. No user value means no persona.

| Key | Value |
| --- | --- |
| `version` | `1`. Any other version is corrupt (see below). |
| `seed` | 16 lower-case hex digits. Internal randomness for later per-profile variation; **never in MCP output, logs or errors**. |
| `preset` | `windows-chrome`, `macos-chrome` or `linux-chrome`: the baseline the persona was generated from. |
| `os`, `platform` | `windows`/`macos`/`linux` and `navigator.platform` (`Win32`, `MacIntel`, `Linux x86_64`). |
| `browser` | `{brand: "Chrome", version: "major.0.build.patch"}` in Chrome's scheme: major 100-160 (the engine is 158; a persona does not claim a much newer Chrome), minor 0, build 1000-99999, patch 0-999. |
| `user_agent` | The reduced user agent for the os and the browser major (`Chrome/<major>.0.0.0`). |
| `client_hints` | `{platform, platform_version, architecture: x86\|arm, bitness: "64", mobile: false}`. |
| `locale`, `languages` | BCP 47 (`en-US`) with a base language Chrome offers as an accept-language; 1-8 distinct entries, the first is the locale, and a region language (`en-US`) comes before its base language (`en`). |
| `timezone` | IANA id (`Europe/Paris`). The schema only checks the syntax; `set-fingerprint` additionally refuses an id ICU does not know (`IsKnownTimezoneId`, the same test Blink applies), so a stored zone is one the renderer can apply. |
| `hardware_concurrency`, `device_memory` | 2-64; 0.25, 0.5, 1, 2, 4 or 8 (a desktop persona: at least 2; above 16 cores, 8). |
| `screen` | `{width, height, device_pixel_ratio, color_depth}`: landscape, 320-7680 x 240-4320 and at least 1024x600 (desktop), ratio 1-4 in steps of 0.25 (1-3 on Windows and Linux, 1 or 2 on macOS), depth 24 or 30. |
| `webgl` | **Optional** (absent = no override; written only when set, so values stored before it still read). `{vendor, renderer}`: printable ASCII, 1-200 chars, `vendor` = `Google Inc. (X)`, `renderer` = `ANGLE (X, gpu, backend)` with the same `X` (parentheses balanced, three parts or more), and for the os exactly one backend family: Windows `Direct3D`, macOS `Metal`, Linux `OpenGL` or `Vulkan` (a renderer mentioning another family is refused). `Apple` GPUs only on macOS, and an Apple Silicon Mac (`client_hints.architecture` `arm`) must report `Google Inc. (Apple)`. Both or neither. A `preset` over MCP never generates it (it clears it); set with `set-fingerprint {fingerprint: {webgl: {vendor, renderer}}}`, `webgl: null` clears. In code, `FingerprintGenerationOptions::webgl` picks a pair from a small catalog of GPUs of the os and architecture (not reachable over MCP). |

Reading is strict: every key above must be present with the right type and no other key may exist.

**Coherence** (`CheckFingerprintPersona`, applied when reading, building and writing): os matches the preset and the
platform; the user agent must be exactly the one built from the os and browser major; client hint platform matches the
os; non-macOS personas are `x86` (their user agent says x86_64), bitness is 64 and `mobile` is false (desktop only: the
screen is at least 1024x600, memory at least 2 GB); the platform version is one of the os (Windows 10-25, macOS 10-30 and
12+ for Chrome 140+, Linux kernel 3-9); an Apple Silicon Mac (`arm`) has at least 8 cores, 8 GB, a ratio of 1 or 2 and an
Apple GPU if `webgl` is set; the first language is the locale; `webgl` agrees with the os and architecture (see its row).
An incoherent value is never written.

**Generation.** `GenerateFingerprintPersona(preset, seed[, options])` builds one device, not independent fields: the
preset gives the os, platform and user agent; the seed draws cores, memory, screen (with its ratio), platform version and,
on macOS, the architecture from the combinations that os has; draws the platform made impossible together (an Apple Silicon
Mac with 4 cores or 4 GB) are raised to the smallest possible value, so seeds that were already coherent keep their
persona. The order of the draws is part of the format (the optional WebGL pick has its own random stream). Over MCP a
`locale` the generator knows (en-US, en-GB, de-DE, fr-FR, ja-JP, ... a table in `fingerprint_persona.cc`) given with a
`preset` also brings its usual time zone, unless `timezone` is given; another valid locale only replaces the default
(en-US, America/New_York). A persona over MCP has no codec, font or other capability fields, and none are generated.
A user patch is applied on top and the whole result is checked, so a patch that makes the device impossible (for example
`webgl` with Direct3D on a macOS preset, or 4 cores on an Apple Silicon Mac) is refused and nothing is written. A seed the
profile's persona already has is kept across patches and presets, so the combination an MCP user gets is stable.

A stored persona that was valid before these rules and breaks one of them now reads as `corrupt` (unreadable) until it is
set again.

**Corrupt state:** a stored value that can't be read (not a dictionary, unknown version, missing/unknown/mistyped key,
incoherent values) is `corrupt`, not "none". It is never applied in part and never silently dropped: `get-fingerprint`
shows `fingerprint: {unreadable: true}`. Fix it with `set-fingerprint {fingerprint: {preset, ...}}`
(replaces it) or `fingerprint: null` (removes it); a patch without `preset` is refused while it is unreadable.

**Legacy per-agent values.** An earlier, never released draft stored the persona per agent, as key `fingerprint_persona`
of a `shuttle.profiles` entry. That layout is **not read**: `ProfilePersonaService` deletes the leftover keys when the
profile starts (`DropLegacyAgentPersonas`) and nothing is migrated into the profile pref. Set the profile persona again
with `set-fingerprint`.

## MCP

See `docs/MCP.md` "Fingerprint persona". `set-fingerprint {fingerprint}` input: `{preset?, browserVersion?, locale?,
languages?, timezone?, hardwareConcurrency?, deviceMemory?, screen?: {width?, height?, devicePixelRatio?, colorDepth?}, webgl?: {vendor, renderer} \| null}`
or `null` (clear); unknown fields are errors (`seed`, `userAgent`, `os`, `platform`, `clientHints` can't be set: they follow
from `preset` and `browserVersion`). `preset` generates a full persona (hardware, screen, platform version picked from the
seed); the other fields override it. Without `preset` the current persona is patched. `get-fingerprint` returns the
persona without its seed; `list-agents` does not carry it (it is not an agent property). `create-agent` /
`update-agent` **refuse** a `fingerprint` argument (it is profile-level). The `set-fingerprint` schema has
`additionalProperties: false` on `fingerprint` and on `screen`, matching the runtime, which rejects unknown keys.

## Runtime

The profile's persona is read through `ProfilePersonaService` (a per-profile keyed service over the profile pref; the MCP
tools write through it and it notifies on change). Everything below is derived from the persona by pure functions
(`persona_runtime.cc`), so headers and scripts agree. No JavaScript injection, no CDP overrides, no Playwright: the values
come from the browser's own plumbing. A profile without a readable persona (none, or `unreadable`) is untouched: stock
values, as before. An off-the-record profile has no persona service and shows stock values.

| Persona field | Applied | Where |
| --- | --- | --- |
| `user_agent` | **yes** | `User-Agent` of the page requests of every tab (`blink::UserAgentOverride` in the tab's renderer preferences, registered with `WebContents::SetUserAgentOverride(..., override_in_new_tabs=true)`; each HTTP(S) main-frame navigation is switched to use it with `NavigationHandle::SetIsOverridingUserAgent` from `DidStartNavigation` of a per-tab observer, so the first navigation of a tab, reloads, history navigations and renderer-initiated ones carry it, when the tab went through `PrefsTabHelper`) and `navigator.userAgent` / `appVersion`; also the `NetworkContext` user agent of the profile's contexts, **only when a context is created** (see below) |
| `browser.version`, `client_hints.*` | **yes** | `UserAgentMetadata` of the same override: `Sec-CH-UA` (brands `Chromium`, `Google Chrome`, a GREASE brand), `-Mobile`, `-Platform`, and on request `-Platform-Version`, `-Arch`, `-Bitness`, `-Full-Version-List`, `-Model` (empty), `-Form-Factors` (`Desktop`), `navigator.userAgentData` and `getHighEntropyValues` |
| `languages`, `locale` | **yes** | `Accept-Language` header (all the profile's `NetworkContext`s, set at creation and updated live when the persona changes) and `navigator.language` / `languages` (renderer preferences `accept_languages`) |
| `platform` | **yes** | `navigator.platform` (window) |
| `hardware_concurrency` | **yes** | `navigator.hardwareConcurrency` (window; a DevTools emulation override still wins) |
| `device_memory` | **yes** | `navigator.deviceMemory` (window) |
| `screen.width`, `screen.height` | **yes** | `screen.width/height/availWidth/availHeight` (no reserved area; `availLeft/Top` 0). Not `window.innerWidth`, `outerWidth`, `matchMedia` `device-width` or the multi-screen API |
| `screen.color_depth` | **yes** | `screen.colorDepth` / `pixelDepth` |
| `screen.device_pixel_ratio` | **yes (scripts only)** | `window.devicePixelRatio`. Layout and painting keep the real ratio, so `matchMedia('(resolution)')`, `image-set` and `<img srcset>` choices still use the real display |
| `timezone` | **yes** (phase 1: not compiled or run yet) | The renderer process' **ICU default time zone**, with V8 notified: `Date` (local time, `getTimezoneOffset`, `toString`, DST rules of the zone), `Intl.DateTimeFormat` (`resolvedOptions().timeZone`, formatting), `toLocale*String`, in pages and in the dedicated workers of that process. See "Time zone and locale" below |
| `locale` (Intl side) | **yes** (phase 1: not compiled or run yet) | The renderer process' **ICU default locale**, V8 notified: `Intl.*` default locale (`DateTimeFormat`, `NumberFormat`, `Collator`, `PluralRules`, ... `resolvedOptions().locale`), `toLocale*String`, `localeCompare`. `navigator.language(s)` and `Accept-Language` come from `languages` (above), so both agree: the first language is the locale. Blink's own form-control/date-input localization (it follows the page language, not the ICU default) is not changed |
| `seed` | **no** | Reserved for later noise; not used yet |
| `webgl.vendor`, `webgl.renderer` | **yes, only these two values** (when the persona has `webgl`) | `WebGLRenderingContext`/`WebGL2RenderingContext.getParameter(UNMASKED_VENDOR_WEBGL / UNMASKED_RENDERER_WEBGL)` (only once `WEBGL_debug_renderer_info` is enabled, as in stock), in the page and in dedicated workers (OffscreenCanvas): renderer preferences `webgl_unmasked_{vendor,renderer}_override`, read in `WebGLRenderingContextBase::getParameter`; applied only when both are non-empty. **Not changed:** `VENDOR` / `RENDERER` (already the constant `WebKit` / `WebKit WebGL`), `VERSION` and `SHADING_LANGUAGE_VERSION` (they embed the real ANGLE/GL strings and are not persona fields), every limit and parameter (`MAX_TEXTURE_SIZE`, ...), `getSupportedExtensions`/`getExtension`, `getShaderPrecisionFormat`, rendering output (readPixels / canvas hash), WebGPU `GPUAdapterInfo` (see the WebGPU row: blocked instead). A persona's GPU string can therefore disagree with the real limits/extensions: pick the string of a GPU class close to the real one. WebGPU contexts (`webgl_rendering_context_webgpu_base`) are not covered. Not compiled or run yet |
| WebGPU (`navigator.gpu`) | **yes, as a block only** (no schema field; derived from `os` and `webgl`; not compiled or run yet) | Native policy `IsWebGpuBlockedForPersona` (`persona_runtime.cc`): the **real host adapter** is exposed only when it can't contradict the persona; otherwise `GPU.requestAdapter()` resolves **`null`**. Blocked when `persona.os` is not the host os (a Windows/macOS persona on a Linux host, ...) **or** the persona has a `webgl` override (it claims a GPU string the adapter is not known to be). No `GPUAdapterInfo` vendor/architecture/description, limit or feature is ever invented or edited: either the host's real values (compatible persona) or no adapter. Plumbed as the renderer preference `webgpu_blocked`, read in `GPU::requestAdapter` (`modules/webgpu/gpu.cc`) before any GPU-process or Dawn contact, in pages and dedicated workers (workers: best effort, same limits as the other fields). Nothing in the GPU process or Dawn is changed, and no other profile is affected. **Not covered:** `navigator.gpu` itself stays (an object; the IDL attribute is non-nullable) and `navigator.gpu.getPreferredCanvasFormat()` / `wgslLanguageFeatures` still answer from static Blink values (host-OS-dependent, not adapter data); a compatible persona's adapter still has the real GPU's limits/features (pick `webgl` strings only with the block in mind: setting `webgl` turns WebGPU off); WebGL (`getParameter` limits/extensions) is unchanged and independent of this block; WebNN/other users of the Dawn context are not gated. A blocked page cannot tell "blocked" from "no usable GPU" except that, unlike a real failure, no console warning is printed |
| canvas, audio, fonts, media/codecs, WebRTC SDP/stats/candidates | **no** | No schema field, nothing spoofed |
| WebRTC IP handling | **no persona field**; native policy only | Unchanged by the persona and by this phase: `webrtc_policy.{h,cc}` picks Chromium's own `disable_non_proxied_udp` for a proxied agent's tabs or a profile with an explicit proxy (`docs/WEBRTC.md`). No SDP, stats or network values are faked, and the persona does not weaken or strengthen that policy |

**How it is plumbed.** `ApplyAgentRendererPrefs` (called from `PrefsTabHelper`'s constructor and every renderer-prefs
update) first calls `ApplyProfilePersonaToWebContents` for **every** `WebContents` of the profile, agent or not and
whatever its storage partition (the agent id is only used afterwards, for the WebRTC proxy policy). That sets the persona's
fields of the tab's `blink::RendererPreferences`: the existing `user_agent_override` and `accept_languages`, plus seven
fields added to `renderer_preferences.mojom` (`navigator_platform_override`, `hardware_concurrency_override`,
`device_memory_override`, `screen_{width,height,color_depth}_override`, `device_pixel_ratio_override`, plus
`webgpu_blocked` (bool); empty/0/false = stock, so
a profile without a persona is unchanged). Blink reads them from `Page::GetRendererPreferences()` in `navigator_base.cc`,
`screen.cc` and `local_dom_window.cc`. No `content/` API was added: the user agent goes through the existing
`SetUserAgentOverride` and `NavigationHandle::SetIsOverridingUserAgent`. The network side is
`ProfilePersonaService::ConfigureNetworkContextParams` (called by `ProfileNetworkContextService` for every context of the
profile, main and partitions, with no agent id).

**Time zone and locale (phase 1).** Two more fields of `renderer_preferences.mojom`, `timezone_override` (IANA id) and
`locale_override` (BCP 47), carry `persona.timezone` and `persona.locale` to the renderer. They are not read by a page API:
`Page::SetRendererPreferences` (now out of line, `page.cc`) compares them with the previous ones and, when they changed,
applies them to the **renderer process**:
- `TimeZoneController::SetPersonaTimeZone(id)` (`core/timezone/`): sets the ICU default time zone
  (`icu::TimeZone::adoptDefault`) and notifies V8 on the main thread and on every worker thread of the process
  (`DateTimeConfigurationChangeNotification`, plus the `timezonechange` event where enabled) - the same notifications as a
  host time zone change, so `Date` and `Intl.DateTimeFormat` follow. The persona zone is a **base layer**, not the exclusive
  DevTools override: host time zone changes are remembered but not applied while a persona zone is set; a DevTools
  `Emulation.setTimezoneOverride` (if a client uses it) still takes precedence while it is held, and releasing it falls back
  to the persona zone, not the host's. An id ICU does not know is refused and changes nothing.
- `LocaleController::SetPersonaLocale(locale)` (`core/inspector/locale_controller`): sets the ICU default locale
  (`base::i18n::SetDefaultIcuLocale`) and calls `LocaleConfigurationChangeNotification` and
  `DateTimeConfigurationChangeNotification` on every isolate of the process (main thread and workers), the same path as
  `Emulation.setLocaleOverride`, again as a base layer under a DevTools override.
No `Emulation`/inspector trust gate or session is involved, and nothing is injected. These two calls change process-wide
ICU state; the "Scope" paragraph below says what keeps that inside one profile. `navigator.language(s)` and
`Accept-Language` come from `languages` as before, so Date/Intl, `navigator.languages` and the header agree (the first
language is the locale).

**Scope (read this before relying on it).** ICU's default time zone and locale are **process-global**: the change reaches
every page and worker thread of the renderer process, and there is **no per-frame or per-page isolation**. Isolation
between profiles is therefore not provided by Blink; it rests on an embedder-side invariant plus one enforcement:
- *Invariant (read in the source, not tested here):* content never puts two `BrowserContext`s (profiles) in one renderer
  process: `RenderProcessHostImpl::IsSuitableHost` returns false for a host of another context
  (`render_process_host_impl.cc`) and spare processes are matched to their context
  (`spare_render_process_host_manager_impl.cc`). So the persona of a profile reaches only that profile's renderer
  processes. Inside the profile every tab, agent and partition has the same persona anyway.
- *Enforcement:* where that invariant does not hold, namely renderers running in the browser process
  (`--single-process`, `RenderProcessHost::run_renderer_in_process()`), the browser **does not send** `timezone_override` /
  `locale_override` at all (`EnforceProcessScopedIntl` in `persona_tab_prefs.cc`): the time zone and locale are then not
  applied (everything else still is), rather than changing every profile and the browser UI. Workers' preferences never
  carry them.
- *Not verified:* no test runs two profiles side by side; other embedder modes that share a renderer between contexts (none
  known in this tree) would break the invariant. Do not read this as proven isolation.
Inside a process several pages carry the same values.
A page without a value (no persona, or a web contents that never got the persona's renderer preferences) does not undo the
value another page of the same process still carries; the last page to lose it restores the host time zone and the
embedder's locale. A web contents of the profile that never goes through the persona preferences (not in a tab strip, no
`PrefsTabHelper`) therefore cannot reset the process, but it also cannot set it.

**Changing a persona.** `set-fingerprint` writes the pref and `ProfilePersonaService` notifies; the callback is not
agent-specific: `ProxyTabSync` re-applies the renderer preferences of **every open tab of the profile** (tab strips) and
`ProfileNetworkContextService` sends the new `Accept-Language` to every live `NetworkContext`. What is live and what is
not:

| What | When a change shows |
| --- | --- |
| Renderer values of open tabs (platform, cores, memory, screen, devicePixelRatio) | At once for the tabs in a tab strip (renderer preferences are re-sent), on the next script read |
| Time zone and locale (Date, Intl, `getTimezoneOffset`, `toLocale*String`) of open tabs | At once for the tabs in a tab strip, in the page and in the dedicated workers of its renderer processes (the process-wide ICU default changes and V8 is notified); no navigation needed; **not at all in `--single-process`** (not sent, see "Scope"). Already-formatted strings and `Intl` objects a page created earlier keep what they were built with (V8 caches them per object); new `Date`/`Intl` calls follow. An open tab of the profile outside a tab strip follows at its next renderer-prefs update |
| User agent, `Sec-CH-UA*`, `navigator.userAgent(Data)`, `navigator.language(s)` of a page | The page already shown keeps what it was loaded with (the renderer reads them per document); the tab's **next navigation or reload** |
| `Accept-Language` header | At once, for every live `NetworkContext` (`SetAcceptLanguage`) |
| `NetworkContext` user agent | **Only for contexts created after the change** (browser restart, or a partition created later); Chromium has no live setter and the runtime does **not** recreate network contexts. A context created before keeps the persona (or stock user agent) it was created with |

Page requests are not affected by the last row: a renderer sends its own `User-Agent` header from the tab's renderer
preferences. Only requests the network service makes without a renderer-supplied `User-Agent` use the context value, so
after a change those can disagree with the pages until the browser restarts. Web contents that are not in a tab strip
(popups, devtools, prerenders) pick the change up through their next renderer-prefs update or when created.

**Clearing, and history entries.** Clearing the persona (`fingerprint: null`) restores the stock values at the next
navigation the same way. Every `NavigationEntry` remembers whether its navigation used the user agent override; an entry
created under a persona keeps that flag, and the same is true for entries restored from a session. With no persona and no
override set, such a flag would also switch `Sec-CH-UA*` off, so the observer (attached to every tab of a profile with
a persona service) calls `SetIsOverridingUserAgent(false)` for the HTTP(S) main-frame navigation when an entry is still
flagged. **Limit:** the flag of an entry is only corrected when that entry is navigated (back/forward, reload); entries
that are not visited again keep it, and tabs without the observer (never went through `PrefsTabHelper`), `chrome://`,
`about:` and `file:` navigations are not corrected. No `content/` change was made to rewrite the flags of all entries.

**Known limits.**
- A **corrupt** stored persona is not applied (never in part): the profile shows the real browser. It is reported as
  `{unreadable: true}`; fix or clear it.
- **Workers: partially.** *Time zone and locale:* dedicated workers run inside a renderer process of the page that
  created them and share its ICU default; they are notified of a change (`CallOnAllWorkerThreads`) and read the new
  default when they start, so a dedicated worker of a persona tab should report the persona's `Intl` time zone/locale
  (the runtime probe prints it; not claimed until it has been run). **Shared and service workers** run in processes that
  may have no page, hence never receive the time zone/locale renderer preferences (the hook is `Page`): their `Intl`/`Date`
  can report the host's. *Everything else:* not supported, nothing is claimed. Dedicated, shared and service workers are
  not guaranteed to show the rest of the persona. As a best effort,
  `ChromeContentBrowserClient::UpdateRendererPreferencesForWorker` (which only knows the profile) sets the persona's
  renderer-preference fields (`ApplyProfilePersonaToWorkerPrefs`), but this was not verified end to end. A worker's own
  `navigator.userAgent`, `navigator.userAgentData` and `Sec-CH-UA*` do not come from those preferences (service worker
  script requests use the context user agent, which is stock until a context is created with the persona), so workers
  can report or send the real values. The runtime probe only prints what a dedicated worker reports.
- `devicePixelRatio` is reported, not rendered; a persona ratio different from the real display is detectable by
  `matchMedia`.
- A persona screen smaller than the real window makes `screen` smaller than `innerWidth`: pick sizes at least as large as
  the tab (headless default 1280x720 fits all presets).
- Time zone/locale are not **per-page**: two tabs of the same profile always show the same ones (one persona per profile),
  and a renderer process is shared by the tabs of the profile, so the zone cannot differ per tab anyway.
- Date/time **display** and parsing in Blink itself (`<input type=date|time>`, form-control locale) use the page language
  and the system's, not the ICU default: they are not changed. An `Intl.DateTimeFormat` created with an explicit
  `timeZone` or locale argument is the page's own choice and is not touched.
- The ICU default is changed after the process starts: a script that cached a `Date`/`Intl.DateTimeFormat` before the
  change (or before the page got the preferences, for the first page of a process) keeps the old one; the first
  navigation of a new tab gets the persona's prefs before its document runs.
- `Date`'s timezone offset in the **renderer** only: browser-side code (cookies' expiry, HTTP `Date` headers, downloads) is UTC
  or host-based and unaffected.
- `ReduceAcceptLanguage` (off by default) would add its own header on navigations and win over the persona's.
- Prerendered pages use a separate navigation controller and are cancelled on a user agent mismatch (stock behavior).
- Non-HTTP(S) navigations (`chrome://`, `about:`, `file:`) are not switched to the user agent override.

## Not implemented

Canvas, audio, fonts, media/codecs, WebRTC SDP/stats/candidate spoofing, shared/service worker support (also for the time
zone/locale), per-profile noise from the seed, a native automation controller. (WebGL: see the `webgl` row above.)

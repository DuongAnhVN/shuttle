// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PERSONA_TAB_PREFS_H_
#define CHROME_BROWSER_SHUTTLE_PERSONA_TAB_PREFS_H_

class Profile;

namespace blink {
struct RendererPreferences;
}

namespace content {
class WebContents;
}

namespace shuttle {

// Applies the fingerprint persona of `profile` (ProfilePersonaService) to
// `contents`, whichever tab, agent or storage partition it shows: the same
// persona for every WebContents of the profile.
//
// Sets in `prefs` (the renderer preferences of `contents`) the user agent with
// its client hints, the languages, navigator.platform, hardwareConcurrency,
// deviceMemory, the screen size and color depth and devicePixelRatio
// (persona_runtime.h). With a persona it also:
//  - registers the user agent with WebContents::SetUserAgentOverride (override
//    in new tabs and renderer-initiated navigations), and
//  - attaches a WebContentsObserver that makes every main-frame navigation of
//    the tab use the override (NavigationHandle::SetIsOverridingUserAgent in
//    DidStartNavigation), so that the first navigation, history navigations
//    and reloads already carry the persona's user agent and Sec-CH-UA-*.
//    A page already shown keeps what it was loaded with until its next
//    navigation or reload.
//
// A profile without a readable persona keeps its values; a persona applied
// earlier and since cleared is removed (the caller derives accept_languages
// from the profile again, as renderer_preferences_util does). A corrupt stored
// persona is not applied in part. The observer is attached to every
// WebContents of a profile that has a persona service, persona or not: with no
// persona and no user agent override it switches the override flag off for
// navigations of history entries still flagged from an earlier persona (a
// flagged entry without an override would also turn Sec-CH-UA* off). Limits:
//  - an entry that is never navigated again keeps its stale flag;
//  - only HTTP(S) main-frame navigations are switched; chrome://, about: and
//    file: keep the entry's own flag;
//  - a WebContents that never gets its renderer preferences applied (no
//    PrefsTabHelper) has no observer and is not covered.
//
// The persona's time zone and locale (timezone_override, locale_override) become
// the ICU defaults of the whole renderer process, which Blink cannot isolate per
// page. They are sent only where a renderer process serves one profile (always,
// except with renderers in the browser process: run_renderer_in_process(), where
// they are left empty) and never with worker preferences.
//
// Call it after renderer_preferences_util::UpdateFromSystemSettings. UI thread.
void ApplyProfilePersonaToWebContents(Profile* profile,
                                      content::WebContents* contents,
                                      blink::RendererPreferences* prefs);

// Worker hook: sets the persona's fields in `prefs` (the renderer preferences
// of a shared or service worker, ChromeContentBrowserClient::
// UpdateRendererPreferencesForWorker, which only knows the profile). Does
// nothing without a readable persona. Dedicated workers do not use this: they
// get their page's preferences (ApplyProfilePersonaToWebContents).
//
// Blink reads these from the worker's renderer preferences, which the browser
// refreshes through PrefWatcher (a persona change reaches live workers):
//  - shared and service workers: user agent string and client hints
//    (WorkerGlobalScope::UserAgent / GetUserAgentMetadata), navigator.platform,
//    hardwareConcurrency, deviceMemory, navigator.languages;
//  - WebGL UNMASKED_VENDOR/RENDERER_WEBGL of a context in any worker (including
//    one made from a transferred OffscreenCanvas).
// Limits: there is no page or agent context here, so only the profile's own
// persona applies; a worker of a profile without a persona is untouched. The
// time zone and locale are not sent (workers do not read them): a worker
// follows the ICU defaults of its renderer process, which a page of the same
// profile in that process sets, so a service or shared worker in a process with
// no page of that profile reports the host time zone and locale.
void ApplyProfilePersonaToWorkerPrefs(Profile* profile,
                                      blink::RendererPreferences* prefs);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PERSONA_TAB_PREFS_H_

// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/persona_tab_prefs.h"

#include <memory>
#include <optional>

#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/fingerprint_persona.h"
#include "chrome/browser/shuttle/persona_runtime.h"
#include "chrome/browser/shuttle/profile_persona_service.h"
#include "chrome/browser/shuttle/profile_persona_service_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "url/gurl.h"

namespace shuttle {

namespace {

const int kPersonaTabStateKey = 0;

// Whether any entry of `contents`' session history is flagged as using the
// user agent override.
bool HasOverridingEntry(content::WebContents* contents) {
  content::NavigationController& controller = contents->GetController();
  for (int i = 0; i < controller.GetEntryCount(); ++i) {
    content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
    if (entry && entry->GetIsOverridingUserAgent()) {
      return true;
    }
  }
  return false;
}

// ICU's default time zone and locale (what timezone_override / locale_override
// become in the renderer) are process-global. Chromium never puts two
// BrowserContexts (profiles) into one renderer process
// (RenderProcessHostImpl::IsSuitableHost rejects a host of another context,
// spare processes are per context), so a profile's persona stays inside its
// own renderer processes - EXCEPT when renderers run inside the browser
// process (--single-process / run_renderer_in_process), where every profile
// and the browser UI share one ICU default. There the two values are never
// sent, so a persona cannot change the time zone or locale of another profile.
void EnforceProcessScopedIntl(blink::RendererPreferences* prefs) {
  if (content::RenderProcessHost::run_renderer_in_process()) {
    prefs->timezone_override.clear();
    prefs->locale_override.clear();
  }
}

// One per WebContents of a profile that has a persona service. Owned by the
// WebContents (user data); observes it.
class PersonaTabState : public content::WebContentsObserver,
                        public base::SupportsUserData::Data {
 public:
  explicit PersonaTabState(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  // Whether the renderer prefs of the tab carry the persona now.
  bool applied() const { return applied_; }
  void set_applied(bool applied) {
    applied_ = applied;
    if (applied) {
      ever_applied_ = true;
    }
  }

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument() ||
        !handle->GetURL().SchemeIsHTTPOrHTTPS()) {
      return;
    }
    // The persona's user agent is the WebContents' override
    // (ApplyProfilePersonaToWebContents). Whether a navigation uses it is a
    // per-navigation flag (NavigationEntry::GetIsOverridingUserAgent) that a
    // stock browser only sets for some navigations, and that history entries
    // keep from when they were created. Setting it here, while the request is
    // being created, covers the first navigation, reloads, history
    // navigations and renderer-initiated ones alike; Chromium copies it to
    // the entry when the navigation commits.
    if (applied_) {
      handle->SetIsOverridingUserAgent(true);
      return;
    }
    // No persona. An entry flagged from an earlier persona (this tab's, or
    // one restored with the session) would use a missing override, which
    // also turns Sec-CH-UA* off: switch it off while no override is set.
    // Entries that are never navigated again keep the stale flag.
    if ((ever_applied_ || HasOverridingEntry(web_contents())) &&
        web_contents()->GetUserAgentOverride().ua_string_override.empty()) {
      handle->SetIsOverridingUserAgent(false);
    }
  }

 private:
  bool applied_ = false;
  bool ever_applied_ = false;
};

}  // namespace

void ApplyProfilePersonaToWebContents(Profile* profile,
                                      content::WebContents* contents,
                                      blink::RendererPreferences* prefs) {
  if (!profile || !contents || !prefs) {
    return;
  }
  ProfilePersonaService* service =
      ProfilePersonaServiceFactory::GetForProfileIfExists(profile);
  if (!service) {
    return;
  }
  std::optional<FingerprintPersona> persona = service->RunPersona();
  auto* state = static_cast<PersonaTabState*>(
      contents->GetUserData(&kPersonaTabStateKey));
  if (!state) {
    auto new_state = std::make_unique<PersonaTabState>(contents);
    state = new_state.get();
    contents->SetUserData(&kPersonaTabStateKey, std::move(new_state));
  }
  if (persona) {
    state->set_applied(true);
    // Before the fields below: SetUserAgentOverride ignores a value that
    // `prefs` (the WebContents' own renderer prefs) already holds, and with it
    // the "override in new tabs" part.
    contents->SetUserAgentOverride(UserAgentOverrideForPersona(*persona),
                                   /*override_in_new_tabs=*/true);
    ApplyPersonaToRendererPrefs(*persona, prefs);
    EnforceProcessScopedIntl(prefs);
    return;
  }
  if (state->applied()) {
    state->set_applied(false);
    contents->SetUserAgentOverride(blink::UserAgentOverride(),
                                   /*override_in_new_tabs=*/false);
    ClearPersonaFromRendererPrefs(prefs);
  }
}

void ApplyProfilePersonaToWorkerPrefs(Profile* profile,
                                      blink::RendererPreferences* prefs) {
  if (!profile || !prefs) {
    return;
  }
  ProfilePersonaService* service =
      ProfilePersonaServiceFactory::GetForProfileIfExists(profile);
  if (std::optional<FingerprintPersona> persona =
          service ? service->RunPersona() : std::nullopt) {
    ApplyPersonaToRendererPrefs(*persona, prefs);
    // Workers never read these two (only Page does, as process-wide ICU
    // defaults); a worker's process may not even be tied to a page, so they
    // are not sent with a worker.
    prefs->timezone_override.clear();
    prefs->locale_override.clear();
  }
}

}  // namespace shuttle

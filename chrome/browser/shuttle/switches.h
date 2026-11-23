// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_SWITCHES_H_
#define CHROME_BROWSER_SHUTTLE_SWITCHES_H_

namespace shuttle {

// With no URL arguments, open a window with chrome://agents (the user's home,
// like a Dock click on macOS). Used by the Linux desktop entry, so a menu
// launch shows a window while terminal and agent launches stay windowless.
// Also applies to a launch forwarded to a running browser.
inline constexpr char kOpenAgentsSwitch[] = "shuttle-open-agents";

// Idle timeout of MCP sessions in minutes (0 = off; "<n>s" = seconds, for
// probes). Default 30. A session can override it (open-session,
// update-session idleTimeoutMinutes).
inline constexpr char kSessionIdleTimeoutSwitch[] =
    "shuttle-session-idle-timeout";
// For probes: idle timeout of every throwaway agent ("<n>s" = seconds, else
// minutes), instead of its idleMinutes.
inline constexpr char kThrowawayIdleTimeoutSwitch[] =
    "shuttle-throwaway-idle-timeout";
// Most throwaway agents alive at once (default 50; 0 = no throwaway agents).
inline constexpr char kMaxThrowawayAgentsSwitch[] =
    "shuttle-max-throwaway-agents";

// Opt-in (off by default): multiple isolated Chromium Profiles in this
// browser, managed through the browser-level tools of
// tools/shuttle/docs/PROFILES.md (create/list/load/stop/delete-browser-profile).
// Without it the browser behaves as before: one startup Profile, legacy
// agents, no ShuttleBrowserProfiles.json, no admin token file.
inline constexpr char kIsolatedProfilesSwitch[] = "shuttle-isolated-profiles";

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_SWITCHES_H_

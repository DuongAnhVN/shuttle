// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PREF_NAMES_H_
#define CHROME_BROWSER_SHUTTLE_PREF_NAMES_H_

namespace shuttle::prefs {

// Dictionary of agent profiles: id -> {"created": <base::Time as value>,
// optional "name", "color" (TabGroupColorId as int), "avatar" (URL),
// kPersistSessionCookies, kProxy}.
inline constexpr char kProfiles[] = "shuttle.profiles";

// Key in a kProfiles entry: false = the agent's session cookies end with the
// browser (missing = true: persisted and restored like the main partition's
// with "Continue where you left off").
inline constexpr char kPersistSessionCookies[] = "persist_session_cookies";

// Key in a kProfiles entry: the agent's own proxy (agent_proxy.h,
// ProxyToPrefValue). Missing = no own proxy (the system proxy). Shape:
// {scheme, host, port, username?, bypass?: [string],
//  password_enc?: base64 os_crypt ciphertext}. The password is never stored
// in plaintext; an unreadable value fails closed (never a direct connection).
inline constexpr char kProxy[] = "proxy";

// Profile-level pref: the ONE fingerprint persona of the Chromium profile,
// shared by every tab, agent and storage partition of it
// (fingerprint_persona.h, FingerprintPersonaToPrefValue): a versioned
// dictionary {version, seed, preset, os, platform, browser, user_agent,
// client_hints, locale, languages, timezone, hardware_concurrency,
// device_memory, screen}. No user value = no persona. A value that can't be
// read (unknown version, unknown or invalid field) is reported as corrupt and
// never applied in part. The runtime reads and writes it through
// ProfilePersonaService (persona_runtime.h says which fields reach the
// renderer and the network). Registered by ProfilePersonaServiceFactory.
inline constexpr char kFingerprintPersona[] = "shuttle.fingerprint_persona";

// Legacy: key "fingerprint_persona" of a kProfiles entry, written by a
// per-agent draft of the feature that never shipped. Never read: the persona
// is profile-level. ProfilePersonaService deletes the leftovers at startup.
inline constexpr char kLegacyAgentFingerprintPersona[] = "fingerprint_persona";

// Profile-level marker of a managed (isolated) Chromium Profile
// (managed_profile_bootstrap.h, tools/shuttle/docs/PROFILES.md section 6):
// {version: 1, core_profile_key: "bp_<32 hex>"}. Written by the bootstrap
// together with the committed persona and the workspace agent entry, through
// PrefService, in ProfileImpl::OnPrefsLoaded. Its presence means "this Profile
// has an isolated identity": a missing or corrupt persona is then a failure,
// never a stock fallback. Absent = legacy/unmanaged Profile, whose semantics
// are unchanged. Registered by ProfilePersonaServiceFactory.
inline constexpr char kManagedProfile[] = "shuttle.managed_profile";
inline constexpr char kManagedProfileVersionKey[] = "version";
inline constexpr char kManagedProfileKeyKey[] = "core_profile_key";
inline constexpr int kManagedProfileVersion = 1;

// List of deleted profile ids whose storage still has to be removed from
// disk (the partition was loaded when the profile was deleted).
inline constexpr char kPendingDeletes[] = "shuttle.pending_deletes";

}  // namespace shuttle::prefs

#endif  // CHROME_BROWSER_SHUTTLE_PREF_NAMES_H_

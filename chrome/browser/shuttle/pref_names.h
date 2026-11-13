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

// List of deleted profile ids whose storage still has to be removed from
// disk (the partition was loaded when the profile was deleted).
inline constexpr char kPendingDeletes[] = "shuttle.pending_deletes";

}  // namespace shuttle::prefs

#endif  // CHROME_BROWSER_SHUTTLE_PREF_NAMES_H_

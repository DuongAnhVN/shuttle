// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_BROWSER_ADMIN_AUTH_H_
#define CHROME_BROWSER_SHUTTLE_BROWSER_ADMIN_AUTH_H_

#include <string>
#include <string_view>

namespace base {
class FilePath;
}

namespace shuttle {

// Browser administration credential of tools/shuttle/docs/PROFILES.md
// section 3: a random 256-bit token in the owner-readable file
// `ShuttleManagerAdminToken` of the user data dir, presented by the manager
// as the `X-Shuttle-Admin-Token` header. It is NOT the network access token
// (network_access.h) and is checked even on loopback. It is never put in a
// URL, in MCP metadata, in an endpoint, in the UI or in a log.
//
// Not yet compiled (see PROFILES.md, "Implementation status").

inline constexpr char kAdminTokenFileName[] = "ShuttleManagerAdminToken";
inline constexpr char kAdminTokenHeader[] = "X-Shuttle-Admin-Token";
inline constexpr size_t kAdminTokenBytes = 32;

// Loads the token from `user_data_dir`, or creates the file (exclusive create
// with mode 0600 on POSIX; on Windows it inherits the private ACL of the user
// data dir) with a new random token. An existing file with an invalid token
// is replaced; on POSIX an existing file readable by group/others is
// tightened to 0600 or, if that fails, rejected. Returns false (admin
// credential unavailable: every admin check then fails) when no usable token
// could be established. Blocking: call at startup. Never logs the token.
bool InitBrowserAdminAuth(const base::FilePath& user_data_dir);

// Whether a token was established by InitBrowserAdminAuth().
bool IsBrowserAdminAuthAvailable();

// Constant-time comparison of `presented` with the stored token. False when
// none is available, when `presented` is empty, or on any mismatch.
bool VerifyBrowserAdminToken(std::string_view presented);

// Pure helpers (also used by tests).
std::string GenerateAdminToken();
// base64url without padding, exactly the length of GenerateAdminToken().
bool IsValidAdminTokenFormat(std::string_view token);

// Test hook: forget the token and, with `token`, install one in memory only.
void SetBrowserAdminTokenForTesting(std::string_view token);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_BROWSER_ADMIN_AUTH_H_

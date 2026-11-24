// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/browser_admin_auth.h"

#include <string>
#include <utility>

#include "base/base64url.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "crypto/random.h"
#include "crypto/secure_util.h"

namespace shuttle {

namespace {

// 32 bytes, base64url without padding.
constexpr size_t kEncodedTokenLength = 43;

struct State {
  std::string token;
};

State& GetState() {
  static base::NoDestructor<State> state;
  return *state;
}

bool WriteNewTokenFile(const base::FilePath& path,
                       const std::string& token,
                       bool replace) {
  if (replace) {
    // The old file is invalid: replace it atomically-enough (the content is
    // useless to anyone until this write completes).
    if (!base::WriteFile(path, token + "\n")) {
      return false;
    }
  } else {
    // Exclusive create: base::File creates POSIX files with mode 0600.
    base::File file(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    if (!file.IsValid()) {
      return false;
    }
    std::string content = token + "\n";
    if (!file.WriteAndCheck(0, base::as_byte_span(content))) {
      return false;
    }
  }
#if BUILDFLAG(IS_POSIX)
  return base::SetPosixFilePermissions(path, 0600);
#else
  return true;
#endif
}

#if BUILDFLAG(IS_POSIX)
// False if the file is readable or writable by group/others and cannot be
// tightened.
bool EnsureOwnerOnly(const base::FilePath& path) {
  int mode = 0;
  if (!base::GetPosixFilePermissions(path, &mode)) {
    return false;
  }
  if ((mode & (base::FILE_PERMISSION_GROUP_MASK |
               base::FILE_PERMISSION_OTHERS_MASK)) == 0) {
    return true;
  }
  return base::SetPosixFilePermissions(path, 0600);
}
#endif

}  // namespace

std::string GenerateAdminToken() {
  std::string token;
  base::Base64UrlEncode(crypto::RandBytesAsVector(kAdminTokenBytes),
                        base::Base64UrlEncodePolicy::OMIT_PADDING, &token);
  return token;
}

bool IsValidAdminTokenFormat(std::string_view token) {
  if (token.size() != kEncodedTokenLength) {
    return false;
  }
  for (char c : token) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool InitBrowserAdminAuth(const base::FilePath& user_data_dir) {
  State& state = GetState();
  state.token.clear();
  base::FilePath path = user_data_dir.AppendASCII(kAdminTokenFileName);

  bool replace = false;
  if (base::PathExists(path)) {
    std::string stored;
    if (base::ReadFileToString(path, &stored)) {
      std::string token(base::TrimWhitespaceASCII(stored, base::TRIM_ALL));
      if (IsValidAdminTokenFormat(token)) {
#if BUILDFLAG(IS_POSIX)
        if (!EnsureOwnerOnly(path)) {
          LOG(ERROR) << "Shuttle: the admin token file has unsafe permissions "
                        "and could not be fixed; browser administration is "
                        "disabled.";
          return false;
        }
#endif
        state.token = std::move(token);
        return true;
      }
    }
    replace = true;
  }

  if (!base::CreateDirectory(user_data_dir)) {
    return false;
  }
  std::string token = GenerateAdminToken();
  if (!WriteNewTokenFile(path, token, replace)) {
    LOG(ERROR) << "Shuttle: could not write the admin token file; browser "
                  "administration is disabled.";
    return false;
  }
  state.token = std::move(token);
  return true;
}

bool IsBrowserAdminAuthAvailable() {
  return !GetState().token.empty();
}

bool VerifyBrowserAdminToken(std::string_view presented) {
  const std::string& token = GetState().token;
  if (token.empty() || presented.empty() || presented.size() != token.size()) {
    return false;
  }
  return crypto::SecureMemEqual(base::as_byte_span(presented),
                                base::as_byte_span(token));
}

void SetBrowserAdminTokenForTesting(std::string_view token) {
  GetState().token = std::string(token);
}

}  // namespace shuttle

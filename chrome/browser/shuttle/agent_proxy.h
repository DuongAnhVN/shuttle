// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_AGENT_PROXY_H_
#define CHROME_BROWSER_SHUTTLE_AGENT_PROXY_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"

namespace net {
class AuthChallengeInfo;
}

namespace os_crypt_async {
class Encryptor;
}

namespace shuttle {

// A proxy one agent (profile) sends its traffic through. Each agent keeps its
// own copy: there is no default and no inheritance (design:
// tools/shuttle/docs/FEATURES.md, "Per-agent proxy").
//
// The password exists in plaintext only in memory (this struct, the login
// delegate). On disk it is encrypted (ProxyToPrefValue); it is never returned
// by MCP, logs or errors (ProxyToRedactedValue, ProxyToDisplayString).
struct ProxySpec {
  // SOCKS4 is not supported: its DNS is resolved locally (leak).
  enum class Scheme { kHttp, kHttps, kSocks5 };

  ProxySpec();
  ProxySpec(const ProxySpec&);
  ProxySpec& operator=(const ProxySpec&);
  ProxySpec(ProxySpec&&);
  ProxySpec& operator=(ProxySpec&&);
  ~ProxySpec();
  bool operator==(const ProxySpec&) const;

  Scheme scheme = Scheme::kHttp;
  // Hostname or IP literal, without brackets, lower case.
  std::string host;
  // 1..65535.
  int port = 0;
  // http/https only. "" = none. A password needs a username.
  std::string username;
  std::string password;
  // Hosts that go direct, in net::ProxyBypassRules syntax
  // ("localhost", "*.internal", "10.0.0.0/8"). At most kMaxProxyBypassEntries.
  // Chromium also bypasses localhost, loopback and link-local destinations
  // implicitly (accepted; "<-loopback>" removes that). An entry that matches
  // every host ("*", "**", ".*", "0.0.0.0/0", "::/0") is refused.
  std::vector<std::string> bypass;
};

inline constexpr size_t kMaxProxyBypassEntries = 64;
inline constexpr size_t kMaxProxyFieldLength = 256;  // user, password, bypass
inline constexpr size_t kMaxProxyHostLength = 253;

// Parsing and validation. Every function returns an error message safe to
// show to an agent (it never contains the password) or "" on success.
//
// "scheme://[user[:password]@]host:port" (scheme: http, https, socks5; the
// scheme is required; user and password are percent-decoded). socks4 is
// refused with a message that says to use socks5.
std::string ParseProxyString(std::string_view text, ProxySpec* out);
// A string (as above) or an object {scheme, host, port, username?, password?,
// bypass?: [string]}. Unknown keys are an error.
std::string ParseProxyValue(const base::Value& value, ProxySpec* out);
// The rules both parsers apply: host and port valid; SOCKS with a username or
// password is refused with exactly "SOCKS proxy with authentication is not
// supported" (no relay); a password needs a username; length and count caps;
// no control characters or spaces in any field; a bypass entry that matches
// every host is refused with exactly "A bypass entry that matches every host is
// not allowed".
std::string CheckProxy(const ProxySpec& spec);

// Output for MCP/UI: {scheme, host, port, username? , hasPassword: bool,
// bypass?}. Never the password.
base::DictValue ProxyToRedactedValue(const ProxySpec& spec);
// "scheme://[user@]host:port" for logs and errors. Never the password.
std::string ProxyToDisplayString(const ProxySpec& spec);

// Storage format (the value of key prefs::kProxy in an agent's kProfiles
// entry). The password is encrypted with os_crypt and base64-encoded; every
// other field is plain.
//
// `encryptor` is the OSCryptAsync instance (asynchronous to obtain; may be
// null while it is not ready). Returns nullopt when the spec is invalid, or
// when it has a password and `encryptor` is null or can't encrypt: the caller
// then stores nothing (never the plaintext password).
std::optional<base::DictValue> ProxyToPrefValue(
    const ProxySpec& spec,
    const os_crypt_async::Encryptor* encryptor);

// Result of reading a stored proxy. A stored proxy that can't be read (corrupt,
// password undecryptable or no `encryptor` to decrypt it, fails CheckProxy) is
// kCorrupt, NOT "none": the agent must never fall back to a direct connection
// because its proxy was damaged (see FailClosedNetConfig).
struct StoredProxy {
  enum class State { kNone, kProxy, kCorrupt };
  State state = State::kNone;
  std::optional<ProxySpec> spec;  // set iff state == kProxy
};
// `value` is the kProfiles entry's prefs::kProxy child, or null if absent
// (kNone). `encryptor` as in ProxyToPrefValue.
StoredProxy ProxyFromPrefValue(const base::Value* value,
                               const os_crypt_async::Encryptor* encryptor);

// Network configuration. net::ProxyConfig with one proxy and no "direct"
// fallback: if the proxy fails, requests fail (they don't leak the real IP).
net::ProxyConfigWithAnnotation ProxyToNetConfig(const ProxySpec& spec);
// What a kCorrupt agent runs with: a config that cannot connect
// (ERR_PROXY_CONNECTION_FAILED), never a direct one.
net::ProxyConfigWithAnnotation FailClosedNetConfig();

// Whether `auth_info` is a proxy challenge (is_proxy) from this spec's
// host:port. Scheme-insensitive (an https proxy answers with the same
// host:port).
bool ProxyMatchesChallenger(const ProxySpec& spec,
                            const net::AuthChallengeInfo& auth_info);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_AGENT_PROXY_H_

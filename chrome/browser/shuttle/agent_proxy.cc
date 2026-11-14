// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/agent_proxy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "net/base/auth.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_host_matching_rules.h"
#include "net/proxy_resolution/proxy_list.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/scheme_host_port.h"

namespace shuttle {

namespace {

constexpr char kSocksAuthError[] =
    "SOCKS proxy with authentication is not supported";
// Longest accepted proxy string (scheme://user:pass@host:port).
constexpr size_t kMaxProxyStringLength = 1024;

// Keys of the proxy object (MCP input) and of the stored value.
constexpr char kKeyScheme[] = "scheme";
constexpr char kKeyHost[] = "host";
constexpr char kKeyPort[] = "port";
constexpr char kKeyUsername[] = "username";
constexpr char kKeyPassword[] = "password";
constexpr char kKeyBypass[] = "bypass";
// Stored only: the os_crypt ciphertext of the password, base64-encoded.
constexpr char kKeyPasswordEnc[] = "password_enc";
constexpr char kKeyHasPassword[] = "hasPassword";

// Records the proxy's traffic for the traffic annotation audit. The agent's
// requests are its own browsing, so cookies are allowed as usual.
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("shuttle_agent_proxy", R"(
      semantics {
        sender: "Shuttle"
        description:
          "Sends the requests of an agent through the proxy that the user or "
          "an MCP client set for that agent."
        trigger:
          "A request made by an agent's tab or by the agent's network context, "
          "including the test-proxy tool."
        data:
          "The requests of the agent's browsing. The proxy user name and "
          "password are sent only to the proxy that asked for them."
        destination: OTHER
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting:
          "Set the agent's proxy with the MCP tools; clear it to go back to "
          "the system proxy."
        policy_exception_justification: "Not implemented."
      })");

std::string_view SchemeName(ProxySpec::Scheme scheme) {
  switch (scheme) {
    case ProxySpec::Scheme::kHttp:
      return "http";
    case ProxySpec::Scheme::kHttps:
      return "https";
    case ProxySpec::Scheme::kSocks5:
      return "socks5";
  }
  return "http";
}

// An invalid scheme makes FromSchemeHostAndPort() return an invalid server,
// which callers turn into a fail-closed config.
net::ProxyServer::Scheme NetScheme(ProxySpec::Scheme scheme) {
  switch (scheme) {
    case ProxySpec::Scheme::kHttp:
      return net::ProxyServer::SCHEME_HTTP;
    case ProxySpec::Scheme::kHttps:
      return net::ProxyServer::SCHEME_HTTPS;
    case ProxySpec::Scheme::kSocks5:
      return net::ProxyServer::SCHEME_SOCKS5;
  }
  return net::ProxyServer::SCHEME_INVALID;
}

// socks4 is refused on its own: its DNS is resolved locally (a leak).
constexpr char kSocks4Error[] =
    "socks4 is not supported (it resolves DNS locally); use socks5";
constexpr char kUnknownSchemeError[] =
    "Unknown proxy scheme (use http, https or socks5)";

// Empty or "" on success; `kSocks4Error`/`kUnknownSchemeError` otherwise.
std::string ParseSchemeText(std::string_view text, ProxySpec::Scheme* out) {
  if (base::EqualsCaseInsensitiveASCII(text, "socks4")) {
    return kSocks4Error;
  }
  std::string lower = base::ToLowerASCII(text);
  if (lower == "http") {
    *out = ProxySpec::Scheme::kHttp;
  } else if (lower == "https") {
    *out = ProxySpec::Scheme::kHttps;
  } else if (lower == "socks5") {
    *out = ProxySpec::Scheme::kSocks5;
  } else {
    return kUnknownSchemeError;
  }
  return std::string();
}

bool IsSocks(ProxySpec::Scheme scheme) {
  return scheme == ProxySpec::Scheme::kSocks5;
}

// True for a byte that can't appear in any proxy field: C0 controls, space,
// DEL. Bytes >= 0x80 are allowed here; UTF-8 is checked separately.
bool HasControlOrSpace(std::string_view text) {
  for (char c : text) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) {
      return true;
    }
  }
  return false;
}

// The canonical host (lower case, IDNA, IP literal normalized), without the
// brackets of an IPv6 literal, or nullopt if `host` isn't a valid host:port
// for `scheme`. `port` must be 1..65535.
std::optional<std::string> CanonicalHost(ProxySpec::Scheme scheme,
                                         std::string_view host,
                                         int port) {
  if (host.empty() || port < 1 || port > 65535) {
    return std::nullopt;
  }
  net::ProxyServer server = net::ProxyServer::FromSchemeHostAndPort(
      NetScheme(scheme), host, static_cast<uint16_t>(port));
  if (!server.is_valid()) {
    return std::nullopt;
  }
  // ProxyServer runs url::CanonicalizeHost (IDNA, lower case, IPv4 and IPv6
  // forms); host_port_pair().host() is that result without IPv6 brackets.
  return server.host_port_pair().host();
}

constexpr char kMatchesEveryHostError[] =
    "A bypass entry that matches every host is not allowed";

// True for a bypass entry that sends every destination direct, which would
// silently disable the proxy. Matching is base::MatchPattern on the hostname
// (net/base/scheme_host_port_matcher_rule.cc), so these all match every host:
//  - a hostname part made only of '*', '.' and '?' ("*", "**", ".*" which
//    becomes "*.*", i.e. every host with a dot, and "*:*" or "*:80" once the
//    port is removed);
//  - a CIDR block with prefix length 0 ("0.0.0.0/0", "::/0"), which matches
//    every IP literal.
// An optional "scheme://" prefix doesn't make the entry narrower here, so it
// is stripped and the entry is refused too.
bool MatchesEveryHost(std::string_view entry) {
  if (size_t separator = entry.find("://");
      separator != std::string_view::npos) {
    entry = entry.substr(separator + 3);
  }
  if (size_t slash = entry.find('/'); slash != std::string_view::npos) {
    std::string_view prefix = entry.substr(slash + 1);
    return !prefix.empty() && prefix.find_first_not_of('0') ==
                                  std::string_view::npos;
  }
  if (size_t colon = entry.rfind(':');
      colon != std::string_view::npos && colon + 1 < entry.size()) {
    entry = entry.substr(0, colon);
  }
  return !entry.empty() && entry.find_first_not_of("*.?") ==
                               std::string_view::npos;
}

base::ListValue BypassList(const std::vector<std::string>& bypass) {
  base::ListValue list;
  for (const std::string& entry : bypass) {
    list.Append(std::string_view(entry));
  }
  return list;
}

// Parses the object form (also the decrypted stored form, which has no
// "password_enc" key). Returns "" or an error that never contains a value.
std::string ParseProxyDict(const base::DictValue& dict, ProxySpec* out) {
  // By value: iterating a DictValue yields temporary (key, value) pairs.
  for (const auto [key, value] : dict) {
    if (key != kKeyScheme && key != kKeyHost && key != kKeyPort &&
        key != kKeyUsername && key != kKeyPassword && key != kKeyBypass) {
      return "Unknown proxy field: " + key.substr(0, 64);
    }
  }

  ProxySpec spec;
  const base::Value* scheme_value = dict.Find(kKeyScheme);
  if (!scheme_value) {
    return "The proxy needs a scheme (http, https or socks5)";
  }
  if (!scheme_value->is_string()) {
    return "The proxy field scheme must be a string";
  }
  std::string scheme_error =
      ParseSchemeText(scheme_value->GetString(), &spec.scheme);
  if (!scheme_error.empty()) {
    return scheme_error;
  }

  const base::Value* host_value = dict.Find(kKeyHost);
  if (!host_value) {
    return "The proxy needs a host";
  }
  if (!host_value->is_string()) {
    return "The proxy field host must be a string";
  }
  const base::Value* port_value = dict.Find(kKeyPort);
  if (!port_value) {
    return "The proxy needs a port";
  }
  if (!port_value->is_int()) {
    return "The proxy field port must be an integer";
  }
  int port = port_value->GetInt();
  if (port < 1 || port > 65535) {
    return "The port must be between 1 and 65535";
  }
  std::optional<std::string> host =
      CanonicalHost(spec.scheme, host_value->GetString(), port);
  if (!host) {
    return "The proxy host is not valid";
  }
  spec.host = *host;
  spec.port = port;

  if (const base::Value* username = dict.Find(kKeyUsername)) {
    if (!username->is_string()) {
      return "The proxy field username must be a string";
    }
    spec.username = username->GetString();
  }
  if (const base::Value* password = dict.Find(kKeyPassword)) {
    if (!password->is_string()) {
      return "The proxy field password must be a string";
    }
    spec.password = password->GetString();
  }
  if (const base::Value* bypass = dict.Find(kKeyBypass)) {
    if (!bypass->is_list()) {
      return "The proxy field bypass must be a list of strings";
    }
    const base::ListValue& list = bypass->GetList();
    if (list.size() > kMaxProxyBypassEntries) {
      return "At most 64 bypass entries";
    }
    for (const base::Value& entry : list) {
      if (!entry.is_string()) {
        return "The proxy field bypass must be a list of strings";
      }
      spec.bypass.push_back(entry.GetString());
    }
  }

  std::string error = CheckProxy(spec);
  if (!error.empty()) {
    return error;
  }
  *out = std::move(spec);
  return std::string();
}

StoredProxy CorruptProxy() {
  StoredProxy stored;
  stored.state = StoredProxy::State::kCorrupt;
  return stored;
}

}  // namespace

ProxySpec::ProxySpec() = default;
ProxySpec::ProxySpec(const ProxySpec&) = default;
ProxySpec& ProxySpec::operator=(const ProxySpec&) = default;
ProxySpec::ProxySpec(ProxySpec&&) = default;
ProxySpec& ProxySpec::operator=(ProxySpec&&) = default;
ProxySpec::~ProxySpec() = default;

bool ProxySpec::operator==(const ProxySpec& other) const {
  return scheme == other.scheme && host == other.host && port == other.port &&
         username == other.username && password == other.password &&
         bypass == other.bypass;
}

std::string ParseProxyString(std::string_view text, ProxySpec* out) {
  if (text.empty() || text.size() > kMaxProxyStringLength) {
    return "The proxy must be 1 to 1024 characters";
  }
  if (HasControlOrSpace(text)) {
    return "The proxy must not contain spaces or control characters";
  }
  size_t separator = text.find("://");
  if (separator == std::string_view::npos) {
    return "The proxy must start with scheme:// (http, https or socks5)";
  }
  ProxySpec spec;
  std::string scheme_error =
      ParseSchemeText(text.substr(0, separator), &spec.scheme);
  if (!scheme_error.empty()) {
    return scheme_error;
  }

  std::string_view rest = text.substr(separator + 3);
  // Checked before the user info, so an '@' in a path can't be taken for one.
  if (rest.find_first_of("/?#") != std::string_view::npos) {
    return "A proxy must not have a path, query or fragment (percent-encode "
           "/, ? and # in the user name or password)";
  }

  std::string_view authority = rest;
  if (size_t at = rest.rfind('@'); at != std::string_view::npos) {
    if (IsSocks(spec.scheme)) {
      return kSocksAuthError;
    }
    authority = rest.substr(at + 1);
    std::string_view user_info = rest.substr(0, at);
    size_t colon = user_info.find(':');
    spec.username =
        base::UnescapeBinaryURLComponent(user_info.substr(0, colon));
    if (colon != std::string_view::npos) {
      spec.password =
          base::UnescapeBinaryURLComponent(user_info.substr(colon + 1));
    }
  }

  std::string_view host;
  std::string_view port_text;
  if (!authority.empty() && authority.front() == '[') {
    size_t close = authority.find(']');
    if (close == std::string_view::npos ||
        close + 1 >= authority.size() || authority[close + 1] != ':') {
      return "The proxy needs a host and a port (host:port)";
    }
    host = authority.substr(1, close - 1);
    port_text = authority.substr(close + 2);
  } else {
    size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      return "The proxy needs a host and a port (host:port)";
    }
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
    if (host.find(':') != std::string_view::npos) {
      return "IPv6 addresses must be in brackets, e.g. [::1]";
    }
  }

  constexpr char kPortError[] = "The port must be a number from 1 to 65535";
  if (port_text.empty() || port_text.size() > 5) {
    return kPortError;
  }
  int port = 0;
  for (char c : port_text) {
    if (c < '0' || c > '9') {
      return kPortError;
    }
    port = port * 10 + (c - '0');
  }
  if (port < 1 || port > 65535) {
    return kPortError;
  }

  std::optional<std::string> canonical =
      CanonicalHost(spec.scheme, host, port);
  if (!canonical) {
    return "The proxy host is not valid";
  }
  spec.host = *canonical;
  spec.port = port;

  std::string error = CheckProxy(spec);
  if (!error.empty()) {
    return error;
  }
  *out = std::move(spec);
  return std::string();
}

std::string ParseProxyValue(const base::Value& value, ProxySpec* out) {
  if (value.is_string()) {
    return ParseProxyString(value.GetString(), out);
  }
  if (const base::DictValue* dict = value.GetIfDict()) {
    return ParseProxyDict(*dict, out);
  }
  return "The proxy must be a string or an object";
}

std::string CheckProxy(const ProxySpec& spec) {
  if (spec.host.empty() || spec.host.size() > kMaxProxyHostLength ||
      HasControlOrSpace(spec.host)) {
    return "The proxy host must be 1 to 253 characters without spaces";
  }
  if (spec.port < 1 || spec.port > 65535) {
    return "The port must be between 1 and 65535";
  }
  std::optional<std::string> canonical =
      CanonicalHost(spec.scheme, spec.host, spec.port);
  if (!canonical || *canonical != spec.host) {
    return "The proxy host is not valid";
  }
  if (IsSocks(spec.scheme) &&
      (!spec.username.empty() || !spec.password.empty())) {
    return kSocksAuthError;
  }
  if (!spec.password.empty() && spec.username.empty()) {
    return "A password needs a user name";
  }
  if (spec.username.size() > kMaxProxyFieldLength ||
      spec.password.size() > kMaxProxyFieldLength) {
    return "The user name and password must be at most 256 bytes";
  }
  if (HasControlOrSpace(spec.username) || HasControlOrSpace(spec.password)) {
    return "The user name and password must not contain spaces or control "
           "characters";
  }
  if (!base::IsStringUTF8(spec.username) ||
      !base::IsStringUTF8(spec.password)) {
    return "The user name and password must be valid UTF-8";
  }
  if (spec.bypass.size() > kMaxProxyBypassEntries) {
    return "At most 64 bypass entries";
  }
  for (const std::string& entry : spec.bypass) {
    if (entry.empty() || entry.size() > kMaxProxyFieldLength ||
        HasControlOrSpace(entry)) {
      return "Each bypass entry must be 1 to 256 characters without spaces";
    }
    if (MatchesEveryHost(entry)) {
      return kMatchesEveryHostError;
    }
    net::ProxyHostMatchingRules rules;
    if (!rules.AddRuleFromString(entry)) {
      return "A bypass entry is not a valid rule";
    }
  }
  return std::string();
}

base::DictValue ProxyToRedactedValue(const ProxySpec& spec) {
  base::DictValue dict;
  dict.Set(kKeyScheme, SchemeName(spec.scheme));
  dict.Set(kKeyHost, spec.host);
  dict.Set(kKeyPort, spec.port);
  if (!spec.username.empty()) {
    dict.Set(kKeyUsername, spec.username);
  }
  dict.Set(kKeyHasPassword, !spec.password.empty());
  if (!spec.bypass.empty()) {
    dict.Set(kKeyBypass, BypassList(spec.bypass));
  }
  return dict;
}

std::string ProxyToDisplayString(const ProxySpec& spec) {
  std::string host = spec.host;
  if (host.find(':') != std::string::npos) {
    host = "[" + host + "]";
  }
  std::string display(SchemeName(spec.scheme));
  display += "://";
  if (!spec.username.empty()) {
    display += spec.username + "@";
  }
  display += host + ":" + std::to_string(spec.port);
  return display;
}

std::optional<base::DictValue> ProxyToPrefValue(
    const ProxySpec& spec,
    const os_crypt_async::Encryptor* encryptor) {
  if (!CheckProxy(spec).empty()) {
    return std::nullopt;
  }
  base::DictValue dict;
  dict.Set(kKeyScheme, SchemeName(spec.scheme));
  dict.Set(kKeyHost, spec.host);
  dict.Set(kKeyPort, spec.port);
  if (!spec.username.empty()) {
    dict.Set(kKeyUsername, spec.username);
  }
  if (!spec.bypass.empty()) {
    dict.Set(kKeyBypass, BypassList(spec.bypass));
  }
  if (!spec.password.empty()) {
    // Never fall back to storing the plaintext.
    if (!encryptor || !encryptor->IsEncryptionAvailable()) {
      return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> ciphertext =
        encryptor->EncryptString(spec.password);
    if (!ciphertext) {
      return std::nullopt;
    }
    dict.Set(kKeyPasswordEnc, base::Base64Encode(*ciphertext));
  }
  return std::move(dict);
}

StoredProxy ProxyFromPrefValue(const base::Value* value,
                               const os_crypt_async::Encryptor* encryptor) {
  if (!value || value->is_none()) {
    return StoredProxy();
  }
  const base::DictValue* stored = value->GetIfDict();
  // A plaintext password is never valid in storage.
  if (!stored || stored->contains(kKeyPassword)) {
    return CorruptProxy();
  }

  base::DictValue fields = stored->Clone();
  if (const base::Value* enc = fields.Find(kKeyPasswordEnc)) {
    if (!enc->is_string()) {
      return CorruptProxy();
    }
    std::optional<std::vector<uint8_t>> ciphertext =
        base::Base64Decode(enc->GetString());
    if (!ciphertext || !encryptor) {
      return CorruptProxy();
    }
    std::optional<std::string> password = encryptor->DecryptData(*ciphertext);
    if (!password) {
      return CorruptProxy();
    }
    fields.Remove(kKeyPasswordEnc);
    if (!password->empty()) {
      fields.Set(kKeyPassword, *password);
    }
  }

  ProxySpec spec;
  if (!ParseProxyDict(fields, &spec).empty()) {
    return CorruptProxy();
  }
  StoredProxy result;
  result.state = StoredProxy::State::kProxy;
  result.spec = std::move(spec);
  return result;
}

net::ProxyConfigWithAnnotation ProxyToNetConfig(const ProxySpec& spec) {
  if (spec.port < 1 || spec.port > 65535) {
    return FailClosedNetConfig();
  }
  net::ProxyServer server = net::ProxyServer::FromSchemeHostAndPort(
      NetScheme(spec.scheme), spec.host, static_cast<uint16_t>(spec.port));
  if (!server.is_valid()) {
    return FailClosedNetConfig();
  }

  net::ProxyConfig config;
  net::ProxyConfig::ProxyRules& rules = config.proxy_rules();
  // A single proxy and no "direct" entry: when it fails, requests fail.
  rules.type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST;
  rules.single_proxies.SetSingleProxyServer(server);
  // Each entry was checked by CheckProxy; a rule that doesn't parse here
  // would only be ignored, so it can't turn into a direct connection.
  // Chromium adds its own implicit bypass for localhost, loopback and
  // link-local destinations (traffic to this machine). That applies here too;
  // it is accepted behavior. "<-loopback>" is not special-cased: it is allowed
  // and removes the implicit bypass.
  for (const std::string& entry : spec.bypass) {
    rules.bypass_rules.AddRuleFromString(entry);
  }
  return net::ProxyConfigWithAnnotation(config, kTrafficAnnotation);
}

net::ProxyConfigWithAnnotation FailClosedNetConfig() {
  // Nothing listens on 127.0.0.1:1, so every connection fails
  // (ERR_PROXY_CONNECTION_FAILED) and nothing goes direct.
  net::ProxyConfig config;
  net::ProxyConfig::ProxyRules& rules = config.proxy_rules();
  rules.type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST;
  rules.single_proxies.SetSingleProxyServer(
      net::ProxyServer::FromSchemeHostAndPort(net::ProxyServer::SCHEME_HTTP,
                                              "127.0.0.1", "1"));
  return net::ProxyConfigWithAnnotation(config, kTrafficAnnotation);
}

bool ProxyMatchesChallenger(const ProxySpec& spec,
                            const net::AuthChallengeInfo& auth_info) {
  if (!auth_info.is_proxy) {
    return false;
  }
  // The challenger's host gets the same canonicalization as the stored host
  // (CanonicalHost), so text forms (IPv6, IPv4, IDN, case) compare equal. A
  // host that doesn't canonicalize never matches.
  std::optional<std::string> challenger_host =
      CanonicalHost(ProxySpec::Scheme::kHttp, auth_info.challenger.host(),
                    auth_info.challenger.port());
  if (!challenger_host) {
    return false;
  }
  // "host." and "host" name the same proxy: strip one trailing dot from each.
  auto strip_dot = [](std::string_view text) {
    if (!text.empty() && text.back() == '.') {
      text.remove_suffix(1);
    }
    return text;
  };
  return base::EqualsCaseInsensitiveASCII(strip_dot(*challenger_host),
                                          strip_dot(spec.host)) &&
         static_cast<int>(auth_info.challenger.port()) == spec.port;
}

}  // namespace shuttle

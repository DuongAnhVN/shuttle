# Feature ledger

What the fork **adds or changes in behavior** on top of Chromium (removals: `REMOVAL-LOG.md`). One row per
feature; keep it current when a fork commit adds, changes or drops a feature. Columns:

- **#**: fork commit number(s) (`REMOVAL-LOG.md` "Fork commits"); **SHA**: commit on `main` that landed it.
- **Upstream touch**: upstream files modified when it landed (excl. `tools/shuttle/`), top areas. These are the
  merge-conflict surface on every upstream merge.
- **Verified by**: the smoke probe (`scripts/smoke-all.sh`) that fails if the feature breaks; `manual` = no probe yet.

Own code lives in `chrome/browser/shuttle/` (MCP server, sessions, agents, page tools, network mode) and
`chrome/browser/resources/agents/` + its WebUI controller (`chrome://agents`).

| # | Feature | SHA | Upstream touch | Verified by | Docs |
| --- | --- | --- | --- | --- | --- |
| 1 | No first-run / default-browser prompt | 452754aa95 | 1: chrome_main_delegate.cc | smoke-cdp (no dialog) | |
| 16 | Always-on CDP: no approval dialog, no automation infobar, `navigator.webdriver` false, policy removed | 48370d6639 | 17: chrome/browser/devtools 8, ui 3, blink 1 | check-automation (opt-in), smoke-cdp | TOOLING |
| 20 | In-browser MCP server `POST /mcp` (agents = storage partitions, sessions, leased tabs); popups keep the opener's partition | 007d52a391 | 12 (+12 new): devtools 3, content devtools 4 | smoke-mcp phase1/2 | MCP.md |
| 21 | Agent identity: name, color, avatar; one tab group per agent, two-way sync, adopt/transfer dragged tabs | a1b652b125 | 8 (own code at the time) | smoke-mcp | MCP.md |
| 22 | No startup window (first agent tab opens it); keep-alive without a window | 00b32091df | 2: devtools, chrome_browser_main.cc | smoke-cdp, close-run1/2 | |
| 23 | `chrome://agents`: agents, sessions, tabs (create/edit/delete, avatar, open/jump) | 707fa803f6 | 9 (+9 new): ui 2, ui/webui/resources 1 | smoke-agents-page | |
| 24 | Per-session CDP endpoints (only the session's tabs + agent storage), agent viewports, CSS-px capture | 58b6b928ab | 12 (+2): content/public/browser 2, devtools 2 | smoke-session-cdp, smoke-mcp | MCP.md |
| 25 | Tab groups are agents: no manual groups, hand/take back tabs, agent bubble, "agent" strings | 01fffd656f | 24: chrome/browser/ui 12, resources 3 | manual (UI) | |
| 26 | Page tools via the agent-browser sidecar (24 tools behind `/mcp`); avatar on the group chip | 9330681592 | 10 (+7): ui 3, resources 1 | manual (needs a signed build; open) | MCP.md |
| 27 | Rebrand to Shuttle (bundle id, internal ids `shuttle`, strings, logos, Linux names) | 8b80db862f | 526: chrome/app/resources 81, ui 33 (strings/assets; `scripts/rebrand.py` re-applies) | smoke-cdp, rebrand.py check | BRANDING.md |
| 28 | Mac release: signing + DMG pipeline, official-build links | 21603b7944 | 2 (+29): framework.order/.exports, installer/mac restored | release-mac.sh | TOOLING |
| 29 | `chrome://agents` is home (Dock reopen), Quick start card with the MCP URL | 3e5d8913b8 | 6: resources 4, app_controller_mac.mm | smoke-agents-page | |
| 32 | Network mode: share one Shuttle across machines (`--shuttle-listen`, access token) | 571c3a4668 | 8 (+2): content devtools 2 | smoke-network (opt-in) | README |
| 36 | Fix: worker use-after-free on session CDP clients | 84718de00b | 4: content devtools 2 | manual (repro open) | REMOVAL-LOG |
| 37 | ImprovedStartupBestEffortDelay on (no 3-min startup fence without a window) | 12988c4c14 | 1: chrome_features.cc | manual | REMOVAL-LOG |
| 38-39 | Linux menu launch opens / reuses `chrome://agents` (`--shuttle-open-agents`) | 963fabcc82, 76e495c1ef | 6 (+1): ui 3, chrome_browser_main.cc | manual (Linux) | LINUX.md |
| 40-42 | Linux/WSL desktop: person launch gets `chrome://agents`; WSL own frame + dark mode; keep running while a session is open | 46479f1e55, b966481b3c, 9586a23162 | 9: ui 3, ui/linux dark_mode 2 | manual (Linux) | LINUX.md |
| 44 | Empty binders for BadgeService / on-device speech (no renderer kill) | bfd55b20e4 | 2: browser_interface_binders.cc, blink 1 | manual (probe open) | REMOVAL-LOG |
| 45 | Agents can't steal focus (no activation on bringToFront/activateTarget) | 9976b54bf0 | 5 (own code) | smoke-focus | REMOVAL-LOG |
| 46 | Agents keep session cookies across restarts (persist + restore, default on, per-agent opt-out `persistSessionCookies`) | (pending) | 2: profile_network_context_service.cc, chrome/browser/net/BUILD.gn | smoke-mcp phase1/2 | MCP.md, REMOVAL-LOG |
| 47 | Build: V8 from github.com/shuttle-browser/v8 (v8.h without v8-version.h: a V8 patch bump no longer recompiles Blink) | (pending) | DEPS (v8 URL + revision), v8 gitlink | dry run after touching v8-version.h | REMOVAL-LOG |
| 50 | Agent tabs keep the agent's storage when they start blank or pass through `about:blank` (site-less SiteInstances stay in the fixed partition) | 328c36624a | 4: content/browser site_info, site_instance_impl | smoke-isolation | MCP.md, REMOVAL-LOG |
| 51 | Fix: agent group headers collapse/expand again (identity sync no longer re-expands them) | d20226e891 | 0 (own code: session_manager.cc) | manual (UI) | REMOVAL-LOG |
| 52 | Agent tabs never launch external apps (no "Open app?" prompt; aborted without an error page) | d41e90fc1f | 2: chrome_content_browser_client.cc, chrome/browser/BUILD.gn | manual (log, `--vmodule`) | MCP.md, REMOVAL-LOG |
| 53 | Pinned home tab: `chrome://agents` at index 0 of every window, closes only with its window; agent-opened windows close with their last agent tab | `3976dd7c3a` | 11: tab_strip_model.{h,cc}, browser_commands, browser_command_controller, browser_tab_strip_model_delegate, views tab_strip + common tab_drag_handler, tab_list_bridge, pinned_tab_codec, session_common_utils | smoke-home-tab | REMOVAL-LOG |
| 54 | Home tab is the only `chrome://agents` tab (singleton navigation, stays on it) and gets focus when the active tab's group collapses or closes | `16aff23008` | 3: browser_navigator.cc + navigator BUILD.gn, tab_strip_model.cc | smoke-home-tab (5-6); group focus manual | REMOVAL-LOG |
| 55 | CDP `Target.closeTarget` / `Page.close` on the home tab reply an error (no false success); Shuttle icon on `chrome://agents` | `5eab0d8236` | 5: devtools inspector_protocol_config.json, protocol target_handler + page_handler, chrome_web_ui_controller_factory.cc + webui BUILD.gn | smoke-home-tab (2, 5b, 7); icon seen by Toby | REMOVAL-LOG |
| 56 | Session lifetime: CDP-created tabs close with their connection; idle timeout (30 min default, `--shuttle-session-idle-timeout`), `update-session`, `open-session` warning | `1a8b1958bd` | 0: own code in `chrome/browser/shuttle/` only | smoke-tab-guards; sidecar restart checked by hand | REMOVAL-LOG |
| 59 | Throwaway agents: `create-agent { throwaway: true, idleMinutes? }`, in-memory storage, deleted after `idleMinutes` idle (default 15) with their tabs and save-text files, cap 50 (`--shuttle-max-throwaway-agents`), "Temporary" chip on `chrome://agents` | (pending) | 0: own code in `chrome/browser/shuttle/` and `resources/agents/`; `agent_partition.cc` (used by chrome_content_browser_client, browser_navigator) accepts in-memory `shuttle-tmp-` partitions | smoke-throwaway | REMOVAL-LOG, MCP.md |
| 61 | Per-agent proxy: each agent has its own proxy (http, https, socks5; socks4 and SOCKS with auth refused), password encrypted at rest, live change, HTTP 407 answered automatically, WebRTC leak guards for proxied agent tabs, fail-closed for an unreadable stored proxy; MCP `proxy` fields, `set-proxy-all`, `assign-proxies`, `test-proxy` | (pending) | 10: chrome_content_browser_client.cc (CreateLoginDelegate hook; comment at UpdateRendererPreferencesForWorker), ui/prefs/prefs_tab_helper.cc (2 ApplyAgentRendererPrefs calls), preloading/preloading_prefs.cc (net.network_prediction_options default disabled), net/profile_network_context_service.cc + _factory.cc (proxy hook, DependsOn), profiles/chrome_browser_main_extra_parts_profiles.cc (factory registration, OSCryptAsync pointer); GN: chrome/browser/BUILD.gn, net/BUILD.gn, profiles/BUILD.gn, ui/prefs/BUILD.gn; new `agent_proxy` source_set in chrome/browser/shuttle/BUILD.gn | smoke-proxy (WebRTC: SKIP when not decidable offline) | MCP.md, this section |

## Per-agent proxy (fork 61)

- **What it adds:** a proxy stored per agent (`shuttle.profiles` entry, key `proxy`; none = the system proxy as
  before). No default and no inheritance: `set-proxy-all` and `assign-proxies` write copies. Changes apply live: the
  agent's NetworkContext gets the new config, connections are closed and the host cache cleared, so the next request
  of an open tab uses it. Throwaway agents keep their proxy in memory only.
- **Upstream touch points:** `chrome/browser/net/profile_network_context_service.cc` (the config hook for agent
  partitions; `proxy_config_monitor_` stays for agents without a proxy), `chrome/browser/chrome_content_browser_client.cc`
  (login delegate answering proxy challenges for agent tabs; renderer prefs: `disable_non_proxied_udp` for WebRTC
  in tabs of a proxied agent), new `source_set("agent_proxy")` in `chrome/browser/shuttle/BUILD.gn`.
- **Fail closed:** a stored proxy that can't be read (corrupt, password not decryptable) makes the agent's traffic
  fail, never go direct.
- **Passwords:** encrypted with `os_crypt` before they reach the pref; never returned by MCP (`hasPassword` only), logs
  or errors. Caveat: on headless Linux without a keyring `os_crypt` uses a built-in key, so the stored password is
  obfuscated, not secret, against a reader of the profile.
- **Schemes:** `http`, `https`, `socks5` only. socks4 is refused ("socks4 is not supported (it resolves DNS locally); use
  socks5"): it resolves destination names locally, a leak. SOCKS with credentials is refused ("SOCKS proxy with
  authentication is not supported"). Refusals apply to every tool that takes a proxy and write nothing.
- **Bypass:** a bypass entry matching every host is refused ("A bypass entry that matches every host is not allowed").
- **Auth:** HTTP/HTTPS proxy challenges are answered from the stored credentials (no dialog). A wrong password is
  detected per request (`first_auth_attempt`); the rejection marks the agent failed and starts a 60 s backoff in which
  no credentials are sent. Changing the agent's proxy clears the backoff; `test-proxy` resets it at its start.
- **WebRTC and speculative loads:** for an agent with a proxy the WebRTC policy is `disable_non_proxied_udp`, the
  profile's per-URL `webrtc_ip_handling_urls` rules are cleared for those tabs, and prefetch/prerender/preconnect are
  disabled for agent tabs with a proxy.
- **Known gaps:** SOCKS auth and socks4 unsupported (no relay); the external MCP profile manager (server "shuttle" with
  `profileId`) does not pass `proxy`; no UI yet (`chrome://agents` proxy form is later work).

### Scope and known limits

- **Guarantee scope:** requests made through the agent's StoragePartition (pages, subresources, workers' fetches).
  Not covered: browser-level network contexts (component updater, metrics, Safe Browsing if present); speculative
  requests that use the default network context, unless disabled for proxied agent tabs; WebRTC in workers (worker
  renderer prefs cannot be attributed to an agent: known gap).
- **Speculative loading:** Shuttle defaults `net.network_prediction_options` to disabled for the whole browser (prefetch,
  prerender, preconnect, predictor). Reason: same-site prefetch uses the default network context, not the agent proxy.
  Residual: an explicitly stored value, an extension or an enterprise policy can re-enable it, and then same-site
  prefetch can bypass the agent proxy. Page hints `<link rel=preconnect|dns-prefetch>` and legacy
  `<link rel=prefetch|preload|modulepreload>` were checked: they run on the page's own (agent) network context, so they
  use the agent proxy.
- **Implicit bypass:** `localhost`, `*.localhost`, loopback, and link-local (169.254/16, fe80::/10) are never proxied
  (destinations on the machine itself; they don't reveal the external IP). Accepted upstream behavior.
- **In flight:** requests in flight when a proxy changes are ended by closing their connections; new requests use the
  new proxy.
- **Attribution:** an agent is recognized by its partition path (`Storage/ext/shuttle-<id>/def`). If upstream changes
  that layout, agents silently lose per-agent proxy handling; smoke-mcp phase 2 and smoke-proxy catch it.
- **Trust:** MCP is a trusted local control plane. `set-proxy-all`, `assign-proxies` and `test-proxy` act on any agent;
  the browser-wide network-mode token is the only authorization, as for every other tool. `test-proxy` adds no authority
  beyond driving a tab and still only reaches public http(s) targets (no loopback, localhost, private or link-local IP
  literals, no userinfo, no redirects).
- **Encryption:** `os_crypt` on headless Linux without a keyring uses a fixed built-in key: the stored password is
  obfuscated, not secret from anyone who can read the profile directory. With a keyring it is protected by it. A
  password proxy created while OS encryption is not ready is refused (tool error), never stored in plaintext; at
  startup, before encryption is ready, such agents run fail-closed until it is.
- **Probe:** `scripts/smoke-proxy.mjs <port> <udd>` (own launch in `smoke-all.sh`): local origin plus HTTP, auth-HTTP and
  SOCKS5 proxies; checks routing per agent, live change, refusals, `set-proxy-all`, `assign-proxies`, 407 handling,
  throwaway, password absence (MCP output and Preferences), `test-proxy` shapes and WebRTC. The fail-closed case is a
  SKIP here (needs a tampered profile).

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
| 56 | Session lifetime: CDP-created tabs close with their connection; idle timeout (off by default since 2026-11-24, was 30 min; opt-in via `--shuttle-session-idle-timeout`), `update-session`, `open-session` warning | `1a8b1958bd` | 0: own code in `chrome/browser/shuttle/` only | smoke-tab-guards; sidecar restart checked by hand | REMOVAL-LOG |
| 59 | Throwaway agents: `create-agent { throwaway: true, idleMinutes? }`, in-memory storage, deleted after `idleMinutes` idle (default 15) with their tabs and save-text files, cap 50 (`--shuttle-max-throwaway-agents`), "Temporary" chip on `chrome://agents` | (pending) | 0: own code in `chrome/browser/shuttle/` and `resources/agents/`; `agent_partition.cc` (used by chrome_content_browser_client, browser_navigator) accepts in-memory `shuttle-tmp-` partitions | smoke-throwaway | REMOVAL-LOG, MCP.md |
| 61 | Per-agent proxy: each agent has its own proxy (http, https, socks5; socks4 and SOCKS with auth refused), password encrypted at rest, live change, HTTP 407 answered automatically, WebRTC leak guards for proxied agent tabs, fail-closed for an unreadable stored proxy; MCP `proxy` fields, `set-proxy-all`, `assign-proxies`, `test-proxy` | (pending) | 10: chrome_content_browser_client.cc (CreateLoginDelegate hook; comment at UpdateRendererPreferencesForWorker), ui/prefs/prefs_tab_helper.cc (2 ApplyAgentRendererPrefs calls), preloading/preloading_prefs.cc (net.network_prediction_options default disabled), net/profile_network_context_service.cc + _factory.cc (proxy hook, DependsOn), profiles/chrome_browser_main_extra_parts_profiles.cc (factory registration, OSCryptAsync pointer); GN: chrome/browser/BUILD.gn, net/BUILD.gn, profiles/BUILD.gn, ui/prefs/BUILD.gn; new `agent_proxy` source_set in chrome/browser/shuttle/BUILD.gn | smoke-proxy (WebRTC: SKIP when not decidable offline) | MCP.md, this section |
| 62 (steps 1-2) | Native fingerprint persona, **profile-level**: ONE versioned, coherence-checked persona per Chromium profile (os, browser, user agent, client hints, locale, timezone, cores/memory, screen), shared by every tab, agent and storage partition, stored in the profile pref `shuttle.fingerprint_persona` (not in `shuttle.profiles`); MCP `set-fingerprint` / `get-fingerprint` (never the seed; `{unreadable: true}` for a stored value that can't be read); `create-agent` / `update-agent` refuse `fingerprint`. **Applied at runtime (step 2), these fields only:** user agent string, Client Hints (`Sec-CH-UA*` headers and `navigator.userAgentData`), `Accept-Language` / `navigator.language(s)`, `navigator.platform`, `hardwareConcurrency`, `deviceMemory`, `screen.width/height/availWidth/availHeight/colorDepth/pixelDepth`, `window.devicePixelRatio`; from each tab's first navigation, no JS injection, no CDP, no per-agent difference. **Phase 1 (timezone/locale, not compiled or run yet):** `timezone` and `locale` are applied as the renderer process' ICU default time zone/locale with V8 notified (`Date`, `getTimezoneOffset`, `Intl.*`, `toLocale*String`; open tabs at once; pages and dedicated workers of the profile's renderer processes; shared/service workers not guaranteed; ICU defaults are process-global, so profile separation rests on content never sharing a renderer process between profiles (source-read, untested) and on the browser not sending the two values in `--single-process` (`EnforceProcessScopedIntl`); blink: `renderer_preferences` `timezone_override`/`locale_override`, `Page::SetRendererPreferences`, `TimeZoneController::SetPersonaTimeZone`, `LocaleController::SetPersonaLocale`; chrome: `IsKnownTimezoneId` in `set-fingerprint`). **Not applied (stored and returned only):** the internal `seed`, and everything the schema has no field for (canvas, audio, fonts, media/codecs, WebRTC SDP/stats; WebRTC IP handling is only the native proxy policy). Workers are not supported (best-effort preference hook, unverified; their user agent and client hints are not guaranteed). Persona changes reach every open tab's next navigation, not the page already shown; Accept-Language is live; the NetworkContext user agent only changes when a context is created again (no live setter, no context recreation) | (pending) | 28: blink `renderer_preferences.{h,mojom}` + `renderer_preferences_mojom_traits.{h,cc}` (11 optional persona fields, default = stock: 7 navigator/screen, 2 WebGL unmasked strings, time zone, locale), `navigator_base.{h,cc}`, `navigator_device_memory.h`, `screen.{h,cc}`, `local_dom_window.cc`, `wheel_event.cc` (reads them), `page.{h,cc}`, `timezone_controller.{h,cc}`, `locale_controller.{h,cc}` (time zone/locale), `webgl_rendering_context_base.cc` (unmasked vendor/renderer), `worker_or_worklet_global_scope.cc` (null-safe `GetRendererPreferences`); chrome_content_browser_client.cc (persona fields in `UpdateRendererPreferencesForWorker`); net/profile_network_context_service.{h,cc} + _factory.cc (profile-wide Accept-Language / user agent of the NetworkContexts, live Accept-Language update, DependsOn), profiles/chrome_browser_main_extra_parts_profiles.cc (factory registration); GN: chrome/browser/BUILD.gn, net/BUILD.gn, profiles/BUILD.gn, ui/prefs/BUILD.gn; chrome/browser/shuttle/BUILD.gn: new `pref_names`, `fingerprint_persona`, `agent_persona`, `persona_tab_prefs`, `webrtc_policy`, `agent_proxy_auth` source_sets (`proxy_auth` moved out of `agent_proxy`, so the network stack's `agent_persona` / `agent_proxy` pull no WebContents code), ui/prefs/BUILD.gn; no `content/` change | smoke-fingerprint-persona (also corrupt mode), smoke-fingerprint-runtime (SKIP without a browser/persona tools) | FINGERPRINT.md, MCP.md |
| 63 (lane B1, **not yet compiled**) | Isolated Chromium Profiles, core half, opt-in (`--shuttle-isolated-profiles`, off by default): `BrowserProfileRegistry` (versioned `ShuttleBrowserProfiles.json` in the user-data-dir root, atomic replace, state machine stopped/loading/initializing/ready/draining/failed/deleted, lifecycle epoch persisted before load/drain/delete, `requestId` idempotency + `request_conflict`, deletion tombstones, entries recover `stopped` at restart); `BrowserProfileRouter` (fixed startup Profile captured once, `Resolve(key)` with `invalid_profile_key`/`profile_not_found`/`profile_not_ready`, create/list/load/stop/delete through `ProfileManager::CreateProfileAsync` and `DeleteProfileHelper`, drain through a `BrowserProfileDrainDelegate` that B2 implements, `drain_timeout` keeps `draining`, concurrent loads share one init); browser-wide `SessionOwnerIndex`/`TargetOwnerIndex`; `ShuttleManagerAdminToken` (256-bit, 0600, constant-time verify); `BuildCapabilities()`; managed-profile bootstrap in `ProfileImpl::OnPrefsLoaded` (persona/workspace/proxy provisioned through PrefService before any keyed service, corrupt or missing persona fails closed, `ProfilePersonaService` has no stock fallback for managed profiles). Not in this row: MCP/CDP routing, tool dispatch, session-id change (lane B2). | uncommitted (branch `sh/native-chromium-fingerprint`) | 5 upstream files: profiles/profile_impl.{h,cc} (the hook), profiles/chrome_browser_main_extra_parts_profiles.{h,cc}, profiles/BUILD.gn, chrome/test/BUILD.gn (test group only); own code in chrome/browser/shuttle/ (6 new source_sets, persona service fail-closed) | none yet (native tests written, never built: `browser_profile_registry_unittest.cc`, `browser_profile_lifecycle_browsertest.cc`) | PROFILES.md |
| 64 (lane B2, **not yet compiled**) | Isolated Chromium Profiles, routing half (needs `--shuttle-isolated-profiles`): `X-Shuttle-Admin-Token` request context through `DevToolsManagerDelegate`; explicit `ConnectionDecision` (admin-allowed / scoped-filter / denied 404-403) for browser, session and page sockets; browser-global `/json/*` admin-only; per-session CDP grants (`?grant=`); scoped `SessionCdpFilter` (re-authorizes every message, own targets/context only); MCP `_meta["shuttle/profileId"]` routing and the six browser-level tools; UUIDv4 session ids with owner tuples; `ShuttleService::Drain`; scoped `set-fingerprint` rules. Off: legacy behaviour unchanged. | uncommitted (branch `sh/native-chromium-fingerprint`) | 3 upstream files: content/browser/devtools/devtools_http_handler.cc, content/public/browser/devtools_manager_delegate.{h,cc}, chrome/browser/devtools/chrome_devtools_manager_delegate.{h,cc}; own code in chrome/browser/shuttle/ (`scope_auth`, `scoped_routing`, session manager/filter/service/tools) | `scope_auth_unittest.cc` (never built); smoke-browser-profiles, smoke-profile-routing, smoke-profile-cdp, smoke-profile-network, smoke-profile-cookies (opt-in, never run) | PROFILES.md, MCP.md |
| 66 (**not yet compiled**) | CAPTCHA native detection and solve, **no DevTools/CDP in the path**: strict rule snapshot (`set/get-captcha-rules`, contract `captcha-contract.md` §1: puzzle-slide only, selectors are data, ≤32 rules ≤64 KB, atomic replace, monotonic revision); per-tab `WebContentsObserver` detector (URL gate + in-frame readiness through `RenderFrameHost::ExecuteJavaScriptInIsolatedWorld` in an embedder-range isolated world, fixed script template + JSON string literals; debounced event passes + 500 ms/30 s then 2 s cadence); browser-wide registry (challenges `ch_<hex>` with `stateRevision` and an internal digest that never leaves, event ring of 256 with cursor/dropped and long-poll waiters, tab lock); collector (isolated-world capture + `CopyFromSurface` fallback, PNG) and executor (trusted `ForwardMouseEvent` drag, 14 moves, 120 ms hold, always released; marker verify poll); MCP tools `set/get-captcha-rules`, `probe-captcha`, `wait-captcha-events`, `get/solve/cancel-captcha-challenge`; mutating page tools and session CDP `Input.*`/`Page.navigate|reload|goBack|goForward` answer `captcha-in-progress` while a solve runs; rules live in memory only (no pref, no disk) | (pending) | 0 upstream files; own code `chrome/browser/shuttle/captcha_{rules,detector,registry,tools,collector,executor}.{h,cc}` + additive wiring in `mcp_tools.cc` (7 tools in `kTools`), `page_tools.cc` and `session_cdp_filter.cc` (tab-lock refusals), `shuttle_service.cc` (observer attach hook), `chrome/browser/shuttle/BUILD.gn` (new `captcha_rules` source_set + unit test) | `captcha_rules_unittest.cc` (never built); smoke-captcha-detect (needs a built browser, never run yet) | MCP.md, this section |

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

## Isolated Chromium Profiles, core lane B1 (fork 63)

**Not yet compiled.** Written on a host with no Chromium build. Contract: `PROFILES.md` (frozen); what exists and
what does not: `PROFILES.md` "Implementation status".

- **What it adds:** the core half of "one regular Chromium Profile per Manager profile" behind
  `--shuttle-isolated-profiles` (off by default; without it the browser is unchanged and none of the new files is
  created). Registry + router + owner indexes + admin token + capabilities + bootstrap, all in
  `chrome/browser/shuttle/`. Browser-level tools, MCP/CDP scope routing, the session-id change and the drain
  implementation are lane B2 and are NOT here.
- **Upstream touch points:** `profiles/profile_impl.cc` (`OnPrefsLoaded` calls `shuttle::PrepareManagedProfile`
  before `OnLocaleReady`; the old remainder moved verbatim into `OnManagedProfileBootstrapped`),
  `profiles/chrome_browser_main_extra_parts_profiles.cc` (router `Initialize` in `PreProfileInit`, startup Profile
  capture in `PostProfileInit`, `Shutdown` in `PostMainMessageLoopRun`), three GN files.
- **Fail closed:** a managed Profile (marker pref `shuttle.managed_profile`) loads only when the router started the
  load; a corrupt/missing persona, unreadable proxy, unknown registry entry or a registry that cannot be read keeps it
  from loading. An unreadable manifest, a manifest of another version, or an unusable admin token disables the whole
  capability and the files are never overwritten.
- **Known limits:** the bootstrap-ordering audit is by source reading only (see `PROFILES.md`); synchronous creation of
  a managed Profile (`ProfileManager::GetProfile`, `--profile-directory`) cannot wait and is not supported;
  `kRemoteDebugging` is the keep-alive origin reused for loaded managed Profiles (no new enum value); directories of a
  deleted Profile are removed by Chromium at the next start when it was loaded.

## Isolated Chromium Profiles, core lane B2 (fork 64)

**Not yet compiled.** Routing, authorization and ownership on top of lane B1. Contract `PROFILES.md`; usage `MCP.md`
("Isolated Chromium Profiles"). Everything is inert without `--shuttle-isolated-profiles`.

- **Upstream touch points:** `content/browser/devtools/devtools_http_handler.cc` (request context from
  `X-Shuttle-Admin-Token` and `?grant=`, explicit decision for every socket, `/json/*` guard, MCP passes the context),
  `content/public/browser/devtools_manager_delegate.{h,cc}` (`RequestContext`, `ConnectionDecision`, virtuals
  `VerifyAdminToken`, `AllowBrowserGlobalHttp`, `DecideConnection`, `CreateConnectionFilter(sid, tid, ctx)`,
  `HandleMcpRequest(..., ctx, cb)`, `ConnectionFilter::SetCloseCallback`; defaults keep the legacy behaviour),
  `chrome/browser/devtools/chrome_devtools_manager_delegate.{h,cc}` (overrides; startup Profile instead of last-used when a
  router exists; installs scoped routing).
- **Own code:** `scope_auth.{h,cc}` (selector parser, decision table and per-session grants, pure and unit tested),
  `scoped_routing.{h,cc}` (the scoped decision and the drain delegate), `ShuttleService::HandleHttpMcpRequest/Drain`,
  `SessionManager` (UUIDv4 ids, owner tuples, grants, target index), `SessionCdpFilter` (scoped mode), `mcp_tools` (browser
  tools, `GetLeasedTab` ownership, grant endpoints, scoped fingerprint rules), sidecar endpoint carries the grant.
- **B3 (same row, not compiled):** `BrowserProfileRouter::ReplacePersona` + registry `ReplacePersonaIntent`
  (`personaReplacementPending`, epoch bump in one write), bootstrap applies the pending intent at the next load; scoped
  `set-fingerprint` on a `stopped` managed Profile (the only tool that reaches it; ready is `profile_busy`); discovery
  page `/` admin-gated; `GetBrowserContext` never last-used with a router. Unit tests in
  `browser_profile_registry_unittest.cc`.
- **Known limits:** windows of a managed Profile that are not leased are not closed on drain;
  `scopedRouting` is false when the router does not exist yet at DevTools delegate creation; a browser-level `Storage`/`Browser`
  command that Playwright `connectOverCDP` needs may be refused by the scoped filter (unverified).

## CAPTCHA native detection (fork 66)

**Not yet compiled.** Wire format: `captcha-contract.md` (shared with shuttle-manager, authoritative); tool surface:
`MCP.md` ("CAPTCHA native detection"). The Manager owns rules, providers and policy; the browser holds only the last
applied snapshot in memory. Nothing in the detection or solve path uses DevTools/CDP, and no Mojo interface is added:
readiness scripts run in an embedder-range isolated world (the collector's one), the drag is a trusted `ForwardMouseEvent` stream, and the
re-check loop is a timer (a MutationObserver could not reach the browser, and the isolated-world JS API does not
await promises - measured in this tree, documented in `captcha_detector.h`).

- **Own code:** `chrome/browser/shuttle/captcha_rules.{h,cc}` (strict parser, host/path matching, JSON-literal
  escaping, the contract's slide-plan maths; pure, unit tested), `captcha_detector.{h,cc}` (per-tab
  `WebContentsObserver`), `captcha_registry.{h,cc}` (browser-wide snapshot, challenges + digest, event ring, waiters,
  tab lock, solve orchestration), `captcha_tools.{h,cc}` (the seven MCP tools), `captcha_collector.{h,cc}` and
  `captcha_executor.{h,cc}` (capture and trusted drag, lane N-B).
- **Upstream touch points:** none. The additive wiring is inside fork-owned files (`mcp_tools.cc`, `page_tools.cc`,
  `session_cdp_filter.cc`, `shuttle_service.cc`, `BUILD.gn`); the isolated-world id is
  `content::ISOLATED_WORLD_ID_CONTENT_END` (the public API CHECKs `world_id <= ISOLATED_WORLD_ID_MAX`, so the Chrome
  internal ids are not usable — measured in `render_frame_host_impl.cc`).
- **Fail closed:** an invalid snapshot never replaces the applied one; a solve sends no input unless the revision and
  the fresh digest match; the mouse button is released on every exit path (finish, cancel, deadline, frame or tab
  death); the tab lock is dropped with the tab.
- Equal or lower rule revisions with different content are stale. `wait-captcha-events` includes a stable per-process
  `bootId`; `dropped` counts the gap for the requested cursor. Gone/not-found challenge reads are tool errors.
- Leased tabs are observed at lease creation and forgotten when released. In multi-profile mode the browser-wide
  CAPTCHA tools are admin-only. While locked, state-load and acting `find` actions are refused; `verifying` is reported
  as `executing`.
- **Known limits:** only `puzzle-slide` exists; detection latency is one poll tick in the worst case (≤ 500 ms in the
  30 s fast window, ≤ 2 s after); the internal digest makes re-detection cheap but a same-picture re-mount is silent
  by design; no rule or challenge persists across a restart (the Manager re-syncs).
- Probe: `smoke-captcha-detect.mjs` (local fixtures on two ports; also exercises the tab lock over a session CDP
  socket). Written with the lane, never run yet (the tree is not compiled on the writing machine).

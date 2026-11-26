# MCP server and agent model

Shuttle's MCP server runs **inside the browser** (C++, `chrome/browser/shuttle/`, so upstream merges rarely touch it).
MCP manages agents, sessions and tabs and offers page tools; CDP stays the transport underneath and an escape hatch.
Many agents share one Shuttle instance. Smoke tests: `scripts/smoke-mcp.mjs`, `smoke-session-cdp.mjs`,
`smoke-agents-page.mjs`, `smoke-network.mjs`.

## Model

| Term | Meaning | Chromium mechanism |
| --- | --- | --- |
| **Agent** | Identity (id = kebab slug `^[a-z0-9]+(-[a-z0-9]+)*$`, ≤64; name, color, optional avatar) + its own persistent storage (cookies, logins, local storage). Created/updated/deleted over MCP or `chrome://agents`. | One persistent, non-default `StoragePartition` per agent (`StoragePartitionConfig::Create(profile, "shuttle-<id>", "", false)`) inside the single Chrome profile; list persisted in the pref `shuttle.profiles` (partition dirs are hashed and can't be listed back). |
| **Throwaway agent** | `create-agent { throwaway: true, idleMinutes? }`: generated id `tmp-<12 hex>`, name/color as usual, no avatar, no `persistSessionCookies`. Deleted with its tabs and save-text files after `idleMinutes` (1-1440, default 15) without activity (MCP calls, CDP messages, a running call, a person looking at a tab; with no session the clock runs from the last session's end). At most 50 live (`--shuttle-max-throwaway-agents`, 0 = off). `state-save`/`state-load` refused. Shown on `chrome://agents` with a "Temporary" chip; `list-agents` adds `throwaway`, `idleMinutes`, `expiresAt`. | In-memory `StoragePartition` (`StoragePartitionConfig::Create(profile, "shuttle-tmp-<id>", "", true)`), registry entry in memory only: nothing survives a restart. Deletion empties the partition (`ClearData`; Chromium never unloads it). Chromium still writes the partition's GPU shader caches to `Storage/ext/shuttle-tmp-<id>/def/`; they go with the agent and at startup. Ids are never reused in a run (a reused id would find the old in-memory partition). Plan: `plans/THROWAWAY-AGENTS.md`. |
| **Session** | One MCP client working as one agent. | In memory only (agents re-open after a restart; session restore isn't relied on). |
| **Tab** | Leased to exactly one session. | `WebContents` created with `SiteInstance::CreateForFixedStoragePartition`; lease data in `AgentTabLease` (`WebContentsUserData`). |
| **Tab group** | The agent's visible identity: one group per agent (all its sessions' tabs), label = name, color = agent color, avatar on the chip. | Upstream tab groups, user-facing strings renamed to "agent"; C++ `TabGroup` ids unchanged for cheap merges. |

- One Chrome profile only; profile-wide state (content settings, history, downloads, extensions) is shared. Accepted.
- Several sessions may share an agent: shared storage (last writer wins), exclusive tab leases.
- No incognito anywhere, CDP included: `Target.createBrowserContext` fails by design.
- Groups exist only as agents: manual group creation (menus, shortcuts, history "open in group") is a no-op; restored
  tabs come back ungrouped. Renaming/recoloring a group writes back to the agent.
- Tab menu "Hand tab to agent ▸" / dragging a tab into an agent's group = adopt (reopened in the agent's storage,
  leased to its latest session); "Take back from agent" reopens it in the user's storage. Dragging out leaves it
  leased but ungrouped (known gap).
- No startup window: Shuttle starts windowless unless it has URLs; the first agent tab or the user creates a window.
  Linux: a person starting it (tty/desktop) gets `chrome://agents`; it keeps running windowless while a session is open.

## Endpoint and protocol

- Same port as CDP (9222, or a free port; see `DevToolsActivePort`), path `/mcp`. Hook in
  `content/browser/devtools/devtools_http_handler.cc` → `DevToolsManagerDelegate::HandleMcpRequest` →
  `ChromeDevToolsManagerDelegate` → `shuttle::ShuttleService` (KeyedService on the default profile).
- JSON-RPC 2.0 over POST, plain `application/json` responses (net::HttpServer sends whole responses: no SSE), stateless
  (no `Mcp-Session-Id`). GET → 405; notifications → 202. Requests with an `Origin` not in `--remote-allow-origins` are
  rejected (web pages can't call `/mcp`); the Host check is upstream's `RequestIsSafeToServe`.
- Default: localhost only, no auth (same trust as the CDP port). Network mode (`--shuttle-listen`, `network_access.cc`)
  requires a token for remote clients; plain HTTP.
- Service pieces: `mcp_tools` (tool table + schemas), `profile_registry` (agents ↔ partitions), `session_manager`
  (sessions, groups, leases, capture handles, viewports), `session_cdp_filter`, `page_tools` + `agent_browser_sidecar`,
  `avatar_cache`.

## Agent tabs

- `allocate-tab`: background tab in the profile's normal window (created if none), in the agent's group.
- **Unthrottled while leased:** the lease holds `IncrementCapturerCount(stay_hidden=false, stay_awake=true)`, so
  background agent tabs report `visible`, run rAF at full rate and don't throttle timers.
- **Viewport:** applied as an emulation override owned by the lease (`Emulation.setDeviceMetricsOverride` through an
  internal DevTools client), so it survives activation and works below Chrome's 500 px window minimum.
- **Opened tabs stay in the partition:** popups, `noopener`/`target=_blank` and Cmd/middle-click tabs from a leased tab
  (upstream leaks the last two to the default partition; patched in `WebContentsImpl::CreateNewWindow` and
  `browser_navigator.cc`). They join the same session and group. CDP-triggered popups need `userGesture: true`.
- **Blank tabs stay in the partition:** a tab allocated without `url`, and `about:blank` along the way, stay in the
  agent's storage (upstream gives a site-less SiteInstance the default partition; patched in `SiteInstanceImpl`,
  fork commit 50). Probe: `scripts/smoke-isolation.mjs`.
- **No external apps:** links to external apps (`mailto:`, `atomicbi://`, ...) in agent tabs are dropped silently:
  no "Open app?" prompt (it blocked the tab's clicks), no error page (fork commit 52). Normal tabs keep Chrome's prompt.
- **Session cookies persist:** an agent's session cookies (no Expires/Max-Age) survive a browser restart, like
  Chrome's main partition with "Continue where you left off" (upstream drops them in every non-default partition).
  Per-agent opt-out: `persistSessionCookies: false` on `create-agent`/`update-agent` (`list-agents` shows it;
  toggle in the `chrome://agents` edit dialog), stored as `persist_session_cookies` in the agent's `shuttle.profiles`
  entry (missing = on). A partition's network context is configured once, so a change applies at the next start;
  opting out deletes nothing (the cookie store drops the agent's session cookies at its next load). Hook:
  `shuttle::ShouldPersistSessionCookies` (`agent_partition.cc`), called from `ProfileNetworkContextService`; it
  maps the partition path (`Storage/ext/shuttle-<id>/def`) back to the agent.
- **Cookie flush** (`FlushCookieStore`) on close-session, agent deletion and `OnClosingAllBrowsers`: cookies set right
  before `Browser.close` were otherwise lost in every partition.
- Deleting an agent closes its tabs, flushes, then `AsyncObliterateStoragePartition` (a still-loaded partition is
  wiped at the next launch).

## Per-session CDP endpoint

`open-session` returns `cdpEndpoint = ws://127.0.0.1:<port>/devtools/session/<sessionId>`: a browser-level DevTools
connection filtered to the session (content hook `DevToolsManagerDelegate::CreateConnectionFilter`;
`session_cdp_filter.cc`). Playwright `connectOverCDP(cdpEndpoint)` and `agent-browser --cdp` see only the agent's world:

- `Target.getTargets`/discovery/auto-attach: only the session's tabs; attach/close/activate on foreign targets → error.
- `Target.createTarget` → a leased tab; `Browser.close` → close-session (never quits the browser).
- Browser contexts/cookies (`Storage.*`) map to the agent's partition.
- Page endpoints (`/devtools/page/<id>`) can't reach other targets. The browser-wide endpoint stays open for tooling
  and DevTools; agents are told to use their session endpoint.
- Gotchas: internal commands sent from inside an agent-host event must be posted (re-entrant detach crashed
  `TargetHandler::AutoAttach`); a DevTools client's `GetTypeForMetrics()` must be DevTools/Extension/RemoteDebugger/Other;
  a client that auto-attached with `waitForDebuggerOnStart` holds new navigations until it disconnects (upstream).

## Session lifetime

- **Tabs a CDP connection creates close with it.** `Target.createTarget` / Playwright `newPage()` on a session
  endpoint makes a session tab that closes when that connection ends (`SessionCdpFilter` remembers them). Tabs from
  `allocate-tab`, tabs handed to the agent and popups stay with the session. Use `allocate-tab` for a tab that must
  outlive the connection; a new connection always lists the session's tabs (Playwright `pages()`).
- **Idle timeout:** a session closes (tabs, cookie flush, sidecar) after `idleTimeoutMinutes` without activity:
  default 0 = never (2026-11-24: sessions no longer time out unless asked; `--shuttle-session-idle-timeout=<minutes>`, 0 = off; `<n>s` for probes), per session on
  `open-session`, changed later with `update-session` (0 = never, e.g. before a long sign-in by a person).
  Activity = MCP calls naming the session or one of its tabs, client messages on its CDP connections, and a person
  switching to one of its tabs (or looking at it when the timeout hits; never in `--headless`, where windows are
  "active" with no one looking). An MCP tool call still running (e.g. a long `wait-for`) counts until it answers.
  A connected but silent client doesn't count, nor does one waiting on a single long CDP command: send something
  (any command) more often than the timeout, or set `idleTimeoutMinutes` 0.
  Sessions the browser opens for the user (`chrome://agents` "Open tab", a tab newly handed to an agent) have no
  timeout; an existing session keeps its own.
- **Automatic and session closes discard the tabs** (no `beforeunload` dialog, no unload handlers), so no tab outlives
  its session or connection. `release-tab` still closes ordinarily.
- **An expired session doesn't close its CDP sockets.** A client on the session endpoint sees its targets detach and
  be destroyed (if it subscribed); its socket stays open until it disconnects, `createTarget` then fails (unknown
  session), and a reconnect to the old endpoint is refused.
- **`open-session` warns** (`warning`, `openSessions`) when the agent already has open sessions; the new one still
  opens. `list-sessions` shows `lastActivityAt` and `idleTimeoutMinutes`.
- Probe: `scripts/smoke-tab-guards.mjs` (own launch in `smoke-all.sh`, idle timeout 8 s).

## Page tools (agent-browser sidecar)

[agent-browser](https://github.com/vercel-labs/agent-browser) (Rust, Apache-2.0, bundled unmodified with LICENSE +
NOTICE) supplies the page tools, proxied through `/mcp`.

- `tools/list` = the agent/session/tab tools + ~24 curated page tools (`snapshot click fill type press select check
  uncheck scroll wait-for eval get-text screenshot back forward reload console errors state-save state-load hover
  dblclick find open`), ~7.6 KB of schema instead of agent-browser's 63 KB `core` set. Kebab-case names; every page
  tool takes a required `targetId` (the session is derived from the tab). `wait-for` merges the five `wait_*` tools.
  Hidden: tabs, close, profile/auth/launch, `path`/`extraArgs`, downloads/uploads.
- Definitions: `config/agent-browser-tools.json` → `scripts/gen-page-tools.mjs` → `page_tools_data.inc` (C++ raw
  string; regenerate and commit together). Args are whitelisted against each tool's schema; unknown args are an error.
  Results drop agent-browser's `structuredContent`/`lifecycle` envelope (`click` → `{"clicked":"@e3"}`, no data → `ok`).
- `screenshot` (fork 60, issue #27): agent-browser captures in device px with no scale option, so the browser
  rescales. Default `scale: "css"` (divide by the tab's device scale factor; the viewport emulation keeps the native
  factor), optional `maxSize` (longer edge), default JPEG (quality 80, like `capture-tab`). When a rescale is needed it
  asks the sidecar for PNG, decodes it (Rust PNG decoder) on the thread pool, resizes (`RESIZE_BETTER`) and encodes
  once; otherwise it passes `format`/`quality` through untouched. The browser names the capture file itself (random
  `shot-*` in the session's `shots` dir, passed as `path`) and reads it fd-anchored (private dirs, no links, regular
  file; 48 MiB cap for a rescale, 10 MiB returned as is), then deletes it (best effort: a capture written after a timeout, or one skipped at
  shutdown, stays in the shots dir until a retired-session sweep exists): agent-browser leaves images over 10 MiB out
  of its reply but still reports success, so the reply's image is never used, and a success always carries an image.
  At most 4 screenshots run at once across sessions (else "Busy ... retry"); captures are read and rescaled on one
  sequence (one decode at a time). Probe: `smoke-page-tools.mjs` (own 2x launch in smoke-all).
- `PageToolManager` keeps one `Sidecar` per session (`agent-browser mcp --tools all`, env `AGENT_BROWSER_CDP=<session
  endpoint>`, constant `AGENT_BROWSER_SESSION=s`, `AGENT_BROWSER_NAMESPACE` unset, private per-session
  `AGENT_BROWSER_SOCKET_DIR=<tmp>/acab-<16 hex of SHA-256(session id)>`: the daemon socket `<dir>/s.sock` is the temp dir
  + 29 bytes and must stay <= 100 bytes (Mac `sun_path` 104); a longer `TMPDIR` (> 71 bytes) moves the dir to `/tmp`.
  `scripts/check-sidecar-socket-path.mjs` models and checks the lengths). Calls are
  serialized per sidecar; a `tab_switch {tab:<targetId>}` precedes calls for a different tab. Never set
  `AGENT_BROWSER_PIN_TAB` (a fresh daemon opens a stray tab).
- Each sidecar starts a daemon that outlives the MCP process: on session close send the `close` tool (3 s), then
  kill; `AGENT_BROWSER_IDLE_TIMEOUT_MS` is the fallback. Per-job timeout 60 s (wait + 15 s; 20 s cold start), then the
  process is killed and restarted on the next call. Cold first call ~0.5 s, later calls 25-50 ms.
- Binary: `$SHUTTLE_AGENT_BROWSER`, else `Contents/Helpers/agent-browser` (Mac) or next to the executable (Linux).
  `scripts/fetch-agent-browser.sh <Helpers-dir>` installs the pinned version; `release-mac.sh` signs it with the app.
- Later, data-driven: port the hot-path tools (snapshot, click, fill, press, wait-for, eval) to in-browser C++ with the
  same names, refs and output; the sidecar keeps the long tail.

## `chrome://agents`

Lit WebUI (`chrome/browser/resources/agents`, `chrome/browser/ui/webui/agents`) from cr elements and cr vars only
(Chromium Settings look, no custom brand colors). Lists agents with avatar, sessions and tabs (2 s refresh while
visible); create, edit name/color/avatar (file → center crop 256 px data: URL) and "Keep session cookies after
restart", delete (storage wiped), end session,
focus a tab, "Open tab" (foreground tab in the agent's storage, e.g. to sign in). Calls the MCP tools via
`chrome.send('callTool')` (allowlisted in `build_webui.gni` `legacy_chrome_send_users`).

## Per-agent proxy

Each agent can send its traffic through its own proxy (`docs/FEATURES.md` "Per-agent proxy"; probe `smoke-proxy.mjs`).
A proxy is a string `scheme://[user[:password]@]host:port` (`http`, `https`, `socks5`) or an object
`{scheme, host, port, username?, password?, bypass?: [string]}` (`bypass` in net::ProxyBypassRules syntax; an entry
matching every host is refused). `null` = no proxy (system proxy, as before). Refused: socks4 ("resolves DNS locally;
use socks5") and SOCKS with a username or password ("SOCKS proxy with authentication is not supported").

- `create-agent {…, proxy?}` (also with `throwaway: true`; the throwaway proxy lives in memory only).
- `update-agent {id, proxy?}`: a value sets it, `null` clears it. Applies live: the next request of an open tab uses it.
- `list-agents`: `proxy` is `null`, `{scheme, host, port, username?, hasPassword, bypass?}` or `{unreadable: true}`
  (a stored proxy that can't be read; the agent then fails closed, it never goes direct). The password is never returned.
- `set-proxy-all {proxy, agentIds?}`: copies one proxy into every persistent agent (or the listed ones); `proxy: null`
  clears. Returns `{updated: [agentId]}`. Each agent keeps its own copy; later edits don't touch the others.
- `assign-proxies {proxies: [proxy], agentIds?, reuse?}`: one proxy per agent, in order (default agents: persistent,
  by creation then id). Fewer proxies: the rest stay as they are and are listed in `unchanged`; `reuse: true` cycles
  through the list. More: the extras are listed in `unused`. Any bad entry fails the whole call with an error that names
  `proxies[i]`, and nothing is written. Returns `{assigned: [{agentId, proxy}], unchanged, unused}` (proxies redacted).
- `test-proxy {agentId, url?}`: loads `url` (default `https://api.ipify.org?format=json`) through the agent's own
  network context. Returns `{agentId, ok, latencyMs, status?, body?, proxy?, error?: {code, message, proxyAuthFailed?}}`.
  `proxyAuthFailed` is true after a 407 with the stored password refused. Targets: public http(s) only (loopback,
  localhost, private and link-local IP literals and userinfo URLs are refused; no redirects).
- HTTP/HTTPS proxy auth (407) is answered automatically with the stored credentials, no dialog. A wrong password is
  detected per request; the agent then sends no credentials for 60 s (changing its proxy or running `test-proxy`
  resets this).
- WebRTC: tabs of an agent with a proxy use `disable_non_proxied_udp`, their per-URL WebRTC rules are cleared, and
  prefetch/prerender/preconnect are disabled for them. Every tab of a profile whose own proxy preference names a
  proxy (command line, policy, extension, settings: fixed servers or PAC) gets the same WebRTC policy. No MCP field;
  native Chromium policy only. See `docs/WEBRTC.md` (TURN, UDP and worker limits).

Limits (`docs/FEATURES.md` "Scope and known limits"): covers requests through the agent's own storage partition, not
browser-level traffic or WebRTC in workers. `localhost` and loopback are never proxied (Chromium's implicit bypass). Speculative loading (prefetch, prerender,
preconnect) is disabled by default; an explicit setting or policy can re-enable same-site prefetch outside the proxy.
In-flight requests end when the proxy changes; new ones use the new proxy. The MCP is a trusted local control plane:
the network-mode token is the only authorization.

## Fingerprint persona

A Chromium **profile** has at most one fingerprint persona (`docs/FINGERPRINT.md`; probes
`smoke-fingerprint-persona.mjs`, `smoke-fingerprint-runtime.mjs`), shared by every tab, agent and storage partition of
the profile: it is **not** an agent property, agents have no `fingerprint` field and cannot differ. **Applied at runtime,
profile-wide, from each tab's next navigation:** user agent string, Client Hints (`Sec-CH-UA*`,
`navigator.userAgentData`), `Accept-Language` and `navigator.language(s)`, `navigator.platform`,
`navigator.hardwareConcurrency`, `navigator.deviceMemory`, `screen.*` size and color depth, `window.devicePixelRatio`.
**Time zone and locale** (phase 1, not compiled or run yet): `timezone` and `locale` are applied as the renderer
process' ICU default time zone and locale (V8 notified): `Date`, `getTimezoneOffset`, `Intl.DateTimeFormat`
(`resolvedOptions().timeZone/locale`), `Intl.NumberFormat` and the other `Intl` APIs, `toLocale*String`; they follow in
open tabs at once, with no navigation. ICU's default is process-wide (no per-frame isolation inside a process); other
profiles are kept out only because content never shares a renderer process between profiles (read in the source, not
tested here), and in `--single-process` the two values are not applied at all (see FINGERPRINT.md "Scope"). `timezone` must be a known IANA id
(`set-fingerprint` refuses an unknown one). Pages and dedicated workers of the profile's tab processes are covered;
shared and service workers in their own processes are not guaranteed. A DevTools `Emulation` timezone/locale override
still wins while it is set. **Coherence:** every persona (generated or patched) is checked as one device, so an impossible
combination is refused with a `fingerprint: ...` error and nothing is written (browser version in Chrome's scheme,
platform version, architecture, desktop-only hardware and screen, Apple Silicon minimums, languages against locale, WebGL
against the os and architecture; `docs/FINGERPRINT.md`). **WebGL** (optional `webgl: {vendor, renderer}`, not generated by a preset, `null` clears,
not compiled or run yet): only what `getParameter(UNMASKED_VENDOR_WEBGL / UNMASKED_RENDERER_WEBGL)` returns, in pages and
dedicated workers; no other WebGL value, limit, extension or rendering output changes (`docs/FINGERPRINT.md`).
**WebGPU** (no field; not compiled or run yet): with a persona applied, `navigator.gpu.requestAdapter()` resolves `null`
when the persona's `os` is not the host's or it has a `webgl` override; otherwise the real host adapter is exposed. No
adapter value is ever faked (`docs/FINGERPRINT.md`, WebGPU row).
**Accepted and returned but NOT applied:** the internal seed. Canvas, audio, fonts, media
and WebRTC SDP/stats are not spoofed (WebRTC IP handling is only the native proxy policy above), and workers are
otherwise not supported. A persona set or changed with open tabs applies its user agent, client hints and
`navigator.language(s)` at each tab's next navigation or reload (the page already shown keeps what it loaded with);
`Accept-Language` of the profile's requests follows at once; the user agent of the network contexts themselves follows
at the next browser start (page requests carry the new one already).

- `set-fingerprint {fingerprint}` (required): `null` clears the persona. Otherwise `fingerprint` is `{preset?,
  browserVersion?, locale?, languages?, timezone?, hardwareConcurrency?, deviceMemory?, screen?: {width?, height?,
  devicePixelRatio?, colorDepth?}, webgl?: {vendor, renderer} | null}`; `preset` is `windows-chrome`, `macos-chrome` or `linux-chrome`. With `preset` a new
  persona is generated (the profile keeps its internal seed) and the other fields override it; without `preset` the other
  fields patch the current persona (`locale` moves `languages` with it unless given) and a preset is required when there
  is none. Clearing and setting again draws a new seed. Unknown fields (and `seed`, `userAgent`, `os`, `platform`,
  `clientHints`) are refused, as is any value that doesn't agree with the others and a `timezone` ICU does not know;
  errors start with `fingerprint: ` and
  nothing is written. Returns `{fingerprint}`.
- `get-fingerprint {}`: returns `{fingerprint}` (`list-agents` does not carry it): `null`, the persona `{version, preset, os, platform, browser: {brand, version}, userAgent, clientHints:
  {platform, platformVersion, architecture, bitness, mobile}, locale, languages, timezone, hardwareConcurrency,
  deviceMemory, screen: {width, height, devicePixelRatio, colorDepth}, webgl?: {vendor, renderer}}` (`webgl` only when
  set), or `{unreadable: true}` (a stored value that
  can't be read; see FINGERPRINT.md). The seed is never returned.
- `create-agent` / `update-agent` **refuse** a `fingerprint` argument with an error naming `set-fingerprint`; the earlier
  per-agent form never shipped and is not kept. A persona stored per agent by that draft is ignored and removed.

## Isolated Chromium Profiles (`--shuttle-isolated-profiles`, lane B2, not yet compiled)

Opt-in and off by default. Contract: `PROFILES.md` (frozen; this section only says how the core implements it). Without
the flag nothing below applies: MCP, CDP, the 10-hex session ids and the unauthenticated loopback `/json/*` are as
before. With it:

- **Admin token.** The browser writes a random 256-bit token to `<user-data-dir>/ShuttleManagerAdminToken` (owner-only).
  A caller presents it as the HTTP header `X-Shuttle-Admin-Token` (MCP POST, `/json/*`, WebSocket upgrade). It is checked
  even on loopback, in constant time, and read from **headers only**: never from the URL or `_meta`. The network access
  token (`?token=`) is not an admin credential. Anything else a caller says about its role is ignored.
- **Browser-level tools** (no `_meta` selector allowed, unknown arguments rejected): `get-capabilities` (public),
  `create-browser-profile`, `list-browser-profiles`, `load-browser-profile`, `stop-browser-profile`,
  `delete-browser-profile` (admin only; absent from a non-admin `tools/list`; a non-admin call is `forbidden`).
  `get-capabilities` returns `{capabilitiesVersion, isolatedProfiles:{supported,enabled}, scopedRouting,
  profileScopeMetadata:"shuttle/profileId", bindingVersion, browserEpoch}`; `scopedRouting` is true only after the
  scoped CDP/drain delegates are installed.
- **Scope selector.** `params._meta["shuttle/profileId"] = "bp_<32 hex>"` on any other tool call, parsed before any session
  is touched. Needs admin (`forbidden` otherwise, a selector grants nothing). Malformed (including a non-object `_meta`,
  `null`, a non-string) is `invalid_profile_key`; unknown `profile_not_found`; not ready `profile_not_ready`. No selector:
  the startup Profile captured at startup, never the last-used one. Selector on a browser-level tool: `invalid_profile_key`.
- **Errors.** `isError:true` with `structuredContent.error:{code,message}`; codes as in `PROFILES.md` section 3. Bad
  arguments of the browser tools use `profile_failed` with the fixed message "Unexpected or malformed arguments."; messages
  never contain paths, seeds or tokens.
- **Sessions.** With a router the session id is a browser-wide UUIDv4 (collision-checked in the shared index); the owner
  `{profile, workspace agent, lifecycle epoch, browser epoch}` is recorded and re-derived on every CDP connect. Tab and
  session tools require the tab/session to belong to the Profile the call is scoped to. `open-session` of a Profile that
  is draining answers `profile_not_ready`.
- **Scoped CDP.** The `cdpEndpoint` and each tab `cdpUrl` carry a per-session bearer grant (`?grant=<256-bit>`), random,
  bound to the owner tuple, never the admin token, revoked on `close-session`, idle close, stop or restart. Decisions
  (`ConnectionDecision`): browser socket and browser-global `/json/*` → admin or 403; session socket → unknown or stale
  owner 404, no/forged grant 403, otherwise a **mandatory** scoped filter (admin gets the same filter); page socket → the
  tab's grant or admin. The scoped filter re-checks authorization on every message and closes on revoke; it exposes only
  its own targets (including popups, workers and flattened child sessions), no foreign BrowserContext, no
  `Target.create/disposeBrowserContext`, no browser-level `Storage.*` except the agent's own cookies, no window-bounds,
  command-line or histogram commands, and permission/download commands only with its own `browserContextId`.
- **Persona.** `set-fingerprint` with a selector (admin) is the only tool that addresses a `stopped` Profile: it does not
  load it, it calls `BrowserProfileRouter::ReplacePersona`, which validates the persona (a FULL persona: `preset`
  required, known time zone; `persona_invalid` otherwise, arguments are exactly `{fingerprint}`), persists the new intent,
  a pending flag and the incremented lifecycle epoch in one registry write, and answers `{fingerprint: <summary, no
  seed>}`. The bootstrap commits it (new seed) at the next load; a Profile still in memory is committed at once. Ready,
  loading, initializing, draining: `profile_busy`; failed: `profile_not_ready`. Clearing is `persona_invalid`. Cookies and
  storage are never touched. Every other tool on a stopped Profile stays `profile_not_ready`.
- **Discovery page.** `/` is gated like `/json/*` (admin or 403) when isolated profiles are on.
- **Drain / stop.** Closes admission, revokes grants, closes sessions and connections, stops the throwaway timer, flushes
  cookies, then polls every 100 ms for up to 2 min until no handle is left; a timeout leaves it `draining` (`drain_timeout`).
- Probes (opt-in, `smoke-all.sh --with profiles`): `smoke-browser-profiles`, `smoke-profile-routing`, `smoke-profile-cdp`,
  `smoke-profile-network`, `smoke-profile-cookies`. Written, never run.

## Open

- `outerWidth/outerHeight` report the window, not the resized tab viewport (detectable mismatch).
- Empty sessions (all tabs closed by hand) stay open until `close-session` or the idle timeout.
- Fidelity mitigations (`Runtime.enable` suppression) and remote takeover/screencast: not in the browser.

# Browser Profiles (isolated Chromium Profiles) — Contract V1

Normative contract shared by the manager (`shuttle-manager`) and the core
(`shuttle`, `tools/shuttle/docs/PROFILES.md`). Keep both copies identical.

Status: **opt-in, off by default** (`SHUTTLE_ISOLATED_PROFILES=1` in the manager,
`--shuttle-isolated-profiles` in the core). Core C++ is compile-untested until a
native build exists; source inspection is not evidence of working isolation.

## 0. Vocabulary

- **Profile** — identity/persona unit. An isolated Manager profile maps to one persistent
  *regular* Chromium Profile/BrowserContext, managed by Chromium's ProfileManager.
- **Browser instance** — operational and crash unit. All Profiles of a node share one
  browser and one root user-data-dir. V1 has no browser pool, sharding or load balancer.
- **Workspace agent** — a StoragePartition (`shuttle-<id>`) inside a Profile. It stays the
  secondary storage/proxy/session unit and is how cookie tooling reaches a profile.
- **Legacy profile** — every profile that exists today: Manager id == core agent id, all in
  the startup Chromium Profile, sharing one persona. Unchanged by this feature.

A persona means configuration and state that do not leak across Profiles. It does not
promise different canvas/audio/font/TLS/GPU hashes or that sites cannot link Profiles.
`--single-process` is not supported.

## 1. Identity and binding

`profiles.json` record gains a `binding` object:

```json
{
  "bindingVersion": 1,
  "logicalProfileId": "account-a",
  "nodeId": "local",
  "coreProfileKey": "bp_0123456789abcdef0123456789abcdef",
  "workspaceAgent": "workspace",
  "lifecycleEpoch": 1,
  "browserEpoch": null,
  "identityMode": "isolated"
}
```

- All fields required. `bindingVersion` is integer `1`. `lifecycleEpoch` is a positive safe
  integer. `browserEpoch` is `null` until verified contact, then the core-issued opaque
  boot id; never fabricated by the manager.
- `identityMode` is `"legacy"` or `"isolated"`. Legacy has `coreProfileKey: null` and keeps
  its agent slug. Records without `binding` are legacy (non-destructive, derived on read).
- `coreProfileKey`: core-issued, `^bp_[0-9a-f]{32}$`, immutable, opaque, never reused.
  Clients never select paths. Core keeps a separate validated key -> Chromium directory
  mapping. Nobody derives directories from keys, names or slugs. Remote nodes expose only
  opaque keys, never filesystem paths.
- Agent slugs keep `^[a-z0-9]+(-[a-z0-9]+)*$` (1-64). The same slug in two Profiles is
  valid; every agent lookup includes the owning Profile (no browser-global slug index).

## 2. Capability negotiation

Browser-level MCP tool `get-capabilities` (args `{}`, read-only):

```json
{
  "capabilitiesVersion": 1,
  "isolatedProfiles": {"supported": true, "enabled": true},
  "scopedRouting": true,
  "profileScopeMetadata": "shuttle/profileId",
  "bindingVersion": 1,
  "browserEpoch": "opaque-boot-uuid"
}
```

- Manager requires `supported && enabled && scopedRouting` and known versions.
- Unknown-tool answer from an old core => `unsupported`. Disabled/incompatible =>
  `unsupported`. Transport failure => `unavailable` (not unsupported).
- Never attempt isolated operations or silently fall back to legacy persona after a failed
  negotiation. Cache per verified browser incarnation; invalidate on disconnect/restart.
- Cookie `CORE_CAPS` (`cookies/transport.js`) is separate and unchanged.

## 3. Authorization, routing and tools

Browser administration uses a separate random 256-bit credential in the owner-readable
`ShuttleManagerAdminToken` file in the user-data-dir, presented as
`X-Shuttle-Admin-Token`. Validated even on loopback. Never in URLs, MCP metadata, endpoints,
UI or logs. Existing network access tokens are not admin credentials. Authenticated
request context is propagated through the HTTP/delegate/router interfaces; a
caller-supplied role field is never trusted.

`get-capabilities` is public/read-only. All browser-profile tools are admin-only and
absent from non-admin tool listings.

Scope selector, parsed and authorized **before** session touching or dispatch:

```json
{"method":"tools/call","params":{"name":"list-agents","arguments":{},
 "_meta":{"shuttle/profileId":"bp_0123456789abcdef0123456789abcdef"}}}
```

- Absent selector => the fixed startup/legacy Profile captured at startup (never last-used).
- Malformed => `invalid_profile_key`; unknown => `profile_not_found`; not ready =>
  `profile_not_ready`. Never fallback. Selectors on browser-level tools are rejected.
- A selector grants no authority.

Browser-level tools (schemas reject unknown properties):

| tool | args | result |
| --- | --- | --- |
| `create-browser-profile` | `{requestId: uuid, workspaceAgent, persona, proxy?}` | `{profile}` in `stopped`; idempotent by `requestId` (`request_conflict` on divergent replay) |
| `list-browser-profiles` | `{}` | `{profiles, browserEpoch}` |
| `load-browser-profile` | `{coreProfileKey}` | `{profile}` at `ready`; concurrent loads share one init |
| `stop-browser-profile` | `{coreProfileKey}` | `{profile}` at `stopped` after drain+flush; never stops the browser |
| `delete-browser-profile` | `{coreProfileKey}` | `{profile}`; requires `stopped` and no descendant handles (`profile_busy`); never the startup profile |

`ProfileRecord`: `coreProfileKey, workspaceAgent, lifecycleEpoch, browserEpoch, lifecycle,
loaded, deletionPending, personaSummary, lastErrorCode`. No paths, credentials, seeds or
endpoints.

Errors: MCP `isError:true`, `structuredContent.error:{code,message}`; codes:
`forbidden, unsupported, invalid_profile_key, profile_not_found, profile_not_ready,
profile_busy, profile_failed, persona_invalid, proxy_not_ready, storage_error,
drain_timeout, request_conflict`. Messages never contain secrets.

## 4. CDP ownership and connection decisions

Explicit `ConnectionDecision`: `admin-allowed`, `scoped-filter` (mandatory non-null filter),
`denied` (rejected before WebSocket accept). Applies to browser, session and page sockets:
unknown/stale owner => HTTP 404, authorization failure => 403; a null filter never means
"unfiltered". In enabled isolated mode the raw browser WebSocket and browser-global
`/json/*` require admin.

MCP is routed through the browser router; CDP resolves through owner indexes; neither
uses `GetLastUsedProfileIfLoaded()`.

Session ids: browser-wide UUIDv4, collision-checked in a shared index. Session index entry:
`{Profile, workspaceAgent, lifecycleEpoch, browserEpoch}`; target index adds owning session
and partition. IDs are locators, not authority. A scoped endpoint carries a separate random
per-session bearer grant (never the admin token) bound to the owner tuple; stop/restart
revokes it and its connections. Filters cover commands, responses, discovery and events
including popups/workers/flattened child sessions, deny foreign BrowserContexts, profile
creation and cross-owner Storage access.

## 5. Lifecycle and persistence

States: `stopped -> loading -> initializing -> ready -> draining -> stopped`;
failures -> `failed`; retry `failed -> loading`; cleanup `failed -> stopped`;
`stopped -> deleted` (tombstone, `deletionPending` until physical removal confirmed).

Drain: atomically close admission, revoke handles, cancel/wait tasks, close
sessions/tabs/workers/connections, flush storage/prefs. Timeout stays `draining`
(`drain_timeout`), never reports `stopped`. `loaded:true` may persist after stop; no
RAM-unload guarantee.

Core: new `BrowserProfileRegistry` / `BrowserProfileRouter` (the existing
`shuttle::ProfileRegistry` is the *agent* registry and is untouched in meaning).
Versioned `ShuttleBrowserProfiles.json` at the user-data-dir root, serialized atomic
replace; stores keys, directory mapping, workspace, epochs, idempotency/provisioning
intent, tombstones. Never endpoints. Lifecycle epoch is persisted+incremented before a new
load, drain, deletion or identity replacement; idempotent repeats do not increment.
Browser restart changes the browser epoch; non-deleted entries recover `stopped`. Startup
never replays cookie imports/provider uploads.

## 6. Bootstrap ordering

`CreateProfileAsync`'s initialized callback is too late (keyed services already exist).
Add `chrome/browser/shuttle/managed_profile_bootstrap.{h,cc}` with
`PrepareManagedProfile(Profile*, completion)` invoked from `ProfileImpl::OnPrefsLoaded`
after success/shutdown checks and before `OnLocaleReady`. It validates/restores persona via
registered PrefService APIs (never writes the Preferences file), provisions workspace
prefs without constructing Shuttle services, and prepares the proxy encryptor before
`ProfileProxyServiceFactory` builds services. Managed NetworkContext creation is gated
until bootstrap completes. Corrupt/missing committed persona fails closed
(`profile_failed`), no stock fallback. If the ordering cannot prevent early identity-
dependent contexts the isolated capability stays disabled.

## 7. Persona V1

Fields (`ParseFingerprintPersonaInput`/`CheckFingerprintPersona`): `preset, browserVersion,
locale, languages, timezone, hardwareConcurrency, deviceMemory,
screen{width,height,devicePixelRatio,colorDepth}, webgl{vendor,renderer}|null`.
Presets `windows-chrome|macos-chrome|linux-chrome`; offer host-compatible ones. Reject
arbitrary UA, seed, platform/client-hints and flags. Generated once, persisted, never
regenerated on Start/poll. Scoped `set-fingerprint` needs `stopped`/drained and never
clears cookies. Proxy stays at workspace agent/partition. No claim of browser-global
proxying or unverified fingerprint surfaces.

## 8. Manager integration

- Flag `SHUTTLE_ISOLATED_PROFILES=1` (default off). New profile `identityMode:"isolated"`
  is opt-in per create; node must report capability. No fallback.
- Start (isolated): negotiate -> `load-browser-profile` -> verify workspace -> open scoped
  session, all under `withProfileLock`. Stop: settle manager descendants, then
  `stop-browser-profile`. Delete: drain, `delete-browser-profile`, then drop metadata after
  ack. Legacy lifecycle unchanged. `list-agents` is not the existence source for isolated
  profiles; reconciliation is Profile-scoped and never prunes unloaded/unsupported records.
- Owner keys: `JSON.stringify([nodeId, coreProfileKey, workspaceAgent, sid])`; profile
  owner key omits `sid`. Owned/endpoints/tasks/viewers bind to lifecycle+browser epochs.
- Cookie jobs add `profileOwnerKey`, `lifecycleEpoch`, `browserEpoch` to the binding and
  compare before every browser/provider call. All existing cookie/service contracts stay
  (see `docs/COOKIES.md`); no auto-start of stopped profiles.
- API: `POST /api/profiles` accepts `identityMode`, `persona`; `GET /api/state` adds
  `identityMode, lifecycle, personaSummary, capability:{supported,enabled,reason}`;
  `PUT /api/profile/:id/persona` needs `manage` and a stopped profile.
- Rollback: older core => isolated profiles show `unsupported`/unavailable; metadata and
  browser data untouched, never converted to legacy or pruned. Code rollback does not
  roll back cookie/account state. No migration of legacy data by copy/rename or cookie
  export/import (CHIPS/IndexedDB/SW/localStorage would be lost).

## 9. Negative tests

duplicate slugs across profiles; traversal/unknown keys; forged admin/scope; cross-profile
targets/cookies/events; stale epochs/grants; concurrent lifecycle calls; proxy failure/
restart; corrupt persona; drain timeout; delete crash; old core; local/remote endpoint
mismatch; cookie permission revocation and job replay.

## 10. Implementation status (core lane B1)

Not yet compiled; written without a build host. The contract above is unchanged.

Bootstrap ordering audit: the earlier claim that no keyed service exists at the hook was incorrect; see
"Review fixes" below. `OnPrefsLoaded` precedes `OnLocaleReady`, `OnSyncToSigninMigrationMaybeCompleted` and bulk
`CreateBrowserContextServices`; that ordering alone is not proof of a managed NetworkContext gate.

B2 must call or implement (see `browser_profile_router.h`, `browser_owner_index.h`, `browser_admin_auth.h`):
`BrowserProfileRouter::Get()`, `Resolve`, `MakeOwner`, `KeyOfProfile`, `CreateProfile`, `ListProfiles`, `LoadProfile`,
`StopProfile`, `DeleteProfile`, `SetDrainDelegate(BrowserProfileDrainDelegate*)` (B2 implements `DrainProfile` and
`HasDescendantHandles`), `SetScopedRoutingReady(bool)`, `session_owners()`, `target_owners()`, `ProcessBrowserEpoch()`,
`BuildCapabilities()`, `VerifyBrowserAdminToken`, `kAdminTokenHeader`, `IsBrowserAdminAuthAvailable`.

Known limits: sync creation of a managed profile (`--profile-directory`) is unsupported; `CommitPendingWrite` gives no
success flag; delete of a loaded profile leaves the directory until the next start; `StopProfile` returns `unsupported`
without a drain delegate; `set-fingerprint` and persona replacement while stopped belong to B2; the Windows admin token
file relies on the user-data-dir ACL (POSIX 0600); the request fingerprint hashes the plaintext proxy; there is no
invalid-argument error code, so bad `workspaceAgent`, `proxy` or `requestId` use `profile_failed` with a fixed message.

### B2 status (core lane B2)

Not yet compiled; written without a build host. Sections 1-9 are unchanged.

Implemented (see `MCP.md` "Isolated Chromium Profiles"): admin request context through the HTTP handler and
`DevToolsManagerDelegate`; explicit `ConnectionDecision` for browser/session/page sockets; browser-global `/json/*` admin-only;
per-session bearer grants; scoped `SessionCdpFilter` (per-message re-authorization, own targets and context only);
MCP routing by `_meta["shuttle/profileId"]` before any session is touched; the six browser-level tools (`get-capabilities`
public, the rest admin-only and hidden from non-admin listings); UUIDv4 session ids with owner tuples; `Drain` through
`BrowserProfileDrainDelegate`; scoped `set-fingerprint` (B2: ready and drained; replaced by B3 below: stopped only; clearing refused).

Known limits: windows of a managed Profile that are not leased are not closed on drain (the discovery page, last-used
`GetBrowserContext` and persona replacement gaps of B2 are closed by B3 below); `scopedRouting` stays false when the router is missing at DevTools delegate creation.

### B3 status (persona replacement, closes the B2 gap)

Decision: while a managed Profile is `stopped`, the source of truth of its persona is the registry's persisted provisioning
intent (`persona_input_json`). `BrowserProfileRouter::ReplacePersona(key, persona_json)` validates it
(`ParseFingerprintPersonaInput` + `CheckFingerprintPersona`; a full persona, preset required), requires `stopped`
(`profile_busy` for ready/loading/initializing/draining, `profile_not_ready` for failed, `profile_not_found` for deleted),
persists intent + `personaReplacementPending` + the incremented lifecycle epoch in ONE atomic registry write, refreshes
`personaSummary`, revokes the Profile's owner-index entries, and never loads the Profile or touches cookies/storage. The
bootstrap commits the pending intent through PrefService at the next load (new seed; an unreadable old persona is no
obstacle); a Profile still in memory (no unload guarantee) is committed live by the router. Scoped `set-fingerprint`
(admin, selector) is routed to it before `Resolve()`, so it is the only tool that reaches a stopped Profile; a ready
Profile never takes a live change (changed from B2). Also: the discovery page `/` is admin-gated; with the router,
`GetBrowserContext(id)` resolves only the startup Profile (never last-used); `DevToolsBrowserContextManager::
CreateBrowserContext` returns null and `Target.createBrowserContext` is refused by the scoped filter, so scoped clients
cannot reach it. Not compiled. Known limits: a managed Profile's own `browserContextId` does not resolve in the content
handlers; if the live commit of a still-loaded Profile fails after the intent was persisted, the flag stays set.

Tests: `scope_auth_unittest.cc` (selector parser, decision table, grants); opt-in probes `smoke-browser-profiles`,
`smoke-profile-routing`, `smoke-profile-cdp`, `smoke-profile-network`, `smoke-profile-cookies` (`smoke-all.sh --with profiles`).
None has been built or run.

### Review fixes

Source changes only; **native C++ remains uncompiled**. The partial writer's fixes were inspected and retained.
Sections 1-9 are unchanged.

- CDP: the shared policy rejects non-flattened attach/auto-attach and Target message tunnelling. Legacy scoped
  transports also reject these tunnels. Browser/Storage commands on page sockets and flattened children are denied,
  independent of nesting; root Storage cookie calls still route to the owning agent partition. Flattened child ids
  must have been announced as owned; nested attachments require an owned parent. Security-sensitive events are parsed
  without JSON key-order assumptions; receivedMessageFromTarget and unowned Browser/Storage events are suppressed.
- Persona: per-profile admission spans registry persistence and the loaded Profile's pref commit, blocking load,
  delete and replacement. Epoch plus exact intent are checked in continuations, MarkPersonaCommitted and now
  MarkProvisioned, so an old callback cannot clear newer pending intent. No cookie API is called by replacement.
- Drain: delegate exhaustion remains draining, retry resumes cleanup, and failed/stopped cleanup cannot certify
  surviving handles. The router rechecks descendants after asynchronous pref flush before releasing its runtime;
  the in-flight marker covers that flush to prevent a second concurrent drain.
- Bootstrap evidence: `ProfileImpl::LoadPrefs` calls `CreateExtensionPrefStore` when constructing the PrefService;
  `Profile::CreateExtensionPrefStore` calls `ExtensionPrefValueMapFactory::GetForBrowserContext(create=true)`.
  That factory depends on ExtensionRegistry and builds a preference map, not a persona/proxy/network service.
  Persona and proxy factories are eager at bulk service creation; ProfileNetworkContextServiceFactory depends on
  both. `ConfigureNetworkContextParamsInternal` obtains the proxy/persona services and applies them to network
  parameters. Neither that path nor ProfileImpl currently has an explicit managed-bootstrap-complete NetworkContext
  admission gate. Therefore `IsolatedProfilesBootstrapSafe()` is **false**: capability unsupported/disabled and
  managed loads fail closed until the network gate and early-creation ordering are proven on a native host.
  Early preference services are not themselves evidence of an identity leak; there is no blanket no-keyed-services claim.
- Focused coverage: scope_auth_unittest exercises all three transport forms, tunnelling, malformed flatten/context,
  and root-only cookie routing; registry tests cover stale provisioning epoch/intent and stale replacement commits.
  smoke-profile-cdp includes startup-cookie sentinels, page/child read/write/clear refusals and tunneled Browser.close.
  JS syntax checked with `node --check`; runtime probes and native unit/browser tests have not run. With capability
  disabled, opt-in profile probes are expected to skip, not demonstrate isolation. Native runtime verification remains
   mandatory, including replacement/lifecycle interleavings, drain timeout/retry with real descendants, and first network identity.

### Managed NetworkContext admission and remaining native gate (2026-11-17)

Source-only follow-up; sections 1–9 remain frozen. This supersedes the disabled
source capability noted in Review fixes, not the requirement for native evidence.

- `ProfileImpl` installs BrowserContext-owned admission at constructor entry,
  before delegate callbacks or `LoadPrefsForNormalStartup`. Early extension
  preference factories remain allowed. `StoragePartitionImpl::InitNetworkContext`
  binds pending NetworkContext **and cert-verifier updater** pipes and calls the
  new `ContentBrowserClient::AwaitNetworkContextAdmission` lifecycle callback.
  Chrome retains weak-bound continuations until bootstrap finishes. Configuration,
  identity service lookup and `CreateNetworkContextInNetworkService` occur only
  in the admitted continuation, also on network-service restart. Destruction or
  bootstrap failure drops pending receivers, never constructs a stock context.
- Successful admission requires validated persisted persona, workspace and proxy
  readback plus required PrefService commit. The gate retains the validated
  encryptor; the proxy factory synchronously adopts it before returning the
  service (no second async auth-readiness race). Targeted CHECKs on persona/proxy/
  network service construction reject unexpected early direct identity-service
  construction; these are invariant failures, not a blanket keyed-service ban.
  `IsolatedProfilesBootstrapSafe()` now denotes this source barrier; runtime
  rollout stays off by default and requires explicit opt-in and native evidence.
- Existing loaded Profiles bypass Chromium bootstrap: router recovery validates
  and applies pending intent, waits for commit even on an exact match, checks
  epoch plus exact normalized persona and intent, then conditionally clears the
  registry intent **before** ready admission. Corrupt pending input cannot accept
  an older valid persona. Existing conditional epoch/intent security fixes and
  CDP transport restrictions are retained.
- Drain in-flight/deadline cover initial registry write, delegate, every loaded
  partition's acknowledged cookie flush, asynchronous pref commit and stopped
  persistence. Retried stops join the ongoing operation; timeout answers
  `drain_timeout`, leaves effective state draining and retains runtime ownership.
  Only durable stopped completion releases it. Failed stopped persistence leaves
  draining with `storage_error`, rather than claiming success.
- Added native browser cases: pending/failed admission continuations, bulk-service
  admission ordering, proxy auth readiness/first NetworkContext path, valid and
  corrupt pending intent on an already-loaded Profile, and deterministic held
  pref-flush timeout/retry (one delegate invocation and retained runtime).
  Dependencies are wired in `//chrome/browser/shuttle:browser_tests`; this tree's
  `//chrome/test:shuttle_profile_tests` compiles a source set, not an executable
  browser-test runner. Native compile/link and execution have **not** occurred.
  HTTP header/first-request, network-service restart with authenticated proxy,
  real descendant drain races, cookie regressions and supported native platform
  verification remain mandatory before rollout. No build/restart/deploy performed.
- Authoring checks: scoped tracked and new-file `git diff --check` passed;
  `node --check` passed for all five profile probes and `lib/profiles.mjs` /
  `lib/smoke.mjs`. These checks do not compile C++, validate GN dependencies or
  demonstrate isolation. Loaded replacement/recovery also recloses network
  creation until exact pending intent is committed; a failed attempt keeps that
   admission pending for retry rather than permitting stale-persona construction.

### Loaded persona NetworkContext retirement P1 (2026-11-18)

Source-only implementation; sections 1–9 are unchanged. Native compilation and
execution remain unavailable. This corrects the preceding gate-only replacement:
pausing construction did not update existing StaticHttpUserAgentSettings.

- New scoped `StoragePartition::RetireNetworkContext` stops running service
  workers without unregistering them, then uses service-owned
  `NetworkContext::Retire`. Retirement destroys all factories (including
  ignore-reset factories/in-flight loaders), snapshots and flushes cookies, and
  destroys the context. The browser waits for snapshot reply **and** a separate
  completion-pipe disconnect held across the entire destructor (the primary
  pipe closes partway through destruction). Cloned contexts, cookie remotes,
  socket/factory dependents and cert
  updater pipes cannot retain the retired context. A missing snapshot fails
  closed and remains latched until browser restart; it is not silently retried
  as a successful handoff.
- Only the owning Profile's already-loaded partitions are enumerated; no global
  network service restart or storage deletion. Cookies (including in-memory and
  nonpersistent session cookies) travel as an in-memory `initial_cookies`
  snapshot and restore before binding the new context receiver. IndexedDB,
  localStorage and SW registration backends are not reset. UI and IO cached
  browser factories reset after retirement; IO reset queues construction without
  awaiting the closed admission gate. Context/cert updater reconnection reuses
  the normal admission-aware crash path, guarded by construction generation.
- Both live stopped replacement and loaded pending-intent recovery retire before
  applying identity. Their existing lifecycle operation guards and epoch/exact
  intent checks remain. Admission reopens only after persona commit, proxy
  identity verification and conditional registry provisioning persistence.
  Failure leaves workload unavailable and pending intent intact.
- Written browser regression: real initial context, cached browser-process
  factory request before/after distinct version+language replacement, renderer
  navigation, cookie/localStorage retention, same loaded Profile, unaffected
  second Profile, loaded pending recovery and network-service crash/reconnect.
  Corrupt recovery asserts admission stays closed. Extended network smoke also
  checks first renderer request and production `test-proxy` browser-process UA
  and language after replacing an already-loaded proxied Profile.
- Remaining native validation: compile/link/GN and all runtime tests; authenticated
  proxy restart, retirement-disconnect/flush failure injection, IO cache races,
  IndexedDB/SW registration retention, shared-worker recovery and supported
  platform coverage. No claim that these surfaces have passed. No remote build,
  browser restart/deploy, commit or push was performed.
- Authoring checks run: `node --check` on the touched network smoke probe;
  scoped tracked/new-file whitespace checks. Source signatures checked for
  `StopAllServiceWorkers`, UI/IO reconnectable factory Reset, cookie snapshot,
  flush and canonical-cookie restore, Mojo callback drop handling and GN test
  dependencies. These checks are not C++ compilation or runtime evidence.

### P1 follow-up audit — blocked, not fixed

- Reopened implementation/coverage checkboxes in
  `plans/persona-network-context-refresh.md`. Earlier status above describes the
  written implementation, not an established preservation guarantee. Both P1
  findings remain: normal web-write cookie replay can reject a valid HTTPS-origin
  nonSecure session cookie overlapping a disk-loaded Secure cookie; browser
  snapshot ownership ends before restore acknowledgement and pending intent can
  clear before replacement restoration succeeds.
- Audited actual CookieMonster APIs: deprecated `SetAllCookiesAsync` clears only
  the unpartitioned map and performs per-insert garbage collection;
  `StoreLoadedCookies` is private and sanitizes loaded entries. Neither is a safe
  drop-in trusted handoff API. Full canonical metadata must not pass through a
  lossy export/import format or synthesized web Set-Cookie source.
- Architectural blocker: the single admission gate controls construction, while
  factory work is queued on the replacement pipe. A safe fix requires distinct
  staged restoration and workload activation, browser-owned snapshots (including
  empty snapshots), and generation/failure ordering across restoration ACK,
  asynchronous registry persistence and network-service crash. The focused plan
  records the minimal amendment and required failure injections. Exploratory
  code changes were removed; existing implementation remains unfixed.
- No build, native tests, live restart, commit or push. Frozen contract sections
  are unchanged. Browser restart cannot honestly guarantee recovery of session
  cookies if the only in-memory snapshot was lost.

### Expanded handoff source draft (2026-11-18) — incomplete, not ship-ready

Sections 1–9 are unchanged. Source changes now add trusted CookieMonster snapshot
replacement (both maps, full canonical copies, disk-load ordering, no web write
checks/GC), optional empty-vs-absent snapshots, and browser-owned handoff state.
Private control-pipe construction/restore precedes registry commit; workload
pipes remain unbound until service-side activation. Generation loss during
restore/persistence fails closed; conditional registry compensation restores
pending intent. Both live stopped replacement and loaded pending recovery enter
staging. Production changes enumerate only the target Profile's loaded partitions.

New unrun tests cover Secure persistent `/` plus HTTPS nonSecure session `/sub`,
CHIPS/nonce/ancestor full canonical equality, duplicate rejection, empty/retry,
late persistent-store key replies, empty staged context crash/retry and registry
compensation. Existing browser cached-factory first-header/storage/other-Profile
coverage remains. Tests use existing net unit and Shuttle unit/browser GN wiring.

All-partition finalization now retains snapshots until the activation barrier,
rechecks generations and closes control owners on partial failure. Its asynchronous
rollback destruction still needs ordering evidence. Still incomplete: complete
router-path restore-disconnect and crash-during-commit injections, IndexedDB/SW
retention coverage, native compilation and execution. Per-partition activation
has an ACK, but this is not an atomic disk/network transaction or a full-browser
crash session-state guarantee. Failed compensation keeps this browser closed;
full browser crash can irretrievably lose nonpersistent cookies/snapshot.
See the focused plan for precise open gates. Scoped whitespace and network-probe
JS syntax checks passed; no native build/test, live restart, commit or push ran.

### Bounded continuation — source still incomplete

Activation ACK now retains an unbound workload receiver; failed partition
preparation does not release siblings. Staged contexts have a destruction-lifetime
pipe held across their destructor. Router rollback waits for every participant
before compensation; snapshots survive and retry requires explicit retirement.
Added unrun production-router disconnect/crash-at-manifest-write/partial-prepare
retry cases and normal-JS IndexedDB/service-worker registration retention checks.
Existing exact canonical cookies and cached-factory/other-Profile checks remain.

Superseded source draft: a NetworkService-owned transaction now validates fixed
attempt/generation membership and binds all ordinary workload ReceiverSets in one
non-yielding release task, with Released/Rejected acknowledgement. Router retains
fixed participants/keepalive/epoch/intent; marks every snapshot nonreplayable before
Release, and quarantines lost replies with teardown fences, no snapshot replay,
closed admission and secret-free indeterminate failure. Released retires snapshots
together. Registry/load failure completion now goes through acknowledged teardown
and conditional compensation. Both retirement reconnect handlers are suppressed;
staged private control is allowlisted and restore-in-progress owner loss watched.

NOT source-complete: snapshots remain partition-owned instead of router-owned;
full runtime phase transitions and cancellation/reentrancy audit remain. New
unrun service-path tests cover missing/duplicate membership, stale/foreign prepare,
queued ordinary calls and repeat Release; they do not establish externally counted
request ordering or the crash/lost-reply/post-release-cookie-mutation matrix.
Implementation and coverage stay unchecked. Precise continuation is in
`plans/persona-network-context-refresh.md`. Native Mojo/C++ build/tests remain
UNRUN; authoring whitespace/JS checks are not native evidence. No live restart,
build, commit, push, manager or settings edit performed.

### Exclusive implementation continuation — still not a completion claim

Authoritative canonical snapshots and release markers are now encapsulated in
shared browser-only generation state retained strongly by router operations;
partitions reference that state. Pre-release retry retains it, successful successor
operations allocate fresh state, and shutdown makes no session-cookie fidelity
claim. Router phases have a central transition check. Reentrant teardown joins
waiters at both router/partition layers, retirement cancellation waits its fences,
and failure exits share conditional compensation. Router destruction cancels
continuations before teardown; Released ACK discards snapshots before stale-owner
failure handling. This is source-only and the outer lifecycle callback audit is
not complete.

Additional UNRUN tests count real HTTP before/after common release, exercise
duplicate Prepare, failed registry commit/sibling retry, second retirement and
joined abort, and reject snapshot replay after a released Set-Cookie request while
another Profile makes HTTP. The mutation test withholds browser finalization; it
does not actually drop a Mojo reply through the router. Missing source assertions
are all-bound-before-first-dispatch, full router lost-reply/indeterminate ordering,
and cancellation/stale-epoch interleavings. Precise file/function follow-up is in
`plans/persona-network-context-refresh.md`. Implementation and coverage stay open;
native build/execution remains UNRUN. Scoped whitespace and network-probe JS checks
passed; no build/restart/commit/push/manager/settings edit occurred.

### Oracle post-review compile-blocker fix (2026-11-18)

Two definite compile blockers and one shadowed-local are fixed; no native evidence:
`[ServiceSandbox=sandbox.mojom.Sandbox.kNetwork]` moved from the inserted
`enum StorageHandoffReleaseResult` onto `interface NetworkService`, and
`[RequireContext=sandbox.mojom.Context.kPrivilegedUtility]` moved from the
inserted `struct StorageHandoffMember` onto `interface NetworkContext`, in the two
public mojoms. `OnInitializingPersisted`'s local `ManagedProfileIntent intent`
now reads `identity` (assignments and `VerifyManagedProfileIdentity` argument
included), leaving the `std::string intent` parameter untouched. Scoped
`git diff --check` is clean; both mojoms were parsed with the repo's existing
`mojom_parser` front end and show the annotations only on those interfaces.
Review found no further definite runtime P0/P1, but native compile and all native
tests remain UNRUN; plan checkboxes stay open. No build/restart/commit/push/
manager/settings edit.

### Admission-crash source continuation (2026-11-22; native UNRUN)

The f64ceb49 staged-restore admission CHECK crash has a bounded SOURCE change,
not a validated native fix: router-only move-only construction proof now flows
explicitly through StoragePartition/ContentBrowserClient/Chrome into the internal
network configuration helper. It is bound to exact Profile/operation/epoch/intent,
participant, retained snapshot, successful retirement, restoring phase and
attempt/new generation; weak ownership and immediate precreation checks fail
closed. No ordinary admission opening, ambient staging bypass, new Mojo field or
service-operation-token prerequisite. Existing-only network/persona/proxy services
are required while paused; strict keyed factory readiness checks remain.

New source tests cover invalid/moved/stale/reused/cancelled proof and missing
network service; production replacement/pending recovery first HTTP checks now
include retained cookies and new persona headers. Native compilation/execution
remain UNRUN. GN audit confirms lifecycle cases are a disabled source_set, NOT a
runnable executable: the pruned Chrome browser-test harness must be restored.
`//chrome/browser/shuttle:shuttle_profile_unittests` covers registry/scope only;
production target is `chrome`. Missing source variants and exact validation
limits are appended to `plans/persona-network-context-refresh.md`. No
build/deploy/restart/commit/reset/stash or Manager/settings edits in this task.

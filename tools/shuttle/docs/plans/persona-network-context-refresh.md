# Persona NetworkContext refresh (bounded P1)

## Steps
- [x] Audit scoped NetworkContext ownership and factory/crash recovery APIs.
- [ ] Implement acknowledged per-partition retirement and integrate both loaded intent paths (source draft present; activation/crash fence still requires closure).
- [ ] Write production-path regression coverage and run available authoring checks.
- [ ] Compile and run native browser tests on a supported build host.

## Execution detail
`StoragePartitionImpl::InitNetworkContext` is the existing context-disconnect
recovery entry point and passes `AwaitNetworkContextAdmission`.
`StoragePartition::ResetURLLoaderFactories` resets UI/IO reconnectable browser
factories, but leaves StaticHttpUserAgentSettings unchanged. NetworkContext's
primary receiver owns its lifetime through `OnConnectionError`; cloned receivers
do not keep it alive. `NetworkContext::Retire` flushes and snapshots cookies;
the caller waits for both its reply and a separate completion pipe held across
the full destructor (not the primary pipe, which closes partway through), proving
destruction of dependent factories and clones. `initial_cookies` restores the
snapshot before binding a replacement receiver, including memory/session cookies.
Expose scoped retirement through StoragePartition, reuse its normal
reconnect path, and invalidate superseded deferred construction callbacks.
Enumerate only `BrowserContext::ForEachLoadedStoragePartition`; never create
unloaded partitions. Keep admission closed across retirement, persona commit,
proxy verification and conditional registry persistence. Do not wait for a new
context or factory while the gate is closed.

Success: first browser-process and renderer requests after A-to-B replacement
(including already-loaded pending-intent recovery) use B's version/language;
cached factories cannot use A, another Profile remains available and unchanged,
cookies and non-network storage survive, failures retain pending intent and deny
workload admission. Native tests are written, not claimed run without a build.

## Remaining native validation
The new browser case covers cached browser-process factories, renderer navigation,
cookie/localStorage retention, another live Profile, loaded pending intent, and
network-service restart. The existing corrupt recovery case now asserts closed
admission. The smoke probe adds first-request renderer identity and browser-process
`test-proxy` identity after version replacement. Authenticated proxy restart,
IndexedDB/SW registration retention, IO factory races and retirement disconnect
failure injection still require native fixture expansion and execution.

## Bounded continuation status (2026-11-18)

Source additions, NOT a completion/ship claim:
- Activation ACK now prepares and retains an UNBOUND workload receiver. A second
  partition's prepare failure cannot execute requests on the first partition.
- Staged construction carries a separate lifetime pipe. On owner disconnect the
  service holds it across the entire destructor. Router staging/activation failure
  and compensation wait for all participating lifetime fences, retain snapshots,
  and require explicit retirement before retry; stale fence callbacks cannot
  silently restore again. TestNetworkContext implements the new Mojo method.
- Production-router browser cases now inject restored-owner disconnect, service
  crash at the real manifest write-start boundary, and second-partition activation
  preparation failure/retry. These are written, not executed. Existing empty and
  stale-generation partition checks and exact-cookie cases are retained.
- Existing same-origin browser fixture now writes/reads real IndexedDB and registers
  a service worker using normal JS APIs; retention uses getRegistration without
  re-registering. Existing cached-factory and other-Profile checks remain.

Superseded by the common-coordinator draft below. Implementation/coverage remain
unchecked: the new source is uncompiled and its failure matrix is incomplete.

Checks: scoped tracked git diff --check and node --check of the network smoke
passed. C++/Mojo signatures were inspected (including test fake and forward
declarations), not compiled. GN/native unit/browser tests remain UNRUN; no build,
commit, push, manager edit, live restart, or settings modification performed.

## P1 review blocker and required amendment

The implementation and coverage steps are reopened. The existing implementation
is not safe to ship: retirement acknowledgement is not restoration acknowledgement.

Storage audit: `CookieMonster::SetAllCookiesAsync` is unsuitable: `SetAllCookies`
clears only `cookies_`, not `partitioned_cookies_`, and invokes garbage collection
after each insert. `StoreLoadedCookies` is private and runs duplicate/control-char
sanitation through `EnsureCookiesMapIsValid`; it is not a public snapshot merge.
Normal `SetCanonicalCookie` replay incorrectly treats trusted storage as a web
write. In particular HTTPS-origin nonSecure session A at `/sub` can be rejected
when persistent Secure A at `/` has already loaded. Merely changing the synthetic
source URL is not a trusted restoration API.

Admission audit: `InitNetworkContext` prepares the replacement pipe and queues
factory calls while `AwaitNetworkContextAdmission` waits. The only current gate
releases construction callbacks; binding the restored receiver then drains those
workload calls. Waiting for restoration before opening this gate deadlocks;
opening it to restore releases workload before the registry transaction finishes.
Furthermore a service crash after restoration ACK but during asynchronous
`MarkPersonaCommitted` can invalidate that ACK before pending intent is cleared.
Neither a one-shot ACK nor retaining a vector alone closes that window.

Minimal required amendment (must be implemented and failure-tested together):

- Add a trusted CookieMonster storage-handoff API, queued after complete disk
  loading, validating the entire snapshot before mutation, reconciling both maps
  without web-write checks, alias pruning or per-insert eviction. Preserve full
  canonical metadata, including partition nonce/ancestor and source scope.
- Represent absent handoff separately from acknowledged empty handoff. Browser
  owns the authoritative snapshot throughout staging and registry persistence;
  ordinary reconnect cannot bypass it or replace it with a partial store.
- Introduce explicit construction/staging, restored and workload-active phases.
  Stage only affected loaded partitions with fresh identity while retaining
  their unbound workload receivers. Restore ACK must be generation-scoped and
  distinct from activation; all partition failures invalidate the transaction.
- Define failure ordering across asynchronous registry persistence and service
  crash. Do not clear durable intent on a stale restore ACK. Preserve the snapshot
  and deny routing on failed/disconnected restoration; specify how a failed
  activation retains/re-establishes durable pending intent before reporting an
  outcome. Do not claim browser restart recovers nonpersistent session cookies
  after the sole snapshot is lost; that is unrecoverable, not successful recovery.
- Add exact overlapping-path regression and full canonical-member comparisons,
  CHIPS nonce/ancestor cases, empty handoff, restoration failure/disconnect and
  crash-during-persistence injection, retry deduplication and second-profile
  availability assertions. Compile/run on a supported native host separately.

The previous investigation stopped at this sequencing blocker. The source draft
below supersedes that stop; Frozen Contract V1 remains unchanged. Existing native
tests do not establish these guarantees, and no native test pass is claimed.

## Expanded handoff implementation draft (2026-11-18)

- `CookieMonster::RestoreStorageSnapshotAsync` validates a complete native
  snapshot before replacing both maps; uses internal insertion without web
  overwrite checks, alias pruning or per-insert GC; reconciles disk through
  ordered delete/add and flush. `OnKeyLoaded` ignores replies after full-load
  completion so late priority results cannot resurrect pre-handoff data.
- `initial_cookies` is optional: absent means ordinary construction; present
  empty is authoritative empty storage. Browser retains the vector through
  restore and registry persistence. This is not provider/import replay.
- Partitions stage a private control NetworkContext while workload receiver
  remains unbound. Restore acknowledgement and the post-persistence fence use
  that control pipe. Normal reconnect fails closed while snapshot is pending;
  retry must explicitly retire/re-enter staging. Both loaded replacement and
  loaded-pending recovery use staging. Only loaded partitions of the target
  Profile are enumerated; no global restart or storage clearing was added.
- Registry compensation `ReinstatePersonaPending` is epoch/intent conditional.
  A lost generation after commit triggers compensation before error completion;
  failed compensation leaves admission closed in this browser. This is NOT an
  atomic disk/network transaction.
- Added cookie regression covers exact Secure persistent `/` plus nonSecure
  HTTPS-origin session `/sub`, CHIPS, nonce/same-site ancestor, full canonical
  member equality, duplicate rejection, retry and empty replacement. Deferred
  store test covers full-load ordering and late priority replies. Browser case
  exercises real retirement/staging/fence, empty snapshot and crash/retry. A
  registry case exercises conditional compensation. Existing cached-factory /
  first-header / storage / second-Profile case remains.

### Open correctness and evidence gates — do not ship this draft

The final activation now has a service-side `ActivateStorageHandoff` reply,
bound to the partition generation: workload binding is its linearization point;
browser snapshot release occurs only after all partition replies. Loss before the reply
fails/compensates, loss after it is ordinary post-activation service failure.
The browser now retains every partition snapshot through the activation barrier,
rechecks restored generations, and closes control owners on partial failure
before compensation. This remains a draft: rollback destruction is asynchronous,
and the complete failure-ordering fixture has not established this protocol.
The implementation step therefore stays unchecked. In-flight commit crash and restore-disconnect
fault injection through the complete router path are still missing, as are
IndexedDB/SW retention and partition-specific second-Profile network assertions.
The browser crash test uses a global test crash only; production does not.

Cookie flush callbacks are drain completion, not durable-write success evidence
(the underlying API provides no success bit). Full browser crash loses the sole
in-memory snapshot and nonpersistent cookies: unrecoverable session-state loss,
not a successful full-fidelity recovery. No full-crash session guarantee.

Authoring evidence: scoped `git diff --check` and `node --check` on the network
smoke probe pass. Native compile/GN generation/unit/content/browser execution
were NOT run (no supported native build host; no build authorized). Cookie cases
use existing `//net:net_unittests`; registry and browser cases use the existing
Shuttle test source sets/aggregate wiring. Missing content/network test sources
in this pruned tree were not silently fabricated as runnable targets.

## Common coordinator continuation (2026-11-18) — NOT source-complete

New source draft:
- NetworkService owns token-indexed transactions. Begin captures immutable
  attempt-token/construction-generation membership and returns service/transaction
  tokens. Prepare retains weak contexts plus unbound workload receivers. Release
  validates every member, sets a common irreversible flag, then synchronously
  binds every ordinary ReceiverSet before returning Released. Repeated Release
  returns Released, never a false no-release certificate. Per-context Mojo release
  was removed. Contexts reject repeated preparation across transactions.
- Staged private primary receivers use ordinary Receiver (not DirectReceiver)
  and a message allowlist for restore acknowledgement, prepare and retirement.
  A raw-pipe watcher detects owner loss while asynchronous restoration has not
  yet bound that receiver; the existing lifetime pipe fences its destructor.
- Router captures fixed participants at retirement and owns their Profile
  keepalive/transaction/epoch/exact intent. Staging, compensation and teardown
  use that vector. ReleasePossible marks all snapshots before the single Release
  send; explicit rejection tears down before retry; lost reply quarantines and
  discards snapshots only after teardown, blocks load/replacement in this browser,
  and reports secret-free indeterminate failure. Released retires all snapshots
  in one browser task before admission. Normal post-ACK reconnect uses current
  store. Both retirement reconnect handlers and construction callbacks are
  suppressed/invalidated. Final load/persona failure completion now passes through
  fixed-vector destruction and conditional compensation before unlocking.
- Production browser test source now includes real service missing/duplicate
  membership rejection, stale/foreign preparation rejection, queued ordinary
  context calls before common Release and idempotent Release. Existing manual
  empty-snapshot test now uses the transaction API.

Precise remaining SOURCE/evidence work (do not mark implementation complete):
1. Audit all cancellation/reentrancy paths, especially simultaneous context and
   transaction disconnects, teardown already in progress, router destruction and
   Released ACK followed by stale registry ownership. Consolidate redundant
   compensation callbacks and prove no lifecycle lock/profile keepalive leak.
2. Finish explicit retiring/restoring/prepared/registryCommitted runtime phase
   transitions (enum exists; only release-boundary transitions are wired). Browser
   authoritative snapshots still live in StoragePartition, not in the router
   operation object; move/encapsulate ownership to meet the requested architecture.
3. Add externally counted HTTP requests proving zero before release AND all-bound
   before first dispatch; duplicate Prepare; registry-write failure with sibling
   destructor ordering; second retirement; stale epoch/dedupe; pre-release/during
   batch/post-request-before-reply crashes; lost reply after cookie mutation with
   no replay; second Profile network availability. The queued-call test is not
   equivalent to the required external-request proof. No fault seam was inserted
   in the critical bind loop (must remain callback-free).
4. Compile generated Mojo/C++ and run production tests on supported native host.
   Inspect new SimpleWatcher/raw-pipe destruction ordering and target dependencies
   there; authoring checks cannot establish native API/runtime correctness.

Browser crash still loses browser-only session snapshots; no fidelity claim.
No build/native execution, restart, commit, push, manager edits or settings edits.

## Exclusive implementation continuation — bounded, still not ship-ready

Source changed in this continuation:
- `NetworkContextHandoffState` encapsulates optional canonical snapshots and the
  irreversible release marker. Router operations strongly retain fixed member
  states; partitions reference the same generation. Failed pre-release retries
  retain that generation; a new successful-operation successor gets fresh state.
  State survives partition destruction while an operation/fence retains it.
  Browser shutdown still loses session snapshots; there is no restart fidelity
  guarantee. No provider serialization or cookie metadata conversion was added.
- Router transitions now centrally validate retiring/restoring/prepared/
  registryCommitted/releasePossible/released/aborting/failedIndeterminate.
  Begin, restoration barriers, retirement completion, disconnect handlers and
  compensation completion check operation ownership/epoch/intent. Released ACK
  disposes snapshots before stale-owner failure handling (never replays them).
- Router and partition aborts join existing teardown instead of CHECKing on a
  second disconnect. Cancellation during retirement waits for its destructor
  fences before staged teardown. Failure completion shares conditional registry
  compensation and joins callers; no raw registry pointer crosses teardown.
  Router destruction invalidates callbacks then tears down with retained operation
  keepalives; forced Profile shutdown abandons callbacks without replay/readiness.
- New UNRUN production-service/browser tests count HTTP requests (zero before
  Release, both complete after), reject duplicate context Prepare without replacing
  the original receiver, inject failed registry commit and immediately retry each
  sibling after public completion, exercise second retirement, join aborts, and
  prove a released request's cookie mutation followed by unfinalized browser state
  cannot replay an old snapshot. The latter deliberately withholds finalization;
  it is NOT an actual dropped-Mojo-reply/router failure-injection test. Another
  Profile performs real HTTP after the quarantined partition is fenced.
- Public router Mojo dependencies are exported in GN. Inspected shared message-id
  generator enum names, Receiver::SetFilter, SimpleWatcher Watch overload, Mojo
  signatures/test fake and wrapper factory constructors. This is not compilation.

### Small isolated SOURCE follow-up (superseded by bounded closure notes below)
1. `browser_profile_router.cc:StageProfileHandoff`,
   `OnProvisionedPersisted`, `OnPersonaHandoffStaged`, and loaded/persona outer
   continuations: finish stale-operation/epoch audit of callbacks outside the
   handoff helpers; prove no stale callback can fail/unlock a later lifecycle
   operation. Add production-router cancellation-during-retirement/restore,
   concurrent transaction+context disconnect and stale-epoch/dedupe assertions.
2. `network_service.cc:StorageHandoffTransaction::Release` and
   `browser_profile_lifecycle_browsertest.cc`: add a non-reentrant test observation
   proving every member is bound before the first external HTTP dispatch, without
   a callback/hook in the bind loop. Current external request counts prove only
   zero-before/common-release and eventual completion.
3. `browser_profile_router.cc:StageProfileHandoff` Release reply and its browser
   fixture: inject an actually lost reply after an HTTP Set-Cookie mutation,
   assert router indeterminate error/pending intent/no admission/no safe retry,
   both teardown fences and other-Profile HTTP. Current partition test exercises
   real quarantine but does not establish this full router transport ordering.
4. `ProfileStorageHandoff::Phase`: `kPrepared` currently denotes restored/staged
   readiness before registry commit; service member Prepare ACKs occur after that
   commit. Clarify/align this naming with the intended phase contract rather than
   claim it establishes the whole asynchronous failure matrix.

Authoring checks this continuation: scoped tracked `git diff --check` clean;
`git diff --no-index --check NUL` on router/header, registry/header and browser test
reported no whitespace diagnostics (exit 1 denotes new-file differences).
`node --check tools/shuttle/scripts/smoke-profile-network.mjs` passed.
Implementation/coverage checkboxes remain open. Native GN/Mojo/C++ compile and all
native execution remain UNRUN. No build, restart, commit, push, reset, stash,
manager edit or settings edit was performed.

## Four-item bounded source continuation (2026-11-18)

1. Outer load callbacks now carry epoch/exact intent through
   `BeginLoad`/`OnLoadingPersisted`/`OnProfileInitialized`,
   `OnInitializingPersisted` and `OnProvisionedPersisted`; obsolete replies do
   not release current runtimes or settle newer load waiters. Loaded recovery's
   staging continuation additionally captures the exact shared operation.
   `OnPersonaHandoffStaged` checks current epoch/intent/state and live operation
   ownership after the pre-commit seam before registry mutation. Existing weak
   router callbacks/destructor invalidation are retained. Written production tests:
   `RouterCancellationRetirementAndRestoreAreTerminalOnce` (retirement/restore
   cancellation, stale outer persistence failure, retry and terminal count), and
   the lost-reply case below (simultaneous context/transaction disconnect, rejected
   concurrent replacement and terminal-once). This is not native execution proof.
2. `StorageHandoffTransaction::Release` shares an actual bound receiver count;
   `NetworkContext::BindStorageHandoffWorkload` increments only after ReceiverSet
   insertion. `GetStorageHandoffBoundCountForTesting` is a read-only ordinary
   receiver request queued BEFORE each factory creation. The existing externally
   counted HTTP test now requires BOTH observations equal two, in addition to zero
   HTTP before Release and two completed requests. No arbitrary callback executes
   in the bind loop; both observations necessarily precede their HTTP dispatch.
3. `HoldReleaseReplyForTesting`/`DropReleaseReplyForTesting` on the real transaction
   hold the actual Release response only AFTER the entire synchronous bind batch,
   then close its transport. Router's narrow pre-send test seam enables this mode.
   `RouterLostReleaseReplyAfterCookieMutationFailsClosed` runs real HTTP Set-Cookie,
   reads the mutated cookie, drops the reply concurrently with context teardown,
   and asserts indeterminate failure, pending intent, closed admission, both
   destructor fences, absent/nonreplayable snapshots, denied retirement/load retry,
   duplicate replacement rejection, one terminal completion and other-Profile HTTP.
   The older partition-only withheld-finalization case remains separately labeled.
4. Phase validation now follows retiring -> restoring -> restoredStaged ->
   registryCommitted -> servicePrepared -> releasePossible -> released.
   `kRestoredStaged` means restoration ACKs before persistence; `kServicePrepared`
   is reached only after ALL service Prepare ACKs following persistence. The router
   lost-reply fixture explicitly observes releasePossible. Abort/quarantine ordering
   is unchanged; no protocol redesign or callback in the release bind loop.

Checks: network smoke `node --check` and scoped tracked `git diff --check` pass;
new router/browser-test no-index whitespace checks emit no diagnostics (exit 1
means new-file differences). C++ declarations/definitions, Mojo fake coverage and
existing browser-test GN/public Mojo dependencies inspected. Native GN generation,
Mojo/C++ compilation and all native tests remain UNRUN/unavailable; implementation
and coverage master checkboxes remain open, not a ship/completion claim. No build,
restart, commit, push, reset, stash, manager edit or settings edit performed.

## Post-review compile-blocker fix (2026-11-18)

Oracle review found two definite compile blockers from the newest Mojo insertions;
both are now fixed mechanically:

1. `services/network/public/mojom/network_service.mojom`: the existing
   `[ServiceSandbox=sandbox.mojom.Sandbox.kNetwork]` had drifted in front of the
   new `enum StorageHandoffReleaseResult` instead of `interface NetworkService`.
   The exact annotation now sits immediately before `interface NetworkService`
   (line 214), after the inserted enum and `StorageHandoffTransaction`.
2. `services/network/public/mojom/network_context.mojom`: the existing
   `[RequireContext=sandbox.mojom.Context.kPrivilegedUtility]` had drifted in
   front of the new `struct StorageHandoffMember` instead of
   `interface NetworkContext`. It now sits immediately before `interface
   NetworkContext` (line 1066), after the inserted struct.
3. `chrome/browser/shuttle/browser_profile_router.cc`,
   `OnInitializingPersisted`: the local `ManagedProfileIntent intent` shadowed
   the `std::string intent` parameter. The local, its four field assignments and
   its `VerifyManagedProfileIdentity` argument are renamed `identity`; every
   string `intent` use (epoch/intent stale checks) is unchanged.

Verification: scoped tracked `git diff --check` on the three files is clean
(exit 0). Both modified mojoms were parsed with the repo's existing
`mojo/public/tools/mojom/mojom_parser` front end (`mojom.parse.parser` +
`third_party/ply`; no dependency install, no build): `StorageHandoffReleaseResult`
and `StorageHandoffMember` carry no attributes, `NetworkService` carries exactly
`ServiceSandbox=sandbox.mojom.Sandbox.kNetwork`, and `NetworkContext` exactly
`RequireContext=sandbox.mojom.Context.kPrivilegedUtility`. This also matches
`mojo/public/tools/bindings/checks/mojom_attributes_check.py`, where both
attributes are interface-only allow-list entries. Non-blocking doc nit left
untouched: the pre-existing `NetworkService`/`NetworkContext` doc comments now
directly precede the inserted enum/struct rather than their interfaces.

Post-fix review found no further definite runtime P0/P1, but native compile and
all native execution remain UNRUN. Implementation and coverage checkboxes stay
unticked. No build, restart, commit, push, reset, stash, manager edit or settings
edit was performed.

## Bounded admission-crash source fix (2026-11-22) — native UNRUN

Deployed f64ceb49 aborted at the strict admission CHECK during staged restore.
The source now threads a move-only `NetworkContextHandoffProof` explicitly from
the router through StoragePartition, a distinct ContentBrowserClient authorized
configuration entry, Chrome's override, and the network service's internal helper.
Only the router can mint it. Weak operation ownership checks bind the exact
Profile, participant, epoch/intent, successful completed retirement, restoring
phase and retained authoritative snapshot identity. The capability captures the
partition attempt/new construction generation before use; moved-from, reused,
stale and cancelled authority fails closed. The service transaction token is
deliberately NOT required: activation mints that later.

The bare staging admission bypass/ambient staging boolean is removed. Ordinary
construction still awaits admission; factories retain their strict readiness
CHECKs. Staged configuration uses existing-only network/persona/proxy lookups and
returns false when any is missing. Recovery/replacement mutation and proxy
identity verification also use existing-only services. The ACK-only registry
fence is a distinct method and cannot construct a context. Private control,
unbound workload, lifetime fence, canonical cookies and common release stay intact.

Tests written/updated (NOT compiled/run): capability wrong participant, stale
attempt/generation, moved/reused authority and cancellation; absent existing
network service; empty-snapshot/crash retry and lost-release fixtures migrated to
router-minted authority; cached-factory production replacement AND pending-load
recovery now assert retained cookie in the first post-release HTTP response along
with persona headers. Existing quarantine/lost-release tests are retained.

GN audit: `//chrome/browser/shuttle:shuttle_profile_unittests` is an executable;
`//chrome/test:shuttle_profile_tests` currently builds ONLY that runner. Lifecycle
tests are in the disabled `//chrome/browser/shuttle:browser_tests` source_set,
NOT linked into an executable. `chrome/test/base` lacks the browser harness C++
sources/headers and chrome/test has an empty test_proto. Restoring a runnable
Chrome browser-test harness is not bounded target wiring, so no fake runnable
target was added. Production build target is `chrome`; native compile, GN gen,
unit/browser execution remain UNRUN. No build/deploy/restart authorized.

Available checks: tracked `git diff --check` clean. Native C++ syntax check is
unavailable (no clang/clang-format/gn on PATH and no supported generated build
graph); source API/invariant checks are not compilation. Remaining evidence:
missing persona/proxy-service fault variants; explicit stale epoch/foreign owner
variants; production-router zero-HTTP assertions at staging, registry persistence
and sibling prepare for BOTH recovery/replacement (existing service-level common
release counts do not prove every router boundary). Implementation/coverage
checkboxes remain open. Dirty cookie/persona work and .vscode preserved; no
commit/reset/stash/build/deploy/restart or Manager edits.

Final authoring checks also passed: `git diff --no-index --check NUL` for new
router/header, capability implementation, browser fixture and both status docs;
`node --check tools/shuttle/scripts/smoke-profile-network.mjs`; Python source
invariant assertions for absent staging boolean, explicit authorized entry,
precreation validation, successful-retirement gate and retained factory CHECKs.
These are source/JS checks only, NOT C++ syntax/native test evidence.

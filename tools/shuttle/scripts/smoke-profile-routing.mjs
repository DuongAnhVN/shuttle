// Scoped MCP routing (docs/PROFILES.md sections 3, 4, 9): node scripts/smoke-profile-routing.mjs <port> <user-data-dir>
// Run by scripts/smoke-all.sh --with profiles, on a launch with --shuttle-isolated-profiles. Exit 3 when not applicable.
//   selector errors (malformed / unknown / not ready), forged admin and forged scope, a selector grants no authority,
//   absent selector = the startup profile (never last-used), duplicate slugs across profiles, cross-profile sessions and
//   tabs, browser-wide UUIDv4 session ids, stale handles after stop/restart, concurrent lifecycle calls, scoped
//   set-fingerprint rules (busy / clear refused / not a cross-profile write).
import { check, finish } from './lib/smoke.mjs';
import { FORGED_KEY, admin, adminCall, call, forgedAdmin, makeProfile, persona, refused, requireIsolated, scopedSession } from './lib/profiles.mjs';

await requireIsolated();
const UUID4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const agentIds = async (opts) => { const r = await call('list-agents', {}, opts); return r.ok ? r.data.agents.map((a) => a.id) : null; };
const inA = (o = {}) => ({ headers: admin, profile: A.key, ...o });

// Fixtures: two throwaway profiles with the SAME workspace slug, and one stopped one.
const A = await makeProfile('shared');
const B = await makeProfile('shared', { persona: persona({ locale: 'de-DE', languages: ['de-DE', 'de'], timezone: 'Pacific/Auckland', hardwareConcurrency: 12 }) });
const S = await makeProfile('stopped-ws', { load: false });

// Selector errors, all before any session is touched.
refused('malformed selector is invalid_profile_key', await call('list-agents', {}, { headers: admin, profile: 'bp_nope' }), 'invalid_profile_key');
refused('non-string selector is invalid_profile_key', await call('list-agents', {}, { headers: admin, profile: 7 }), 'invalid_profile_key');
refused('null selector is invalid_profile_key (no fallback)', await call('list-agents', {}, { headers: admin, profile: null }), 'invalid_profile_key');
refused('traversal selector is invalid_profile_key', await call('list-agents', {}, { headers: admin, profile: '../../x' }), 'invalid_profile_key');
refused('unknown well-formed selector is profile_not_found', await call('list-agents', {}, { headers: admin, profile: FORGED_KEY }), 'profile_not_found');
refused('stopped profile selector is profile_not_ready', await call('list-agents', {}, { headers: admin, profile: S.key }), 'profile_not_ready');
// A selector grants no authority.
refused('anonymous selector is forbidden', await call('list-agents', {}, { profile: A.key }), 'forbidden');
refused('forged admin selector is forbidden', await call('list-agents', {}, { headers: forgedAdmin, profile: A.key }), 'forbidden');
refused('anonymous cannot reach an unknown profile either (forbidden, no existence oracle)', await call('list-agents', {}, { profile: FORGED_KEY }), 'forbidden');
refused('anonymous malformed selector never falls back', await call('list-agents', {}, { profile: 'bp_nope' }), 'invalid_profile_key');

// Duplicate slugs across profiles: the slug exists in each, independently.
const aIds = await agentIds(inA()), bIds = await agentIds({ headers: admin, profile: B.key });
check('profile A lists its own workspace agent', !!aIds?.includes('shared'), `${aIds}`);
check('profile B lists its own workspace agent with the same slug', !!bIds?.includes('shared'), `${bIds}`);
const mk = await adminCall('create-agent', { id: 'only-in-a', name: 'Only in A' }, { profile: A.key });
check('create-agent in A', mk.ok, `${mk.error?.code ?? mk.error?.message ?? ''}`);
check('the new agent is in A and not in B', (await agentIds(inA())).includes('only-in-a') && !(await agentIds({ headers: admin, profile: B.key })).includes('only-in-a'));
const dupB = await adminCall('create-agent', { id: 'only-in-a', name: 'Same slug in B' }, { profile: B.key });
check('the same slug is valid in B (agent lookups include the owning profile)', dupB.ok, `${dupB.error?.message ?? ''}`);
await adminCall('delete-agent', { id: 'only-in-a' }, { profile: B.key });

// Absent selector = the fixed startup profile, never the last-used one (B was touched last).
const startupIds = await agentIds({ headers: admin });
check('no selector never reaches an isolated profile (despite B being the last one touched)', !!startupIds && !startupIds.includes('shared') && !startupIds.includes('only-in-a') && !startupIds.includes('stopped-ws'), `${startupIds}`);
const smuggled = await call('list-agents', { _meta: { 'shuttle/profileId': A.key }, profile: A.key });
check('a selector smuggled into the arguments is not a selector (the startup profile answers)', smuggled.ok ? JSON.stringify(smuggled.data.agents.map((a) => a.id)) === JSON.stringify(startupIds) : true, `${smuggled.error?.code ?? 'startup'}`);
const anonIds = await agentIds({});
check('anonymous, no selector: the startup profile is still served (legacy)', !!anonIds && JSON.stringify(anonIds) === JSON.stringify(startupIds));

// Sessions: browser-wide UUIDv4, owner-bound.
const sa = await scopedSession(A);
const sb = await scopedSession(B);
check('session ids are UUIDv4', UUID4.test(sa.sessionId) && UUID4.test(sb.sessionId) && sa.sessionId !== sb.sessionId, `${sa.sessionId.length}`);
check('endpoints are not the admin token (the grant is per session and per endpoint)', !JSON.stringify([sa.cdpEndpoint, sa.cdpUrl]).includes(admin['X-Shuttle-Admin-Token']));
check('two sessions get two different grants', new URL(sa.cdpEndpoint).searchParams.get('grant') !== new URL(sb.cdpEndpoint).searchParams.get('grant'));

// Cross-profile: every id is a locator, the scope decides.
for (const [label, name, a] of [
  ['allocate-tab', 'allocate-tab', { sessionId: sb.sessionId }],
  ['close-session', 'close-session', { sessionId: sb.sessionId }],
  ['list-tabs', 'list-tabs', { sessionId: sb.sessionId }],
]) {
  const r = await adminCall(name, a, { profile: A.key });
  check(`${label} of B's session under A's scope is refused`, !r.ok, `${r.error?.code ?? r.error?.message ?? 'ALLOWED'}`.slice(0, 80));
}
const nav = await adminCall('navigate-tab', { targetId: sb.targetId, url: 'about:blank' }, { profile: A.key });
check("navigate-tab of B's tab under A's scope is refused", !nav.ok, `${nav.error?.message ?? 'ALLOWED'}`.slice(0, 80));
const nav0 = await adminCall('navigate-tab', { targetId: sb.targetId, url: 'about:blank' });
check("navigate-tab of B's tab with no selector (startup profile) is refused", !nav0.ok, `${nav0.error?.message ?? 'ALLOWED'}`.slice(0, 80));
const navAnon = await call('navigate-tab', { targetId: sb.targetId, url: 'about:blank' });
check("navigate-tab of B's tab by an anonymous caller is refused", !navAnon.ok);
const own = await adminCall('navigate-tab', { targetId: sb.targetId, url: 'about:blank' }, { profile: B.key });
check("navigate-tab of B's tab under B's scope works", own.ok, `${own.error?.message ?? ''}`.slice(0, 80));
const closeBad = await adminCall('close-session', { sessionId: 'not-a-uuid' }, { profile: A.key });
check('close-session of a garbage id is refused', !closeBad.ok);

// Scoped set-fingerprint: refused while a session exists (busy), never cleared, never crossing profiles.
const fpA = persona({ locale: 'it-IT', languages: ['it-IT', 'it'], timezone: 'Europe/Rome' });
refused('set-fingerprint with live sessions is profile_busy', await adminCall('set-fingerprint', { fingerprint: fpA }, { profile: A.key }), 'profile_busy');
refused('clearing a managed profile persona is persona_invalid', await adminCall('set-fingerprint', { fingerprint: null }, { profile: B.key }), 'persona_invalid');
refused('anonymous set-fingerprint with a selector is forbidden', await call('set-fingerprint', { fingerprint: fpA }, { profile: A.key }), 'forbidden');
refused('forged admin set-fingerprint is forbidden', await call('set-fingerprint', { fingerprint: fpA }, { headers: forgedAdmin, profile: A.key }), 'forbidden');
await adminCall('close-session', { sessionId: sa.sessionId }, { profile: A.key });
// Even drained, a READY profile never takes a live persona change: stopped is the only accepted state.
refused('set-fingerprint on a ready (drained) profile is profile_busy', await adminCall('set-fingerprint', { fingerprint: fpA }, { profile: A.key }), 'profile_busy');
const got = await adminCall('get-fingerprint', {}, { profile: B.key });
const bBefore = JSON.stringify(got.data?.fingerprint ?? got.error);

// Stale handles: stopping A closes its sessions; reloading never revives them.
const s2 = await scopedSession(A);
const tabsBefore = await adminCall('list-tabs', { sessionId: s2.sessionId }, { profile: A.key });
check('A has a live session before stop', tabsBefore.ok);
const stopA = await adminCall('stop-browser-profile', { coreProfileKey: A.key });
check('stop A drains it', stopA.ok && stopA.data.profile.lifecycle === 'stopped', `${stopA.error?.code ?? ''}`);
refused('stopped A is profile_not_ready for scoped calls', await adminCall('list-agents', {}, { profile: A.key }), 'profile_not_ready');

// Persona replacement while STOPPED (contract section 7): the only tool a stopped profile accepts. It does not load
// the profile, moves the lifecycle epoch, never clears cookies, never touches B.
const set = await adminCall('set-fingerprint', { fingerprint: fpA }, { profile: A.key });
const cold = await adminCall('set-fingerprint', { fingerprint: fpA }, { profile: S.key });
check('an unloaded profile queues the replacement for its next load',
  cold.ok && cold.data.applied === false && cold.data.pendingRestart === true);
check('the stopped profile reports the durable replacement state',
  set.ok && set.data.applied === stopA.data.profile.loaded &&
  set.data.pendingRestart === !stopA.data.profile.loaded);
check('set-fingerprint on a stopped profile works', set.ok && set.data.fingerprint?.timezone === 'Europe/Rome', `${set.error?.code ?? set.error?.message ?? ''}`);
check('set-fingerprint answer carries no seed', !/seed/i.test(JSON.stringify(set.data ?? '')));
const recA = (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === A.key);
check('the profile stays stopped and its epoch moved', recA.lifecycle === 'stopped' && recA.lifecycleEpoch > stopA.data.profile.lifecycleEpoch, `${recA.lifecycle} ${recA.lifecycleEpoch}/${stopA.data.profile.lifecycleEpoch}`);
refused('a persona without preset is refused (full persona only)', await adminCall('set-fingerprint', { fingerprint: { timezone: 'Europe/Rome' } }, { profile: A.key }), 'persona_invalid');
refused('an unknown time zone is refused', await adminCall('set-fingerprint', { fingerprint: persona({ timezone: 'Mars/Olympus_Mons' }) }, { profile: A.key }), 'persona_invalid');
refused('clearing a stopped managed persona is persona_invalid', await adminCall('set-fingerprint', { fingerprint: null }, { profile: A.key }), 'persona_invalid');
refused('extra arguments are refused', await adminCall('set-fingerprint', { fingerprint: fpA, sessionId: 'x' }, { profile: A.key }), 'profile_failed');
check("A's persona change did not touch B's persona", JSON.stringify((await adminCall('get-fingerprint', {}, { profile: B.key })).data?.fingerprint) === bBefore);
check("B kept working while A stopped", (await adminCall('list-tabs', { sessionId: sb.sessionId }, { profile: B.key })).ok);
check('B did not change epoch', (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === B.key)?.lifecycle === 'ready');
const reloadA = await adminCall('load-browser-profile', { coreProfileKey: A.key });
check('A reloads at a new lifecycle epoch', reloadA.ok && reloadA.data.profile.lifecycleEpoch > stopA.data.profile.lifecycleEpoch, `${reloadA.error?.code ?? ''}`);
const stale = await adminCall('list-tabs', { sessionId: s2.sessionId }, { profile: A.key });
check("A's pre-stop session is gone after restart (stale handle)", !stale.ok, `${stale.error?.message ?? 'REVIVED'}`.slice(0, 80));
const activeAfter = (await adminCall('get-fingerprint', {}, { profile: A.key })).data;
check('the loaded profile has no pending replacement', activeAfter?.pendingRestart === false);
const fpAfter = activeAfter?.fingerprint;
check('the replaced persona is what the reloaded profile presents', fpAfter?.timezone === 'Europe/Rome' && fpAfter?.locale === 'it-IT', JSON.stringify([fpAfter?.timezone, fpAfter?.locale]));

// Concurrent lifecycle calls on one key are serialized, never a torn state.
const [x1, x2, x3, x4] = await Promise.all([
  adminCall('stop-browser-profile', { coreProfileKey: A.key }), adminCall('load-browser-profile', { coreProfileKey: A.key }),
  adminCall('stop-browser-profile', { coreProfileKey: A.key }), adminCall('load-browser-profile', { coreProfileKey: A.key })]);
const end = (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === A.key);
check('concurrent stop/load calls end in a settled state (ready or stopped)', ['ready', 'stopped'].includes(end.lifecycle), `${end.lifecycle}`);
check('every concurrent call answered ok or a lifecycle error code (no crash, no transport error)', [x1, x2, x3, x4].every((r) => r.ok || ['profile_busy', 'profile_not_ready', 'drain_timeout', 'profile_failed'].includes(r.error?.code)),
  [x1, x2, x3, x4].map((r) => r.error?.code ?? 'ok').join(','));

// Persona replacement is serialized per profile: concurrent replacements and a load never tear it.
await adminCall('stop-browser-profile', { coreProfileKey: A.key });
const pA = persona({ locale: 'es-ES', languages: ['es-ES', 'es'], timezone: 'Europe/Madrid' });
const pB = persona({ locale: 'pt-BR', languages: ['pt-BR', 'pt'], timezone: 'America/Sao_Paulo' });
const before = (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === A.key);
const [q1, q2, q3] = await Promise.all([
  adminCall('set-fingerprint', { fingerprint: pA }, { profile: A.key }),
  adminCall('set-fingerprint', { fingerprint: pB }, { profile: A.key }),
  adminCall('load-browser-profile', { coreProfileKey: A.key })]);
const okOrBusy = (r) => r.ok || r.error?.code === 'profile_busy';
check('concurrent replace/replace/load: every answer is ok or profile_busy', [q1, q2, q3].every(okOrBusy), [q1, q2, q3].map((r) => r.error?.code ?? 'ok').join(','));
const after = (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === A.key);
check('the epoch only moved forward and the state settled (stopped or ready)', after.lifecycleEpoch >= before.lifecycleEpoch && ['stopped', 'ready'].includes(after.lifecycle), `${before.lifecycleEpoch}->${after.lifecycleEpoch} ${after.lifecycle}`);
if (after.lifecycle === 'stopped') await adminCall('load-browser-profile', { coreProfileKey: A.key });
const finalFp = (await adminCall('get-fingerprint', {}, { profile: A.key })).data?.fingerprint;
const winners = [q1.ok && pA, q2.ok && pB].filter(Boolean).map((p) => p.timezone);
check('the loaded persona is exactly one of the accepted replacements (never a mix)', !winners.length || winners.includes(finalFp?.timezone), `${finalFp?.timezone} in ${winners}`);

finish();

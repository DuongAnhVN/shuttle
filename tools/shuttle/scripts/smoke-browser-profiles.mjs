// Browser-level profile tools (docs/PROFILES.md sections 2, 3, 5, 9): node scripts/smoke-browser-profiles.mjs <port> <user-data-dir>
// Run by scripts/smoke-all.sh --with profiles, on a launch with --shuttle-isolated-profiles. Exit 3 when not applicable.
//   capabilities (public) and tool visibility; admin-only; forged admin; create/list/load/stop/delete lifecycle;
//   idempotency + request_conflict; unknown properties; traversal and unknown keys; duplicate slugs across profiles;
//   persona_invalid; the startup profile can't be deleted; delete needs stopped; no paths/seeds/tokens in any result.
import { randomUUID } from 'node:crypto';
import { check, finish } from './lib/smoke.mjs';
import { KEY_RE, FORGED_KEY, admin, adminCall, call, forgedAdmin, makeProfile, noSecrets, persona, refused, requireIsolated, toolNames } from './lib/profiles.mjs';

const caps = await requireIsolated();
check('capabilities shape', caps.capabilitiesVersion === 1 && caps.profileScopeMetadata === 'shuttle/profileId' && caps.bindingVersion === 1 &&
  typeof caps.browserEpoch === 'string' && caps.browserEpoch.length > 0, JSON.stringify({ ...caps, browserEpoch: '<opaque>' }));

// Visibility: browser-profile tools are absent from a non-admin listing; get-capabilities is public.
const BROWSER_ADMIN = ['create-browser-profile', 'list-browser-profiles', 'load-browser-profile', 'stop-browser-profile', 'delete-browser-profile'];
const anon = await toolNames();
check('anonymous tools/list has get-capabilities and no admin tool', anon.includes('get-capabilities') && BROWSER_ADMIN.every((n) => !anon.includes(n)), anon.filter((n) => /browser/.test(n)).join(','));
const forgedList = await toolNames(forgedAdmin);
check('forged admin token sees no admin tool', BROWSER_ADMIN.every((n) => !forgedList.includes(n)));
const adminList = await toolNames(admin);
check('admin tools/list has every browser-profile tool', BROWSER_ADMIN.every((n) => adminList.includes(n)));

// Admin-only: anonymous and forged are `forbidden`; a caller-supplied role field is never trusted.
const listArgs = {};
refused('anonymous list-browser-profiles is forbidden', await call('list-browser-profiles', listArgs), 'forbidden');
refused('forged admin token list-browser-profiles is forbidden', await call('list-browser-profiles', listArgs, { headers: forgedAdmin }), 'forbidden');
refused('role fields in arguments grant nothing', await call('list-browser-profiles', { admin: true, role: 'admin' }, { headers: forgedAdmin }), 'forbidden');
refused('role fields in _meta grant nothing', await call('list-browser-profiles', {}, { meta: { role: 'admin', admin: true } }), 'forbidden');
refused('anonymous create-browser-profile is forbidden', await call('create-browser-profile', { requestId: randomUUID(), workspaceAgent: 'x', persona: persona() }), 'forbidden');
refused('anonymous delete-browser-profile is forbidden', await call('delete-browser-profile', { coreProfileKey: FORGED_KEY }), 'forbidden');

// A selector on a browser-level tool is rejected, even for admin; malformed beats everything.
refused('selector on a browser-level tool is rejected (admin)', await adminCall('list-browser-profiles', {}, { profile: FORGED_KEY }), 'invalid_profile_key');
refused('malformed selector on get-capabilities', await call('get-capabilities', {}, { profile: 'bp_nope' }), 'invalid_profile_key');

// Lifecycle.
const reqId = randomUUID();
const args = { requestId: reqId, workspaceAgent: 'smoke-prof-a', persona: persona() };
const a = await makeProfile('smoke-prof-a', { load: false });
check('created profile has a core-issued key and is stopped', KEY_RE.test(a.key) && a.record.lifecycle === 'stopped', `${a.record.lifecycle}`);
check('record carries only the contract fields', Object.keys(a.record).every((k) => ['coreProfileKey', 'workspaceAgent', 'lifecycleEpoch', 'browserEpoch', 'lifecycle', 'loaded', 'deletionPending', 'personaSummary', 'lastErrorCode'].includes(k)),
  Object.keys(a.record).join(','));
noSecrets('create result has no path, seed or token', a.record);

const again = await adminCall('create-browser-profile', { requestId: a.requestId, workspaceAgent: 'smoke-prof-a', persona: persona() });
check('create is idempotent by requestId (same key, no new epoch)', again.ok && again.data.profile.coreProfileKey === a.key && again.data.profile.lifecycleEpoch === a.record.lifecycleEpoch, JSON.stringify(again.error ?? ''));
refused('same requestId with a different persona is request_conflict', await adminCall('create-browser-profile', { requestId: a.requestId, workspaceAgent: 'smoke-prof-a', persona: persona({ locale: 'de-DE', languages: ['de-DE', 'de'] }) }), 'request_conflict');
refused('same requestId with a different workspace is request_conflict', await adminCall('create-browser-profile', { requestId: a.requestId, workspaceAgent: 'smoke-prof-z', persona: persona() }), 'request_conflict');

// Unknown properties and invalid arguments never create anything.
const before = (await adminCall('list-browser-profiles')).data.profiles.length;
for (const [label, bad] of [
  ['unknown property', { ...args, requestId: randomUUID(), extra: 1 }],
  ['non-uuid requestId', { ...args, requestId: 'not-a-uuid' }],
  ['bad workspace slug', { ...args, requestId: randomUUID(), workspaceAgent: '../x' }],
  ['reserved tmp- workspace', { ...args, requestId: randomUUID(), workspaceAgent: 'tmp-x' }],
  ['missing persona', { requestId: randomUUID(), workspaceAgent: 'smoke-prof-b' }],
]) {
  const r = await adminCall('create-browser-profile', bad);
  check(`create refuses ${label}`, !r.ok && !!r.error?.code, `${r.error?.code}`);
}
for (const [label, bad] of [
  ['arbitrary user agent', { userAgent: 'x' }], ['seed', { seed: 1 }], ['platform', { platform: 'x' }], ['client hints', { clientHints: {} }],
  ['unknown timezone', { timezone: 'Mars/Olympus_Mons' }], ['unknown preset', { preset: 'beos-chrome' }],
]) {
  const r = await adminCall('create-browser-profile', { requestId: randomUUID(), workspaceAgent: 'smoke-prof-bad', persona: persona(bad) });
  check(`persona ${label} is refused (persona_invalid or profile_failed)`, !r.ok && ['persona_invalid', 'profile_failed'].includes(r.error?.code), `${r.error?.code}`);
}
check('refused creates left no profile behind', (await adminCall('list-browser-profiles')).data.profiles.length === before);

// Keys: traversal and unknown.
for (const bad of ['../x', '..\\x', 'bp_../..', 'bp_' + 'A'.repeat(32), 'x', '', 'bp_0123', `${a.key}/..`]) {
  const r = await adminCall('load-browser-profile', { coreProfileKey: bad });
  check(`load refuses key ${JSON.stringify(bad).slice(0, 30)}`, !r.ok && ['invalid_profile_key', 'profile_failed'].includes(r.error?.code), `${r.error?.code}`);
}
refused('load of an unknown well-formed key is profile_not_found', await adminCall('load-browser-profile', { coreProfileKey: FORGED_KEY }), 'profile_not_found');
refused('stop of an unknown key is profile_not_found', await adminCall('stop-browser-profile', { coreProfileKey: FORGED_KEY }), 'profile_not_found');
refused('delete of an unknown key is profile_not_found', await adminCall('delete-browser-profile', { coreProfileKey: FORGED_KEY }), 'profile_not_found');
refused('a profile tool refuses unknown properties', await adminCall('load-browser-profile', { coreProfileKey: a.key, path: '/' }), 'profile_failed');

// Duplicate slugs across profiles are valid and independent.
const b = await makeProfile('smoke-prof-a', { load: false });
check('the same workspace slug in two profiles gives two profiles', b.key !== a.key && b.workspaceAgent === a.workspaceAgent);

// load -> ready, concurrent loads share one init, stop drains, delete needs stopped.
const [l1, l2, l3] = await Promise.all([1, 2, 3].map(() => adminCall('load-browser-profile', { coreProfileKey: a.key })));
check('concurrent loads all succeed with the same epoch and end ready', [l1, l2, l3].every((l) => l.ok && l.data.profile.lifecycle === 'ready' && l.data.profile.lifecycleEpoch === l1.data.profile.lifecycleEpoch),
  [l1, l2, l3].map((l) => l.error?.code ?? l.data.profile.lifecycle).join(','));
refused('delete of a ready profile is profile_busy', await adminCall('delete-browser-profile', { coreProfileKey: a.key }), 'profile_busy');
const listed = (await adminCall('list-browser-profiles')).data;
check('list shows both profiles and the browserEpoch', listed.browserEpoch === caps.browserEpoch && [a.key, b.key].every((k) => listed.profiles.some((p) => p.coreProfileKey === k)));
noSecrets('list result has no path, seed or token', listed);
check('personaSummary of a ready profile has no seed', !JSON.stringify(listed.profiles.find((p) => p.coreProfileKey === a.key).personaSummary).match(/seed/i));
const st = await adminCall('stop-browser-profile', { coreProfileKey: a.key });
check('stop reports stopped with a higher lifecycle epoch', st.ok && st.data.profile.lifecycle === 'stopped' && st.data.profile.lifecycleEpoch > l1.data.profile.lifecycleEpoch, `${st.error?.code ?? st.data.profile.lifecycle}`);
const st2 = await adminCall('stop-browser-profile', { coreProfileKey: a.key });
check('stop of a stopped profile is idempotent (no new epoch)', st2.ok && st2.data.profile.lifecycleEpoch === st.data.profile.lifecycleEpoch);
check('stopping a profile left the other one untouched', (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === b.key)?.lifecycle === 'stopped');

// The startup profile is not a managed profile: it has no key to delete.
const startup = (await adminCall('list-browser-profiles')).data.profiles;
check('the startup profile is not listed as an isolated profile', startup.every((p) => KEY_RE.test(p.coreProfileKey)));
const del = await adminCall('delete-browser-profile', { coreProfileKey: a.key });
check('delete of a stopped, idle profile succeeds', del.ok, `${del.error?.code ?? 'ok'}`);
const after = (await adminCall('list-browser-profiles')).data.profiles.find((p) => p.coreProfileKey === a.key);
check('a deleted profile is gone or only a tombstone', !after || after.deletionPending === true || after.lifecycle === 'deleted', `${after?.lifecycle}`);
const reload = await adminCall('load-browser-profile', { coreProfileKey: a.key });
check('load of a deleted profile fails (no resurrection)', !reload.ok && ['profile_not_found', 'profile_failed'].includes(reload.error?.code), `${reload.error?.code ?? 'LOADED'}`);

finish();

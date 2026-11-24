// CDP ownership and connection decisions (docs/PROFILES.md section 4, 9): node scripts/smoke-profile-cdp.mjs <port> <user-data-dir>
// Run by scripts/smoke-all.sh --with profiles, on a launch with --shuttle-isolated-profiles. Exit 3 when not applicable.
//   raw browser socket and browser-global /json/* need admin; session/page sockets: unknown = 404, no/other grant = 403,
//   the grant is per session and never the admin token; a scoped socket sees only its own targets, no foreign
//   BrowserContexts, no profile creation, no cross-owner Storage; popups stay inside the owner; stop revokes the sockets
//   and the stale endpoints answer 404; legacy scoped sockets also reject tunnelling.
import { check, cdp, finish, httpStatus, json, sleep, wsStatus, base } from './lib/smoke.mjs';
import { admin, adminCall, forgedAdmin, makeProfile, persona, requireIsolated, scopedSession } from './lib/profiles.mjs';

await requireIsolated();
const A = await makeProfile('cdp-a');
const B = await makeProfile('cdp-b', { persona: persona({ locale: 'de-DE', languages: ['de-DE', 'de'], timezone: 'Pacific/Auckland' }) });
const sa = await scopedSession(A);
const sb = await scopedSession(B);
const grantOf = (url) => new URL(url).searchParams.get('grant');
const withGrant = (url, g) => { const u = new URL(url); u.searchParams.delete('grant'); if (g) u.searchParams.set('grant', g); return u.toString(); };
const ws = (path, query = '') => `ws://${new URL(base).host}${path}${query}`;

// 1. Browser-global HTTP and the raw browser socket need admin.
for (const path of ['/json/version', '/json', '/json/list', '/json/protocol']) {
  check(`${path} without admin is 403`, (await httpStatus(path)) === 403, `${await httpStatus(path)}`);
  check(`${path} with a forged admin token is 403`, (await httpStatus(path, forgedAdmin)) === 403);
}
check('the discovery page / without admin is 403', (await httpStatus('/')) === 403, `${await httpStatus('/')}`);
check('the discovery page / with admin is served', (await httpStatus('/', admin)) === 200);
check('/json/version with admin is 200', (await httpStatus('/json/version', admin)) === 200);
check('/json/new without admin is refused', [403, 404, 405].includes(await httpStatus('/json/new?about:blank')), `${await httpStatus('/json/new?about:blank')}`);
check('the admin token in the URL is not accepted', (await httpStatus(`/json/version?token=${admin['X-Shuttle-Admin-Token']}`)) === 403 &&
  (await httpStatus(`/json/version?admin=${admin['X-Shuttle-Admin-Token']}`)) === 403);
const version = await json('/json/version', admin);
check('/json/version advertises a browser socket', /^ws:/.test(version.webSocketDebuggerUrl ?? ''));
check('raw browser socket without admin is 403', (await wsStatus(version.webSocketDebuggerUrl)) === 403);
check('raw browser socket with a forged admin token is 403', (await wsStatus(version.webSocketDebuggerUrl, forgedAdmin)) === 403);
check('a session grant never opens the raw browser socket', (await wsStatus(withGrant(version.webSocketDebuggerUrl, grantOf(sa.cdpEndpoint)))) === 403);
const rawAdmin = await cdp(version.webSocketDebuggerUrl, admin);
const targets = (await rawAdmin.send('Target.getTargets')).result?.targetInfos ?? [];
check('the admin browser socket is unfiltered (sees both profiles\' tabs)', [sa.targetId, sb.targetId].every((id) => targets.some((t) => t.targetId === id)));
rawAdmin.close();

// 2. Session sockets: grant required, bound to the session, 404 for unknown.
check('session socket with its grant opens', (await wsStatus(sa.cdpEndpoint)) === 101);
check('session socket without a grant is 403', (await wsStatus(withGrant(sa.cdpEndpoint, null))) === 403);
check('session socket with a wrong grant is 403', (await wsStatus(withGrant(sa.cdpEndpoint, 'A'.repeat(43)))) === 403);
check("session socket with another session's grant is 403", (await wsStatus(withGrant(sa.cdpEndpoint, grantOf(sb.cdpEndpoint)))) === 403);
check('session socket with a forged admin token and no grant is 403', (await wsStatus(withGrant(sa.cdpEndpoint, null), forgedAdmin)) === 403);
check('unknown session id is 404', (await wsStatus(ws('/devtools/session/00000000-0000-4000-8000-000000000000', `?grant=${grantOf(sa.cdpEndpoint)}`))) === 404);
check('traversal / garbage session ids are 404', (await wsStatus(ws('/devtools/session/..%2Fbrowser', `?grant=${grantOf(sa.cdpEndpoint)}`))) !== 101 &&
  (await wsStatus(ws('/devtools/session/x', `?grant=${grantOf(sa.cdpEndpoint)}`))) === 404);
check('the grant is not the admin token', grantOf(sa.cdpEndpoint) !== admin['X-Shuttle-Admin-Token']);
check('admin on a session socket still gets the scoped filter (see below)', (await wsStatus(withGrant(sa.cdpEndpoint, null), admin)) === 101);

// 3. Page sockets: the tab's own grant opens it, a foreign or missing one does not.
check('page socket with its grant opens', (await wsStatus(sa.cdpUrl)) === 101);
check('page socket without a grant is 403', (await wsStatus(withGrant(sa.cdpUrl, null))) === 403);
check("page socket of B's tab with A's grant is 403", (await wsStatus(withGrant(sb.cdpUrl, grantOf(sa.cdpEndpoint)))) === 403);
check('page socket of an unknown target is 404', (await wsStatus(ws('/devtools/page/' + 'F'.repeat(32), `?grant=${grantOf(sa.cdpEndpoint)}`))) === 404);

// 4. A scoped session socket sees only its own owner's targets and contexts.
let ca = await cdp(sa.cdpEndpoint);
const cb = await cdp(sb.cdpEndpoint);
const seen = async (c) => ((await c.send('Target.getTargets')).result?.targetInfos ?? []).map((t) => t.targetId);
const aSees = await seen(ca), bSees = await seen(cb);
check('A sees its tab and not B\'s', aSees.includes(sa.targetId) && !aSees.includes(sb.targetId), `${aSees.length}`);
check('B sees its tab and not A\'s', bSees.includes(sb.targetId) && !bSees.includes(sa.targetId), `${bSees.length}`);
const ctxs = await ca.send('Target.getBrowserContexts');
check('Target.getBrowserContexts reveals no foreign contexts', !ctxs.error ? (ctxs.result?.browserContextIds ?? []).length === 0 : true, JSON.stringify(ctxs.error ?? ''));
const refusedMsg = (r) => !!r.error;
check('Target.attachToTarget on a foreign target is refused', refusedMsg(await ca.send('Target.attachToTarget', { targetId: sb.targetId, flatten: true })));
check('Target.activateTarget / closeTarget on a foreign target is refused', refusedMsg(await ca.send('Target.activateTarget', { targetId: sb.targetId })) &&
  refusedMsg(await ca.send('Target.closeTarget', { targetId: sb.targetId })));
check('Target.createBrowserContext (profile creation) is refused', refusedMsg(await ca.send('Target.createBrowserContext')));
check('Target.disposeBrowserContext is refused', refusedMsg(await ca.send('Target.disposeBrowserContext', { browserContextId: 'x' })));
check('Target.createTarget in a foreign browserContextId is refused', refusedMsg(await ca.send('Target.createTarget', { url: 'about:blank', browserContextId: 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' })));
check('Target.setAutoAttach / setDiscoverTargets at browser level stay scoped (no error, no foreign targets)',
  !(await ca.send('Target.setDiscoverTargets', { discover: true })).error);
await sleep(300);
const foreignEvents = ca.events.filter((e) => /^Target\./.test(e.method ?? '') && JSON.stringify(e.params ?? {}).includes(sb.targetId));
check('discovery events never mention the other owner\'s targets', foreignEvents.length === 0, `${foreignEvents.length}`);
// Cross-owner storage access.
for (const [label, method, params] of [
  // Storage.get/set/clearCookies without a browserContextId are answered with the session's OWN agent partition (probe-profile-cookies).
  ['Storage.getStorageKeyForFrame', 'Storage.getStorageKeyForFrame', { frameId: sb.targetId }],
  ['Storage.clearDataForOrigin', 'Storage.clearDataForOrigin', { origin: 'http://127.0.0.1', storageTypes: 'all' }],
  ['Browser.getBrowserCommandLine', 'Browser.getBrowserCommandLine', {}],
  ['Browser.setPermission without a browserContextId', 'Browser.setPermission', { permission: { name: 'geolocation' }, setting: 'granted' }],
  ['Browser.setDownloadBehavior without a browserContextId', 'Browser.setDownloadBehavior', { behavior: 'allow' }],
]) check(`${label} is refused on a scoped socket`, refusedMsg(await ca.send(method, params)), `${(await ca.send(method, params)).error?.message ?? 'ALLOWED'}`.slice(0, 60));
const foreignCookies = await ca.send('Storage.getCookies', { browserContextId: 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' });
check('Storage.getCookies for a foreign browserContextId is refused', refusedMsg(foreignCookies));
ca.ws.send('{not json'); ca.ws.send(JSON.stringify({ id: 'x', method: 7 })); await sleep(300);
check('malformed CDP messages are not forwarded: the browser keeps answering other sockets', (await httpStatus('/json/version', admin)) === 200);
if (ca.ws.readyState !== WebSocket.OPEN) ca = await cdp(sa.cdpEndpoint);  // a filter may legitimately close on garbage

// 4b. Nested CDP and transport nesting: the same policy on the session socket, the page socket and flattened child sessions.
const flat = (r) => r.error ? r.error.message : 'ALLOWED';
check('Target.attachToTarget without flatten is refused', refusedMsg(await ca.send('Target.attachToTarget', { targetId: sa.targetId })), flat(await ca.send('Target.attachToTarget', { targetId: sa.targetId })).slice(0, 60));
check('Target.attachToTarget with flatten:false is refused', refusedMsg(await ca.send('Target.attachToTarget', { targetId: sa.targetId, flatten: false })));
check('Target.setAutoAttach with flatten:false is refused', refusedMsg(await ca.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: false })));
check('Target.attachToBrowserTarget is refused', refusedMsg(await ca.send('Target.attachToBrowserTarget')));
for (const inner of [{ id: 1, method: 'Browser.close' }, { id: 2, method: 'Storage.getCookies' }, { id: 3, method: 'Target.createBrowserContext' }]) {
  check(`Target.sendMessageToTarget carrying ${inner.method} is refused`, refusedMsg(await ca.send('Target.sendMessageToTarget', { targetId: sa.targetId, message: JSON.stringify(inner) })));
}
check('the nested Browser.close did not run: the browser, A and B are alive', (await httpStatus('/json/version', admin)) === 200 && (await wsStatus(sa.cdpEndpoint)) === 101 && (await wsStatus(sb.cdpEndpoint)) === 101);
const attached = await ca.send('Target.attachToTarget', { targetId: sa.targetId, flatten: true });
const child = attached.result?.sessionId;
check('a flattened attach to an own target works', !!child, flat(attached));
const SENTINEL = 'startup_sentinel_' + Math.random().toString(36).slice(2);
const rawAdmin2 = await cdp(version.webSocketDebuggerUrl, admin);
await rawAdmin2.send('Storage.setCookies', { cookies: [{ name: SENTINEL, value: 's', url: 'http://127.0.0.1/', expires: Math.floor(Date.now() / 1000) + 600 }] });
const cookieNames = (r) => (r.result?.cookies ?? []).map((c) => c.name);
const pp = await cdp(sa.cdpUrl);
if (child) {
  for (const [method, params] of [
    ['Storage.getCookies', {}],
    ['Storage.setCookies', { cookies: [{ name: SENTINEL, value: 'overwritten', url: 'http://127.0.0.1/' }] }],
    ['Storage.clearCookies', {}],
  ]) check(`${method} on a flattened child is refused`, refusedMsg(await ca.send(method, params, child)));
  check('Browser.setPermission on a child session is refused', refusedMsg(await ca.send('Browser.setPermission', { permission: { name: 'geolocation' }, setting: 'granted' }, child)));
  check('Storage.clearDataForOrigin on a child session is refused', refusedMsg(await ca.send('Storage.clearDataForOrigin', { origin: 'http://127.0.0.1', storageTypes: 'all' }, child)));
  check('Target.sendMessageToTarget on a child session is refused', refusedMsg(await ca.send('Target.sendMessageToTarget', { sessionId: child, message: JSON.stringify({ id: 9, method: 'Browser.close' }) }, child)));
  check('a foreign browserContextId on a child session is refused', refusedMsg(await ca.send('Storage.getCookies', { browserContextId: 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' }, child)));
}
const viaPage = await pp.send('Storage.getCookies');
check('Storage.getCookies on the PAGE socket is refused', refusedMsg(viaPage), flat(viaPage));
const viaSession = await ca.send('Storage.getCookies');
check('root Storage.getCookies still uses the owner partition', !viaSession.error && !cookieNames(viaSession).includes(SENTINEL));
check('Storage.setCookies on the page socket is refused', refusedMsg(await pp.send('Storage.setCookies', {
  cookies: [{ name: SENTINEL, value: 'overwritten', url: 'http://127.0.0.1/' }],
})));
check('Storage.clearCookies on the page socket is refused', refusedMsg(await pp.send('Storage.clearCookies')));
const startupCookies = (await rawAdmin2.send('Storage.getCookies')).result?.cookies ?? [];
check('page/child cookie reads, writes and clears leave the startup sentinel unchanged', startupCookies.some((c) => c.name === SENTINEL && c.value === 's'));
for (const [label, method, params] of [
  ['Browser.setPermission', 'Browser.setPermission', { permission: { name: 'geolocation' }, setting: 'granted' }],
  ['Browser.setDownloadBehavior', 'Browser.setDownloadBehavior', { behavior: 'allow' }],
  ['Storage.clearDataForOrigin', 'Storage.clearDataForOrigin', { origin: 'http://127.0.0.1', storageTypes: 'all' }],
  ['Target.createBrowserContext', 'Target.createBrowserContext', {}],
  ['Target.sendMessageToTarget', 'Target.sendMessageToTarget', { sessionId: 'x', message: '{"id":1,"method":"Browser.close"}' }],
  ['Target.attachToTarget flatten:false', 'Target.attachToTarget', { targetId: sa.targetId, flatten: false }],
]) check(`${label} on the page socket is refused`, refusedMsg(await pp.send(method, params)));
check('no receivedMessageFromTarget tunnelling events reach scoped clients', ![...ca.events, ...pp.events].some((e) => e.method === 'Target.receivedMessageFromTarget'));
await rawAdmin2.send('Storage.setCookies', { cookies: [{ name: SENTINEL, value: 's', url: 'http://127.0.0.1/', expires: 1 }] });  // remove the sentinel
rawAdmin2.close(); pp.close();

// 5. Popups and child targets stay inside the owner.
const pa = await cdp(sa.cdpUrl);
await pa.send('Runtime.evaluate', { expression: "window.open('about:blank#popup','_blank') && 1", userGesture: true, returnByValue: true });
await sleep(800);
const aAfter = (await ca.send('Target.getTargets')).result?.targetInfos ?? [];
const bAfter = (await cb.send('Target.getTargets')).result?.targetInfos ?? [];
const popup = aAfter.find((t) => t.targetId !== sa.targetId && /popup/.test(t.url ?? ''));
console.log(`info a popup of A: ${popup ? 'visible to A' : 'none (blocked)'}`);
check('a popup of A is never visible to B', !bAfter.some((t) => /popup/.test(t.url ?? '')));
pa.close();

// 6. Lifecycle: stopping B revokes its sockets; its endpoints become 404 (stale), and a reload does not revive them.
let closedB = false;
cb.ws.addEventListener('close', () => { closedB = true; });
const stop = await adminCall('stop-browser-profile', { coreProfileKey: B.key });
check('stop-browser-profile B drains', stop.ok, `${stop.error?.code ?? ''}`);
for (let i = 0; i < 20 && !closedB; i++) await sleep(100);
check("B's open session socket was closed by the stop", closedB);
check("B's session endpoint is now 404 (stale)", (await wsStatus(sb.cdpEndpoint)) === 404, `${await wsStatus(sb.cdpEndpoint)}`);
check("B's page endpoint is now 404 (stale)", (await wsStatus(sb.cdpUrl)) === 404, `${await wsStatus(sb.cdpUrl)}`);
const reload = await adminCall('load-browser-profile', { coreProfileKey: B.key });
check('B reloads', reload.ok);
check('B\'s old session endpoint stays dead after a reload (new lifecycle epoch)', (await wsStatus(sb.cdpEndpoint)) === 404);
check("A's socket kept working through B's stop and restart", !(await ca.send('Target.getTargets')).error);
check("A's endpoint is still valid", (await wsStatus(sa.cdpEndpoint)) === 101);

// 7. Revoking the grant closes A's sockets: close the session and the same endpoint dies.
let closedA = false;
ca.ws.addEventListener('close', () => { closedA = true; });
await adminCall('close-session', { sessionId: sa.sessionId }, { profile: A.key });
for (let i = 0; i < 20 && !closedA; i++) await sleep(100);
check("closing A's session closes its sockets", closedA);
check("A's closed session endpoint is gone (404)", (await wsStatus(sa.cdpEndpoint)) === 404);
cb.close();

finish();

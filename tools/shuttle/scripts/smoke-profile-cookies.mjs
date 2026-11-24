// Cookie isolation between Profiles (docs/PROFILES.md sections 3, 4, 5, 7, 9): node scripts/smoke-profile-cookies.mjs <port> <user-data-dir>
// Run by scripts/smoke-all.sh --with profiles, on a launch with --shuttle-isolated-profiles. Exit 3 when not applicable.
// Two throwaway Profiles with the SAME workspace slug, one local origin:
//   1. a cookie set through A's scoped session is invisible to B (CDP Storage.getCookies, Network.getAllCookies on the
//      page socket, document.cookie), and the other way round; clearing A's cookies leaves B's;
//   2. a foreign browserContextId on a cookie command is refused;
//   3. a persistent cookie survives stop + load of its Profile (drain flushes storage) and is still not B's;
//   4. a persona change (set-fingerprint while the profile is stopped, then load) never clears cookies;
//   5. an old endpoint of a stopped profile can't read the cookies of the restarted one (stale grant);
//   6. no cookie value ever shows in a browser-level tool result.
import http from 'node:http';
import { cdp, check, cleanup, finish, sleep, wsStatus } from './lib/smoke.mjs';
import { adminCall, makeProfile, noSecrets, persona, refused, requireIsolated, scopedSession } from './lib/profiles.mjs';

await requireIsolated();
const origin = http.createServer((req, res) => { res.writeHead(200, { 'Content-Type': 'text/html', 'Cache-Control': 'no-store' }); res.end('<!doctype html><title>c</title>ok'); });
await new Promise((r) => origin.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${origin.address().port}`;
cleanup(() => origin.close());
const EXPIRES = Math.floor(Date.now() / 1000) + 86400 * 30;
const SECRET_A = 'cookie-a-' + Math.random().toString(36).slice(2), SECRET_B = 'cookie-b-' + Math.random().toString(36).slice(2);

const A = await makeProfile('cookie-ws');
const B = await makeProfile('cookie-ws', { persona: persona({ locale: 'de-DE', languages: ['de-DE', 'de'], timezone: 'Pacific/Auckland' }) });
let sa = await scopedSession(A), sb = await scopedSession(B);
const names = (r) => (r.result?.cookies ?? []).map((c) => `${c.name}=${c.value}`);
async function openPage(s) {
  const c = await cdp(s.cdpUrl);
  await c.send('Page.navigate', { url: `${ORIGIN}/` }, undefined, 30000);
  await sleep(500);
  return c;
}
const jsCookie = async (c) => (await c.send('Runtime.evaluate', { expression: 'document.cookie', returnByValue: true })).result?.result?.value;

// 1. Same name, same origin, two profiles.
let ca = await cdp(sa.cdpEndpoint), cb = await cdp(sb.cdpEndpoint);
const setA = await ca.send('Storage.setCookies', { cookies: [{ name: 'shared', value: SECRET_A, url: ORIGIN, expires: EXPIRES }, { name: 'sess', value: SECRET_A, url: ORIGIN }] });
const setB = await cb.send('Storage.setCookies', { cookies: [{ name: 'shared', value: SECRET_B, url: ORIGIN, expires: EXPIRES }] });
check('both sessions can set their own cookies', !setA.error && !setB.error, JSON.stringify(setA.error ?? setB.error ?? ''));
const aCookies = names(await ca.send('Storage.getCookies')), bCookies = names(await cb.send('Storage.getCookies'));
check("A sees its own cookies and none of B's", aCookies.includes(`shared=${SECRET_A}`) && !aCookies.some((c) => c.includes(SECRET_B)), `${aCookies.length}`);
check("B sees its own cookie and none of A's", bCookies.includes(`shared=${SECRET_B}`) && !bCookies.some((c) => c.includes(SECRET_A)) && !bCookies.some((c) => c.startsWith('sess=')), `${bCookies.length}`);
const pa = await openPage(sa), pb = await openPage(sb);
check('document.cookie in A is A\'s only', (await jsCookie(pa)).includes(SECRET_A) && !(await jsCookie(pa)).includes(SECRET_B));
check('document.cookie in B is B\'s only', (await jsCookie(pb)).includes(SECRET_B) && !(await jsCookie(pb)).includes(SECRET_A));
const allB = await pb.send('Network.getAllCookies');
check('Network.getAllCookies on B\'s page socket shows no cookie of A', !(allB.result?.cookies ?? []).some((c) => c.value === SECRET_A), `${allB.result?.cookies?.length ?? JSON.stringify(allB.error)}`);

// Clearing A's cookies never reaches B.
const clr = await ca.send('Storage.clearCookies');
check("A's clearCookies succeeds", !clr.error, JSON.stringify(clr.error ?? ''));
check("A's cookies are gone", names(await ca.send('Storage.getCookies')).length === 0);
check("B's cookie survived A's clearCookies", names(await cb.send('Storage.getCookies')).includes(`shared=${SECRET_B}`));
await ca.send('Storage.setCookies', { cookies: [{ name: 'shared', value: SECRET_A, url: ORIGIN, expires: EXPIRES }] });

// 2. Foreign browserContextId.
for (const method of ['Storage.getCookies', 'Storage.clearCookies']) {
  check(`${method} with a foreign browserContextId is refused`, !!(await ca.send(method, { browserContextId: 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' })).error);
}
check('Storage.setCookies with a foreign browserContextId is refused', !!(await ca.send('Storage.setCookies', { browserContextId: 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF', cookies: [{ name: 'x', value: 'y', url: ORIGIN }] })).error);
check("B's cookies are unchanged after those refusals", names(await cb.send('Storage.getCookies')).includes(`shared=${SECRET_B}`));
pa.close(); pb.close(); ca.close(); cb.close();

// 6 (early). Browser-level tools never echo cookie values.
noSecrets('list-browser-profiles shows no cookie value', (await adminCall('list-browser-profiles')).data, [SECRET_A, SECRET_B]);

// 3+4. Stop A (flush), persona change while drained, reload: the persistent cookie is back, B untouched.
await adminCall('close-session', { sessionId: sa.sessionId }, { profile: A.key });
const stop = await adminCall('stop-browser-profile', { coreProfileKey: A.key });
check('stop A', stop.ok, `${stop.error?.code ?? ''}`);
refused('any other scoped tool on a stopped profile is profile_not_ready', await adminCall('list-agents', {}, { profile: A.key }), 'profile_not_ready');
// The persona is replaced while STOPPED, without loading the profile; cookies and storage are not touched.
const setfp = await adminCall('set-fingerprint', { fingerprint: persona({ locale: 'it-IT', languages: ['it-IT', 'it'], timezone: 'Europe/Rome' }) }, { profile: A.key });
check('set-fingerprint on a stopped profile works', setfp.ok, `${setfp.error?.code ?? ''}`);
const load = await adminCall('load-browser-profile', { coreProfileKey: A.key });
check('reload A', load.ok, `${load.error?.code ?? ''}`);
refused('set-fingerprint on the now ready profile is profile_busy', await adminCall('set-fingerprint', { fingerprint: persona({ timezone: 'Europe/Madrid' }) }, { profile: A.key }), 'profile_busy');
const old = sa;
sa = await scopedSession(A);
ca = await cdp(sa.cdpEndpoint);
const back = names(await ca.send('Storage.getCookies'));
check('the persistent cookie survived stop + load + persona change (cookies are never cleared)', back.includes(`shared=${SECRET_A}`), `${back.length} cookies`);
check("B's cookie is still B's", names(await (await cdp(sb.cdpEndpoint)).send('Storage.getCookies')).includes(`shared=${SECRET_B}`));

// 5. The old endpoint can't read the restarted profile.
check("A's pre-stop endpoint is dead (404), the new one is alive", (await wsStatus(old.cdpEndpoint)) === 404 && (await wsStatus(sa.cdpEndpoint)) === 101);
ca.close();

finish();

// Per-Profile persona on the wire and in scripts, and the proxy fail policy (docs/PROFILES.md sections 3, 7, 9):
//   node scripts/smoke-profile-network.mjs <port> <user-data-dir>
// Run by scripts/smoke-all.sh --with profiles, on a launch with --shuttle-isolated-profiles. Exit 3 when not applicable.
// Local only: one origin on 127.0.0.1 records the request headers of every request, a local HTTP proxy fixture serves a
// fake host (proxied.test) and records what reaches it.
//   1. two Profiles with different personas: each one's first navigation carries ITS User-Agent, Accept-Language and
//      Sec-CH-UA*, and what scripts read (userAgent, languages, hardwareConcurrency, deviceMemory, screen, time zone,
//      locale, UTC offsets) is the same persona; they differ from each other and from the startup profile;
//   2. dedicated workers: the values printed are compared with the page; a mismatch is a SKIP (open), not a pass;
//   3. proxy: a Profile whose workspace agent has a proxy sends its traffic ONLY through it (the fixture sees the
//      request with the persona's headers); an unproxied Profile never touches the fixture;
//   4. fail policy: a proxy that cannot be reached is an error, never a direct fallback (the origin sees no request);
//   5. stopping the proxied Profile leaves the other one untouched.
import http from 'node:http';
import { cdp, check, cleanup, finish, skipCheck, sleep } from './lib/smoke.mjs';
import { adminCall, makeProfile, persona, requireIsolated, scopedSession } from './lib/profiles.mjs';

await requireIsolated();

const originHits = [], proxyHits = [];
const html = (tag) => `<!doctype html><title>${tag}</title><img src="/img?tag=${tag}-sub">ok`;
const origin = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  originHits.push({ tag: u.searchParams.get('tag'), headers: { ...req.headers } });
  if (u.pathname === '/img') { res.writeHead(200, { 'Content-Type': 'image/gif', 'Cache-Control': 'no-store' }); return res.end(Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7', 'base64')); }
  res.writeHead(200, { 'Content-Type': 'text/html', 'Cache-Control': 'no-store' });
  res.end(html(u.searchParams.get('tag')));
});
await new Promise((r) => origin.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${origin.address().port}`;
cleanup(() => origin.close());
// The proxy answers http://proxied.test/... itself (an absolute-URI GET), so nothing needs DNS or a real server.
const proxy = http.createServer((req, res) => {
  const u = new URL(req.url);
  proxyHits.push({ host: u.host, tag: u.searchParams.get('tag'), headers: { ...req.headers } });
  if (u.pathname === '/img') { res.writeHead(200, { 'Content-Type': 'image/gif', 'Cache-Control': 'no-store' }); return res.end(Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7', 'base64')); }
  res.writeHead(200, { 'Content-Type': 'text/html', 'Cache-Control': 'no-store' });
  res.end(html(u.searchParams.get('tag')));
});
await new Promise((r) => proxy.listen(0, '127.0.0.1', r));
const PROXY = `http://127.0.0.1:${proxy.address().port}`;
cleanup(() => proxy.close());
const deadPort = await new Promise((r) => { const s = http.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });

const PAGE = `(async () => {
  const f = Intl.DateTimeFormat().resolvedOptions();
  let worker;
  try {
    worker = await new Promise((resolve, reject) => {
      const w = new Worker(URL.createObjectURL(new Blob(['postMessage({ua: navigator.userAgent, languages: navigator.languages, hc: navigator.hardwareConcurrency, tz: Intl.DateTimeFormat().resolvedOptions().timeZone, loc: Intl.DateTimeFormat().resolvedOptions().locale})'])));
      w.onmessage = (e) => resolve(e.data); w.onerror = () => reject(new Error('worker error')); setTimeout(() => reject(new Error('worker timeout')), 5000);
    });
  } catch (e) { worker = { error: String(e.message) }; }
  return { ua: navigator.userAgent, languages: [...navigator.languages], hc: navigator.hardwareConcurrency, dm: navigator.deviceMemory,
    sw: screen.width, sh: screen.height, dpr: devicePixelRatio, tz: f.timeZone, loc: f.locale,
    offJan: new Date(Date.UTC(2024, 0, 15, 12)).getTimezoneOffset(), offJul: new Date(Date.UTC(2024, 6, 15, 12)).getTimezoneOffset(), worker };
})()`;
const offsetMinutes = (tz, date) => {
  const p = new Intl.DateTimeFormat('en-US', { timeZone: tz, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric' }).formatToParts(date);
  const g = (t) => Number(p.find((x) => x.type === t).value);
  return Math.round((date.getTime() - Date.UTC(g('year'), g('month') - 1, g('day'), g('hour'), g('minute'), g('second'))) / 60000);
};
const JAN = new Date(Date.UTC(2024, 0, 15, 12)), JUL = new Date(Date.UTC(2024, 6, 15, 12));
const waitUntil = async (fn, ms) => { for (const until = Date.now() + ms; Date.now() < until; await sleep(200)) if (fn()) return true; return fn(); };

let tagN = 0;
// Navigates the profile's tab (first navigation of a fresh tab included); returns {tag, main, sub, page, nav}.
async function visit(s, url, hits) {
  const tag = `n${++tagN}`;
  const c = await cdp(s.cdpUrl);
  try {
    const nav = await c.send('Page.navigate', { url: `${url}/p?tag=${tag}` }, undefined, 30000);
    await waitUntil(() => hits.some((h) => h.tag === `${tag}-sub`), 8000);
    await sleep(300);
    const r = await c.send('Runtime.evaluate', { expression: PAGE, returnByValue: true, awaitPromise: true }, undefined, 30000);
    return { tag, nav: nav.result ?? nav.error, main: hits.find((h) => h.tag === tag), sub: hits.find((h) => h.tag === `${tag}-sub`), page: r.result?.result?.value ?? {} };
  } finally { c.close(); }
}
const personaOf = async (p) => (await adminCall('get-fingerprint', {}, { profile: p.key })).data?.fingerprint;

function expectPersona(label, f, v) {
  const h = v.main?.headers ?? {}, p = v.page;
  check(`${label}: main request User-Agent is the persona's`, h['user-agent'] === f.userAgent, `${h['user-agent']}`);
  check(`${label}: subresource User-Agent and Accept-Language agree with the main request`, v.sub?.headers?.['user-agent'] === f.userAgent && v.sub?.headers?.['accept-language'] === h['accept-language']);
  check(`${label}: Accept-Language starts with the persona's locale`, (h['accept-language'] ?? '').startsWith(f.languages[0]), `${h['accept-language']}`);
  check(`${label}: Sec-CH-UA names the persona's platform and major version`, h['sec-ch-ua-platform'] === `"${f.clientHints.platform}"` &&
    (h['sec-ch-ua'] ?? '').includes(`"Google Chrome";v="${f.browser.version.split('.')[0]}"`), `${h['sec-ch-ua-platform']} ${h['sec-ch-ua']}`);
  check(`${label}: headers and scripts agree (userAgent, languages)`, p.ua === h['user-agent'] && JSON.stringify(p.languages) === JSON.stringify(f.languages), `${p.ua}`);
  check(`${label}: hardwareConcurrency, deviceMemory, screen, devicePixelRatio`, p.hc === f.hardwareConcurrency && p.dm === f.deviceMemory && p.sw === f.screen.width && p.sh === f.screen.height && p.dpr === f.screen.devicePixelRatio, JSON.stringify([p.hc, p.dm, p.sw, p.sh, p.dpr]));
  check(`${label}: time zone and locale are the persona's (Intl, Jan/Jul UTC offsets)`, p.tz === f.timezone && p.loc === f.locale && p.offJan === offsetMinutes(f.timezone, JAN) && p.offJul === offsetMinutes(f.timezone, JUL), JSON.stringify([p.tz, p.loc, p.offJan, p.offJul]));
  const w = p.worker ?? {};
  if (w.error) skipCheck(`${label}: dedicated worker values`, w.error);
  else if (w.ua === f.userAgent && w.tz === f.timezone && w.loc === f.locale && w.hc === f.hardwareConcurrency) check(`${label}: dedicated worker sees the same persona`, true);
  else skipCheck(`${label}: dedicated worker sees the persona`, `worker differs from the persona: ${JSON.stringify(w).slice(0, 160)} (workers not wired)`);
}

// 1+2. Two profiles, two personas, first navigation of fresh tabs.
const A = await makeProfile('net-a');
const B = await makeProfile('net-b', { persona: persona({ locale: 'de-DE', languages: ['de-DE', 'de'], timezone: 'Pacific/Auckland', hardwareConcurrency: 12, deviceMemory: 8, screen: { width: 1920, height: 1080, devicePixelRatio: 1, colorDepth: 24 } }) });
const sa = await scopedSession(A), sb = await scopedSession(B);
const fa = await personaOf(A), fb = await personaOf(B);
check('both personas read back and differ', !!fa && !!fb && (fa.userAgent !== fb.userAgent || fa.timezone !== fb.timezone), `${fa?.timezone} / ${fb?.timezone}`);
if (!fa || !fb) await finish();
const va = await visit(sa, ORIGIN, originHits), vb = await visit(sb, ORIGIN, originHits);
expectPersona('A (first navigation)', fa, va);
expectPersona('B (first navigation)', fb, vb);
check('A and B never show each other\'s hardware or time zone', va.page.hc !== vb.page.hc && va.page.tz !== vb.page.tz && va.page.languages[0] !== vb.page.languages[0]);
// the startup profile keeps its own (stock) identity
const startup = await adminCall('open-session', { agent: 'smoke-net-startup' }).then(async (r) => {
  if (!r.ok) { const c = await adminCall('create-agent', { id: 'smoke-net-startup', name: 'Net startup' }); if (!c.ok) return null;
    cleanup(() => adminCall('delete-agent', { id: 'smoke-net-startup' })); return adminCall('open-session', { agent: 'smoke-net-startup' }); }
  return r;
});
if (startup?.ok) {
  cleanup(() => adminCall('close-session', { sessionId: startup.data.sessionId }));
  const t = await adminCall('allocate-tab', { sessionId: startup.data.sessionId });
  const vs = t.ok ? await visit({ cdpUrl: t.data.cdpUrl }, ORIGIN, originHits) : null;
  check('the startup profile is not given an isolated persona', !!vs && vs.main?.headers['user-agent'] !== fa.userAgent && vs.main?.headers['user-agent'] !== fb.userAgent && vs.page.tz !== fa.timezone && vs.page.tz !== fb.timezone,
    `${vs?.page?.tz}`);
} else skipCheck('startup profile identity', 'could not open a startup-profile session');

// 3. Proxy: C's workspace agent has a proxy; A (none) never reaches the fixture.
const C = await makeProfile('net-c', { proxy: PROXY, persona: persona({ locale: 'es-ES', languages: ['es-ES', 'es'], timezone: 'Europe/Madrid', hardwareConcurrency: 8 }) });
const sc = await scopedSession(C);
const fc = await personaOf(C);
const vc = await visit(sc, 'http://proxied.test', proxyHits);
check('proxied profile: the fixture saw the request for the fake host', proxyHits.some((h) => h.host === 'proxied.test' && h.tag === vc.tag), `${proxyHits.length} hits`);
check('proxied profile: the page loaded through the proxy', vc.page.ua === fc.userAgent, `${vc.nav?.errorText ?? ''}`);
check('proxied profile: the proxy request carries the persona\'s headers', proxyHits.find((h) => h.tag === vc.tag)?.headers['user-agent'] === fc.userAgent);
check('proxied profile: subresources use the proxy too', proxyHits.some((h) => h.tag === `${vc.tag}-sub`));
const before = proxyHits.length;
await visit(sa, 'http://proxied.test', proxyHits).catch(() => null);
check('unproxied profile A never reaches the proxy fixture', proxyHits.length === before, `${proxyHits.length - before} extra hits`);
const direct = originHits.length;
await visit(sc, ORIGIN, originHits).catch(() => null);
console.log(`info proxied profile, loopback origin: ${originHits.length - direct} direct hits (Chromium bypasses the proxy for loopback by default)`);

// 4. Fail policy: an unreachable proxy is an error, never a direct fallback.
const D = await makeProfile('net-d', { proxy: `http://127.0.0.1:${deadPort}`, load: false });
const ld = await adminCall('load-browser-profile', { coreProfileKey: D.key });
if (!ld.ok) {
  check('a proxy that cannot be set up fails the load with proxy_not_ready (no ready profile without its proxy)', ld.error?.code === 'proxy_not_ready' || ld.error?.code === 'profile_failed', `${ld.error?.code}`);
} else {
  const sd = await scopedSession(D);
  const before2 = originHits.length;
  const failed = await visit(sd, ORIGIN.replace('127.0.0.1', 'localhost.proxied.test'), originHits).catch((e) => ({ nav: { errorText: String(e.message) } }));
  check('dead proxy: the navigation fails with a proxy error', /PROXY|TUNNEL|CONNECTION/i.test(failed.nav?.errorText ?? ''), `${failed.nav?.errorText}`);
  check('dead proxy: no direct fallback (the origin saw nothing)', originHits.length === before2, `${originHits.length - before2} hits`);
}

// 5. Stopping the proxied profile leaves the others untouched.
await adminCall('stop-browser-profile', { coreProfileKey: C.key });
const vb2 = await visit(sb, ORIGIN, originHits);
expectPersona('B after C stopped', fb, vb2);

// 6. The same still-loaded Profile changes construction-time identity. Keep B
// alive throughout and check both first-navigation headers and retained state.
const seed = await cdp(sa.cdpUrl);
try {
  await seed.send('Page.navigate', { url: `${ORIGIN}/p?tag=retain` });
  await sleep(400);
  await seed.send('Runtime.evaluate', { expression: "document.cookie='persona_retained=yes; path=/'; localStorage.persona_retained='yes'" });
} finally { seed.close(); }
const stoppedA = await adminCall('stop-browser-profile', { coreProfileKey: A.key });
check('A stopped before identity replacement', stoppedA.ok);
const changedA = await adminCall('set-fingerprint', {
  fingerprint: persona({ browserVersion: '151.0.7100.1', locale: 'it-IT', languages: ['it-IT', 'it'] }),
}, { profile: A.key });
check('loaded stopped A accepts replacement', changedA.ok, changedA.error?.code);
const loadedA = await adminCall('load-browser-profile', { coreProfileKey: A.key });
check('A loads after replacement', loadedA.ok, loadedA.error?.code);
if (changedA.ok && loadedA.ok) {
  const replacementSession = await scopedSession(A);
  const replacementPersona = await personaOf(A);
  const replacementVisit = await visit(replacementSession, ORIGIN, originHits);
  expectPersona('A after version replacement (first request)', replacementPersona, replacementVisit);
  check('replacement actually changed UA version', replacementPersona.userAgent !== fa.userAgent);
  const retained = await cdp(replacementSession.cdpUrl);
  try {
    const state = await retained.send('Runtime.evaluate', { expression: "({cookie:document.cookie, storage:localStorage.persona_retained})", returnByValue: true });
    const value = state.result?.result?.value;
    check('replacement retains session cookie and localStorage', value?.cookie?.includes('persona_retained=yes') && value.storage === 'yes');
  } finally { retained.close(); }
  // test-proxy uses a browser-process factory with no explicit UA override.
  const beforeCheck = proxyHits.length;
  const stoppedC = await adminCall('set-fingerprint', {
    fingerprint: persona({ browserVersion: '152.0.7200.1', locale: 'it-IT', languages: ['it-IT', 'it'] }),
  }, { profile: C.key });
  const reloadedC = await adminCall('load-browser-profile', { coreProfileKey: C.key });
  check('proxied loaded Profile replacement succeeds', stoppedC.ok && reloadedC.ok);
  if (stoppedC.ok && reloadedC.ok) {
    const currentC = await personaOf(C);
    await adminCall('test-proxy', { agentId: C.workspaceAgent, url: 'http://proxied.test/p?tag=browser-process-replaced' }, { profile: C.key });
    const request = proxyHits.slice(beforeCheck).find((hit) => hit.tag === 'browser-process-replaced');
    check('browser-process proxy check uses replacement UA and language', request?.headers['user-agent'] === currentC.userAgent && request?.headers['accept-language']?.startsWith('it-IT'));
  }
  expectPersona('B remains available during A/C replacement', fb, await visit(sb, ORIGIN, originHits));
}

finish();

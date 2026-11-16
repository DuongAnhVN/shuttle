// Fingerprint persona, profile-level runtime application (docs/FINGERPRINT.md "Runtime"): node scripts/smoke-fingerprint-runtime.mjs <port>
// Run by scripts/smoke-all.sh after smoke-fingerprint-persona.mjs, on the same launch. Exit 3 (skipped) when no browser
// answers on <port> or it has no persona tools: nothing here can run without the built binary.
//
// The persona is ONE per Chromium profile: every agent, tab and storage partition of the profile presents the same one.
// Local only: one origin on 127.0.0.1 records the request headers of every request (main frame and a subresource), and
// the probe reads what scripts see with Runtime.evaluate (a test harness: the runtime itself uses no CDP or injection).
//   1. with a profile persona set, each agent's FIRST navigation carries the persona's User-Agent, Sec-CH-UA*,
//      Accept-Language; subresources agree;
//   2. what scripts read (navigator.userAgent/userAgentData/platform/languages/hardwareConcurrency/deviceMemory,
//      screen.*, devicePixelRatio) is the same persona as the headers;
//   3. sharing: two agents (two storage partitions) and a throwaway agent show the SAME persona, with no per-agent
//      difference; there is no per-agent isolation to test;
//   4. a persona change shows on the next navigation of every open tab (no restart); clearing it restores the stock value.
//   5. the persona's time zone and locale are the page's ICU defaults: Intl.DateTimeFormat timeZone/locale, getTimezoneOffset
//      (Jan and Jul), toLocaleString; a change applies to an open tab, clearing restores the host's.
// Not covered (not implemented, see FINGERPRINT.md): canvas, audio, fonts, media, WebRTC spoofing.
// Workers: dedicated-worker values (including time zone/locale) are printed as information only.
import http from 'node:http';
import { cdp, check, cleanup, finish, rpc, skip, skipCheck, sleep } from './lib/smoke.mjs';

// Is a browser there, and does it know the profile persona?
let tools;
try {
  tools = (await rpc('tools/list'))?.body?.result?.tools;
} catch (e) {
  await skip(`no browser answering MCP on this port (${e.message}); build and launch the browser first`);
}
if (!tools?.find((t) => t.name === 'set-fingerprint')) {
  await skip('no set-fingerprint tool: this browser build predates the profile-level fingerprint persona');
}

async function call(name, a = {}) {
  const { status, body } = await rpc('tools/call', { name, arguments: a });
  if (status !== 200 || !body?.result) throw new Error(`${name}: HTTP ${status} ${JSON.stringify(body?.error ?? body).slice(0, 200)}`);
  if (body.result.isError) return { error: body.result.content?.[0]?.text ?? 'error' };
  return body.result.structuredContent ?? body.result;
}
const setFp = (fingerprint) => call('set-fingerprint', { fingerprint });
const waitUntil = async (fn, ms) => {
  for (const until = Date.now() + ms; Date.now() < until; await sleep(200)) if (await fn()) return true;
  return !!(await fn());
};

// Origin: records the headers of every request by tag (?tag=...); the page asks for a subresource tagged <tag>-sub and
// requests high-entropy client hints for the next navigation (Accept-CH).
const hits = [];
const origin = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const tag = u.searchParams.get('tag');
  hits.push({ tag, path: u.pathname, headers: { ...req.headers } });
  if (u.pathname === '/img') {
    res.writeHead(200, { 'Content-Type': 'image/gif', 'Cache-Control': 'no-store' });
    return res.end(Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7', 'base64'));
  }
  res.writeHead(200, {
    'Content-Type': 'text/html',
    'Cache-Control': 'no-store',
    'Accept-CH': 'Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, Sec-CH-UA-Bitness, Sec-CH-UA-Full-Version-List, Sec-CH-UA-Model',
  });
  res.end(`<!doctype html><title>${tag}</title><img src="/img?tag=${tag}-sub">ok`);
});
await new Promise((r) => origin.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${origin.address().port}`;
cleanup(() => origin.close());

const made = [];
const sessions = new Map();  // agent -> {sessionId, targetId, cdpUrl}
cleanup(async () => {
  for (const s of sessions.values()) await call('close-session', { sessionId: s.sessionId });
  for (const id of made) await call('delete-agent', { id });
  await setFp(null);
});
async function tabOf(agent) {
  if (!sessions.has(agent)) {
    const s = await call('open-session', { agent });
    if (s.error) throw new Error(s.error);
    const t = await call('allocate-tab', { sessionId: s.sessionId });
    if (t.error) throw new Error(t.error);
    sessions.set(agent, { sessionId: s.sessionId, targetId: t.targetId, cdpUrl: t.cdpUrl });
  }
  return sessions.get(agent);
}
async function evalIn(cdpUrl, expression) {
  const c = await cdp(cdpUrl);
  try {
    const r = await c.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, undefined, 30000);
    return r.result?.result?.value ?? { exception: r.result?.exceptionDetails?.text };
  } finally { c.close(); }
}

// What scripts read in the page.
const PAGE = `(async () => {
  const u = navigator.userAgentData;
  const he = u ? await u.getHighEntropyValues(['platformVersion', 'architecture', 'bitness', 'fullVersionList', 'uaFullVersion', 'model']) : null;
  let worker = null;
  try {
    worker = await new Promise((resolve, reject) => {
      const w = new Worker(URL.createObjectURL(new Blob(['postMessage({ua: navigator.userAgent, platform: navigator.platform, hc: navigator.hardwareConcurrency, dm: navigator.deviceMemory, languages: navigator.languages, tz: Intl.DateTimeFormat().resolvedOptions().timeZone, loc: Intl.DateTimeFormat().resolvedOptions().locale})'])));
      w.onmessage = (e) => resolve(e.data); w.onerror = (e) => reject(new Error('worker error')); setTimeout(() => reject(new Error('worker timeout')), 5000);
    });
  } catch (e) { worker = { error: String(e.message) }; }
  // WebGPU policy: 'nogpu' (no navigator.gpu), 'null' (requestAdapter resolved null: blocked, or no usable host GPU) or 'adapter'.
  let gpu;
  try {
    if (!navigator.gpu) gpu = 'nogpu';
    else {
      const a = await Promise.race([navigator.gpu.requestAdapter(), new Promise((r) => setTimeout(() => r('timeout'), 10000))]);
      gpu = a === null ? 'null' : a === 'timeout' ? 'timeout' : 'adapter';
    }
  } catch (e) { gpu = 'error:' + e.message; }
  return {
    gpu,
    ua: navigator.userAgent, platform: navigator.platform, language: navigator.language, languages: [...navigator.languages],
    hc: navigator.hardwareConcurrency, dm: navigator.deviceMemory,
    sw: screen.width, sh: screen.height, saw: screen.availWidth, sah: screen.availHeight, cd: screen.colorDepth, pd: screen.pixelDepth,
    dpr: window.devicePixelRatio,
    tz: Intl.DateTimeFormat().resolvedOptions().timeZone, intlLocale: Intl.DateTimeFormat().resolvedOptions().locale,
    offJan: new Date(Date.UTC(2024, 0, 15, 12)).getTimezoneOffset(), offJul: new Date(Date.UTC(2024, 6, 15, 12)).getTimezoneOffset(),
    dateStr: new Date(Date.UTC(2024, 6, 15, 12, 30, 45)).toLocaleString(), numStr: (1234567.891).toLocaleString(),
    uad: u && { platform: u.platform, mobile: u.mobile, brands: u.brands.map((b) => b.brand + '/' + b.version) },
    he, worker,
  };
})()`;

let tagN = 0;
// Navigates the agent's tab; returns the origin's records of the main request and of its subresource.
async function visit(agent) {
  const t = await tabOf(agent);
  const tag = `v${++tagN}`;
  const nav = await call('navigate-tab', { targetId: t.targetId, url: `${ORIGIN}/p?tag=${tag}` });
  if (nav.error) throw new Error(nav.error);
  const got = await waitUntil(() => hits.some((h) => h.tag === `${tag}-sub`), 10000);
  const main = hits.find((h) => h.tag === tag);
  const sub = hits.find((h) => h.tag === `${tag}-sub`);
  return { tag, main, sub, got, page: await evalIn(t.cdpUrl, PAGE) };
}

// Stock values first (no persona yet): what the browser reports on its own.
const A = 'smoke-fr-a', B = 'smoke-fr-b';
for (const id of [A, B]) await call('delete-agent', { id });
await setFp(null);
for (const [id, name] of [[A, 'Runtime A'], [B, 'Runtime B']]) {
  const r = await call('create-agent', { id, name });
  if (r.error) throw new Error(r.error);
  made.push(id);
}
const stock = await visit(A);

// A persona that differs from any stock browser (an old version number and non-default hardware, so a stock
// Linux/Mac/Windows host can't match by accident).
const set1 = await setFp({ preset: 'macos-chrome', browserVersion: '150.0.7000.1', locale: 'fr-FR', timezone: 'Asia/Tokyo', hardwareConcurrency: 10, deviceMemory: 8, screen: { width: 1512, height: 982 } });
const f1 = set1.fingerprint;
check('profile persona set', !!f1 && f1.os === 'macos', JSON.stringify(set1.error ?? ''));
if (!f1) await finish();

const major = (f) => f.browser.version.split('.')[0];
const q = (s) => `"${s}"`;
// Reference values computed by this (Node) process for an explicit zone and locale: what the page's DEFAULT zone/locale must give.
const offsetMinutes = (tz, date) => {
  const p = new Intl.DateTimeFormat('en-US', { timeZone: tz, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric' }).formatToParts(date);
  const g = (t) => Number(p.find((x) => x.type === t).value);
  return Math.round((date.getTime() - Date.UTC(g('year'), g('month') - 1, g('day'), g('hour'), g('minute'), g('second'))) / 60000);
};
const JAN = new Date(Date.UTC(2024, 0, 15, 12)), JUL = new Date(Date.UTC(2024, 6, 15, 12));
const DATE = new Date(Date.UTC(2024, 6, 15, 12, 30, 45));

// Native WebGPU policy (FINGERPRINT.md "WebGPU"): the host adapter is exposed only when it can't contradict the persona; otherwise
// navigator.gpu.requestAdapter() resolves null. Never a fabricated adapter. Host os from the stock navigator.platform.
const HOST_OS = { Win32: 'windows', MacIntel: 'macos' }[stock.page.platform] ?? (/^Linux/.test(stock.page.platform) ? 'linux' : '');
function expectWebGpu(label, f, p) {
  const blocked = f.os !== HOST_OS || !!f.webgl;
  if (blocked) {
    check(`${label}: WebGPU is blocked (requestAdapter resolves null) for a persona the host adapter can't match`, p.gpu === 'null', `${p.gpu}`);
  } else {
    // Compatible persona: the host adapter is exposed as in stock (null only if the host has no usable GPU).
    check(`${label}: WebGPU is the stock host result for a compatible persona`, p.gpu === stock.page.gpu, `${p.gpu} (stock ${stock.page.gpu})`);
  }
}

// Everything one visit must show for persona `f`.
function expectPersona(label, f, v) {
  const h = v.main?.headers ?? {};
  check(`${label}: main request User-Agent is the persona's`, h['user-agent'] === f.userAgent, `${h['user-agent']}`);
  check(`${label}: subresource User-Agent is the persona's`, v.sub?.headers?.['user-agent'] === f.userAgent, `${v.sub?.headers?.['user-agent']}`);
  check(`${label}: Accept-Language starts with the persona's locale`, (h['accept-language'] ?? '').startsWith(`${f.languages[0]},`) || h['accept-language'] === f.languages[0],
    `${h['accept-language']}`);
  check(`${label}: subresource Accept-Language is the main one`, v.sub?.headers?.['accept-language'] === h['accept-language'], `${v.sub?.headers?.['accept-language']}`);
  check(`${label}: Sec-CH-UA-Platform / Mobile are the persona's`, h['sec-ch-ua-platform'] === q(f.clientHints.platform) && h['sec-ch-ua-mobile'] === '?0',
    `${h['sec-ch-ua-platform']} ${h['sec-ch-ua-mobile']}`);
  check(`${label}: Sec-CH-UA names the persona's major version`, (h['sec-ch-ua'] ?? '').includes(`"Google Chrome";v="${major(f)}"`), `${h['sec-ch-ua']}`);
  const p = v.page ?? {};
  check(`${label}: navigator.userAgent`, p.ua === f.userAgent, `${p.ua}`);
  check(`${label}: navigator.platform`, p.platform === f.platform, `${p.platform}`);
  check(`${label}: navigator.language(s)`, p.language === f.languages[0] && JSON.stringify(p.languages) === JSON.stringify(f.languages), `${p.language} ${JSON.stringify(p.languages)}`);
  check(`${label}: navigator.hardwareConcurrency`, p.hc === f.hardwareConcurrency, `${p.hc}`);
  check(`${label}: navigator.deviceMemory`, p.dm === f.deviceMemory, `${p.dm}`);
  check(`${label}: screen size and available size`, p.sw === f.screen.width && p.sh === f.screen.height && p.saw === f.screen.width && p.sah === f.screen.height,
    `${p.sw}x${p.sh} avail ${p.saw}x${p.sah}`);
  check(`${label}: screen color depth`, p.cd === f.screen.colorDepth && p.pd === f.screen.colorDepth, `${p.cd}/${p.pd}`);
  check(`${label}: window.devicePixelRatio`, p.dpr === f.screen.devicePixelRatio, `${p.dpr}`);
  // Time zone and locale (ICU defaults of the renderer process): Date, Intl.DateTimeFormat, getTimezoneOffset (winter and summer, so
  // the zone rules and not only a fixed offset are checked), toLocale*String.
  check(`${label}: Intl.DateTimeFormat().resolvedOptions().timeZone`, p.tz === f.timezone, `${p.tz}`);
  check(`${label}: Intl.DateTimeFormat().resolvedOptions().locale`, p.intlLocale === f.locale, `${p.intlLocale}`);
  check(`${label}: Date.getTimezoneOffset (Jan and Jul)`, p.offJan === offsetMinutes(f.timezone, JAN) && p.offJul === offsetMinutes(f.timezone, JUL),
    `${p.offJan}/${p.offJul} expected ${offsetMinutes(f.timezone, JAN)}/${offsetMinutes(f.timezone, JUL)}`);
  check(`${label}: Date.toLocaleString and Number.toLocaleString use the persona's zone and locale`,
    p.dateStr === DATE.toLocaleString(f.locale, { timeZone: f.timezone }) && p.numStr === (1234567.891).toLocaleString(f.locale),
    `${p.dateStr} | ${p.numStr}`);
  check(`${label}: navigator.userAgentData low-entropy values`, p.uad?.platform === f.clientHints.platform && p.uad?.mobile === false &&
    p.uad.brands.some((b) => b === `Google Chrome/${major(f)}`), JSON.stringify(p.uad));
  const he = p.he ?? {};
  check(`${label}: getHighEntropyValues`, he.platformVersion === f.clientHints.platformVersion && he.architecture === f.clientHints.architecture &&
    he.bitness === f.clientHints.bitness && he.uaFullVersion === f.browser.version && he.model === '' &&
    (he.fullVersionList ?? []).some((b) => b.brand === 'Google Chrome' && b.version === f.browser.version), JSON.stringify(he).slice(0, 200));
  expectWebGpu(label, f, p);
  // Informational only: workers are not wired for the persona.
  console.log(`info ${label}: dedicated worker sees ${JSON.stringify(p.worker)}`);
}
function expectHighEntropyHeaders(label, f, v) {
  const h = v.main?.headers ?? {};
  if (!h['sec-ch-ua-platform-version']) {
    skipCheck(`${label}: high-entropy client hint headers`, 'the browser did not send Sec-CH-UA-Platform-Version (Accept-CH not honored for this origin)');
    return;
  }
  check(`${label}: high-entropy hint headers are the persona's`, h['sec-ch-ua-platform-version'] === q(f.clientHints.platformVersion) &&
    h['sec-ch-ua-arch'] === q(f.clientHints.architecture) && h['sec-ch-ua-bitness'] === q(f.clientHints.bitness) &&
    (h['sec-ch-ua-full-version-list'] ?? '').includes(`"Google Chrome";v="${f.browser.version}"`), JSON.stringify({
      pv: h['sec-ch-ua-platform-version'], arch: h['sec-ch-ua-arch'], bit: h['sec-ch-ua-bitness'], fvl: h['sec-ch-ua-full-version-list'] }).slice(0, 220));
}

// 1+2. The open tab of A, navigated after the persona was set; B's tab is brand new (its FIRST navigation).
const a1 = await visit(A);
expectPersona('A (open tab, next navigation)', f1, a1);
const b1 = await visit(B);
expectPersona('B (first navigation)', f1, b1);
const a2 = await visit(A);
expectHighEntropyHeaders('A second navigation', f1, a2);

// 3. One persona for the whole profile: two agents, two partitions, and a throwaway agent present the same values.
check('A and B present the same persona (user agent, platform, languages, cores, screen, Accept-Language)',
  a1.main?.headers['user-agent'] === b1.main?.headers['user-agent'] && a1.page.platform === b1.page.platform &&
  JSON.stringify(a1.page.languages) === JSON.stringify(b1.page.languages) && a1.page.hc === b1.page.hc && a1.page.sw === b1.page.sw &&
  a1.main?.headers['accept-language'] === b1.main?.headers['accept-language'], `${a1.page.platform} / ${b1.page.platform}`);
check('the persona differs from the stock values it replaced (the probe can tell them apart)',
  stock.main?.headers['user-agent'] !== f1.userAgent && stock.page.hc !== f1.hardwareConcurrency, `${stock.main?.headers['user-agent']}`);
const th = await call('create-agent', { throwaway: true, name: 'Runtime throwaway' });
if (th.error) {
  skipCheck('throwaway agent shares the profile persona', th.error);
} else {
  made.push(th.id);
  expectPersona('throwaway (first navigation)', f1, await visit(th.id));
}

// 4. A persona change reaches the open tabs at their next navigation; clearing restores the stock values.
const set2 = await setFp({ browserVersion: '151.0.7100.3', locale: 'de-DE', timezone: 'Pacific/Auckland' });
const f2 = set2.fingerprint;
check('set-fingerprint patches the persona', f2?.browser?.version === '151.0.7100.3' && f2?.languages?.[0] === 'de-DE', JSON.stringify(set2.error ?? f2?.browser));
if (f2) {
  expectPersona('A after the change (open tab, next navigation)', f2, await visit(A));
  expectPersona('B after the change (open tab, next navigation)', f2, await visit(B));
}
// WebGPU policy: a persona that claims a WebGL GPU string is incompatible with the host adapter, even on the host's own os.
const hostPreset = { windows: 'windows-chrome', macos: 'macos-chrome', linux: 'linux-chrome' }[HOST_OS];
if (hostPreset) {
  const gl = { windows: { vendor: 'Google Inc. (NVIDIA)', renderer: 'ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)' },
    macos: { vendor: 'Google Inc. (Apple)', renderer: 'ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)' },
    linux: { vendor: 'Google Inc. (NVIDIA Corporation)', renderer: 'ANGLE (NVIDIA Corporation, NVIDIA GeForce RTX 3060/PCIe/SSE2, OpenGL 4.5.0)' } }[HOST_OS];
  const s = await setFp({ preset: hostPreset, webgl: gl });
  if (s.fingerprint) expectWebGpu('host-os persona with a webgl override', s.fingerprint, (await visit(A)).page);
  else skipCheck('host-os persona with a webgl override blocks WebGPU', s.error ?? 'set-fingerprint failed');
  const s2 = await setFp({ preset: hostPreset });
  if (s2.fingerprint) expectWebGpu('host-os persona without webgl', s2.fingerprint, (await visit(A)).page);
}
const clr = await setFp(null);
check('set-fingerprint null clears the persona', clr.fingerprint === null, JSON.stringify(clr.error ?? clr.fingerprint));
for (const id of [A, B]) {
  const v = await visit(id);
  check(`cleared persona (${id}): next navigation carries the stock user agent`, v.main?.headers['user-agent'] === stock.main?.headers['user-agent'] &&
    v.page.ua === v.main?.headers['user-agent'], `${v.main?.headers['user-agent']}`);
  check(`cleared persona (${id}): scripts report the stock cores/platform again`, v.page.hc === stock.page.hc && v.page.platform === stock.page.platform,
    `${v.page.hc} ${v.page.platform}`);
  check(`cleared persona (${id}): time zone and locale are the host's again`, v.page.tz === stock.page.tz && v.page.intlLocale === stock.page.intlLocale &&
    v.page.offJan === stock.page.offJan && v.page.offJul === stock.page.offJul, `${v.page.tz} ${v.page.intlLocale}`);
  check(`cleared persona (${id}): WebGPU is the stock host result again`, v.page.gpu === stock.page.gpu, `${v.page.gpu} (stock ${stock.page.gpu})`);
}
// An unknown IANA time zone is refused (the renderer would ignore it silently).
const bad = await setFp({ preset: 'linux-chrome', timezone: 'Mars/Olympus_Mons' });
check('set-fingerprint refuses an unknown IANA time zone', !!bad.error && /timezone/.test(bad.error), JSON.stringify(bad.error ?? bad.fingerprint?.timezone));

finish();

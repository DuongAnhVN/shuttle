// Per-agent proxy (docs/FEATURES.md "Per-agent proxy"): node scripts/smoke-proxy.mjs <port> [udd]
// Run by scripts/smoke-all.sh on its own launch; <udd> (the profile dir) enables the Preferences check.
// Everything is local: an origin server, three HTTP forward proxies (one with Basic auth, which also takes CONNECT)
// and one SOCKS5 proxy (no auth), all in this process. The origin is reached only through a proxy, as
// http://origin.test:<port> (the proxies map that host to 127.0.0.1); a direct request arrives as 127.0.0.1.
// Every proxy logs what it forwards (200) or challenges (407, with whether the request carried credentials); the checks
// compare those logs. test-proxy targets origin.test too: a non-loopback host the proxy resolves (test-proxy refuses
// loopback, localhost and userinfo URLs).
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { args, cdp, check as checkNamed, cleanup, finish, rpc, skipCheck, sleep } from './lib/smoke.mjs';

// lib's check is (name, ok, detail). This probe was written with both orders; a call whose first argument is not a
// string is (ok, name, detail). Without this, an (ok, name, ...) call passes whatever `ok` is (the name is truthy).
const check = (a, b, c) => (typeof a === 'string' ? checkNamed(a, b, c) : checkNamed(b, a, c));

const udd = args[0];
const SOCKS_MSG = 'SOCKS proxy with authentication is not supported';
// Throwaway credentials, generated per run; they only exist in this process and the browser.
const USER = `u${crypto.randomBytes(4).toString('hex')}`;
const PW = `pw-${crypto.randomBytes(12).toString('base64url')}`;
const WRONG = `${PW}-wrong`;
const B64 = (s) => Buffer.from(s).toString('base64');
const authed = (required, header) => !required || header === `Basic ${B64(`${USER}:${PW}`)}`;
const mapHost = (host) => host === 'origin.test';  // the only host the proxies forward

// Origin: records {tag, host} per request. A proxied hit has Host origin.test:<port>, a direct one 127.0.0.1:<port>.
const hits = [];
const origin = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  hits.push({ tag: u.searchParams.get('tag'), host: req.headers.host });
  res.writeHead(200, { 'Content-Type': 'text/html', 'Cache-Control': 'no-store' });
  res.end(`<title>${u.pathname}</title>ok`);
});
// WebSocket endpoint (ws:// goes through CONNECT on an HTTP proxy): answers the upgrade, then stays open.
origin.on('upgrade', (req, sock) => {
  const u = new URL(req.url, 'http://x');
  hits.push({ tag: u.searchParams.get('tag'), host: req.headers.host, ws: true });
  const accept = crypto.createHash('sha1').update(`${req.headers['sec-websocket-key']}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64');
  sock.on('error', () => {});
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
});
const listen = (srv) => new Promise((resolve) => srv.listen(0, '127.0.0.1', () => resolve(srv.address().port)));
const originPort = await listen(origin);
const ORIGIN = `http://origin.test:${originPort}`;
cleanup(() => origin.close());

// HTTP forward proxy: absolute-form GET forwarding, CONNECT tunnels, optional Basic auth (407 + Proxy-Authenticate).
function httpProxy(id, { auth = false } = {}) {
  const log = [];
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://origin.test');
    req.resume();
    if (!authed(auth, req.headers['proxy-authorization'])) {
      log.push({ status: 407, host: u.hostname, creds: req.headers['proxy-authorization'] != null });
      res.writeHead(407, { 'Proxy-Authenticate': 'Basic realm="smoke"', 'Content-Length': '0' });
      return res.end();
    }
    if (!mapHost(u.hostname)) { log.push({ status: 502, host: u.hostname }); res.writeHead(502); return res.end(); }
    log.push({ status: 200, host: u.hostname });
    const headers = { ...req.headers, host: u.host, 'x-smoke-proxy': id };
    delete headers['proxy-authorization'];
    delete headers['proxy-connection'];
    const up = http.request({ host: '127.0.0.1', port: originPort, method: req.method,
      path: u.pathname + u.search, headers }, (r) => { res.writeHead(r.statusCode, r.headers); r.pipe(res); });
    up.on('error', () => { if (!res.headersSent) res.writeHead(502); res.end(); });
    req.pipe(up);
  });
  srv.on('connect', (req, sock, head) => {
    const host = req.url.split(':')[0];
    if (!authed(auth, req.headers['proxy-authorization'])) {
      log.push({ kind: 'connect', status: 407, host });
      sock.end('HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="smoke"\r\nContent-Length: 0\r\n\r\n');
      return;
    }
    if (!mapHost(host)) { log.push({ kind: 'connect', status: 502, host }); sock.end('HTTP/1.1 502 Bad Gateway\r\n\r\n'); return; }
    log.push({ kind: 'connect', status: 200, host });
    const up = net.connect(originPort, '127.0.0.1', () => {
      sock.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head.length) up.write(head);
      sock.pipe(up);
      up.pipe(sock);
    });
    up.on('error', () => sock.destroy());
    sock.on('error', () => up.destroy());
  });
  srv.log = log;
  return srv;
}

// SOCKS5 proxy, no authentication (method 0), CONNECT only; the domain is resolved here (socks5 = remote DNS).
function socksProxy() {
  const log = [];
  const srv = net.createServer((c) => {
    c.on('error', () => c.destroy());
    c.once('data', (greet) => {
      if (greet[0] !== 5) return c.destroy();
      c.write(Buffer.from([5, 0]));
      c.once('data', (req) => {
        if (req[0] !== 5 || req[1] !== 1) return c.end(Buffer.from([5, 7, 0, 1, 0, 0, 0, 0, 0, 0]));
        let host;
        if (req[3] === 3) host = req.subarray(5, 5 + req[4]).toString();
        else if (req[3] === 1) host = [...req.subarray(4, 8)].join('.');
        else return c.end(Buffer.from([5, 8, 0, 1, 0, 0, 0, 0, 0, 0]));
        if (!mapHost(host)) { log.push({ status: 504, host }); return c.end(Buffer.from([5, 4, 0, 1, 0, 0, 0, 0, 0, 0])); }
        log.push({ status: 200, host });
        const up = net.connect(originPort, '127.0.0.1', () => {
          c.write(Buffer.from([5, 0, 0, 1, 0, 0, 0, 0, 0, 0]));
          up.pipe(c);
          c.pipe(up);
        });
        up.on('error', () => c.destroy());
      });
    });
  });
  srv.log = log;
  return srv;
}

const PX = { a: httpProxy('http-a'), b: httpProxy('http-b'), auth: httpProxy('http-auth', { auth: true }), s: socksProxy() };
const PORT = {};
for (const [k, srv] of Object.entries(PX)) { PORT[k] = await listen(srv); cleanup(() => srv.close()); }
// proxy value for a probe: 'a'|'b'|'auth' (http) or 's' (socks5); cred = password to embed, or none.
const pxUrl = (k, cred = '') => `${k === 's' ? 'socks5' : 'http'}://${cred ? `${USER}:${cred}@` : ''}127.0.0.1:${PORT[k]}`;
const fwd = (k) => PX[k].log.filter((e) => e.status === 200).length;
const challenged = (k) => PX[k].log.filter((e) => e.status === 407).length;
const snap = () => Object.fromEntries(Object.keys(PX).map((k) => [k, fwd(k)]));
const diff = (before, after) => Object.fromEntries(Object.keys(before).map((k) => [k, after[k] - before[k]]));
const same = (before, after) => Object.values(diff(before, after)).every((d) => d === 0);

// MCP calls. A tool error comes back as {error} (checks expect it); protocol errors throw. Every output is kept
// in `seen` for the password scan at the end.
const seen = [];
async function call(name, a = {}) {
  const { status, body } = await rpc('tools/call', { name, arguments: a });
  if (status !== 200 || !body?.result) throw new Error(`${name}: HTTP ${status} ${JSON.stringify(body?.error ?? body).slice(0, 200)}`);
  if (body.result.isError) {
    const error = body.result.content?.[0]?.text ?? 'error';
    seen.push(error);
    return { error };
  }
  const out = body.result.structuredContent ?? body.result;
  seen.push(JSON.stringify(out));
  return out;
}
const listAll = async () => (await call('list-agents')).agents;
const persistent = async () => (await listAll()).filter((a) => !a.throwaway);
const view = (list) => list.map((a) => [a.id, a.proxy ?? null]).sort((x, y) => (x[0] < y[0] ? -1 : 1));

// Agents and sessions this probe made; removed at exit.
const made = [];
const sessions = new Map();  // agent -> {sessionId, targetId, cdpUrl}
cleanup(async () => {
  for (const s of sessions.values()) await call('close-session', { sessionId: s.sessionId });
  for (const id of made) await call('delete-agent', { id });
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
// Navigates the agent's tab to a fresh origin URL (unique tag: no cache hit) and returns the origin's record of it.
let tagN = 0;
// navigate-tab can return while a navigation through a proxy is still in flight, so wait (up to waitMs) until the origin
// has recorded the request. A case that is expected to FAIL (wrong password, backoff) passes a short waitMs.
async function visit(agent, pathname = '/p', waitMs = 10000, base = ORIGIN) {
  const t = await tabOf(agent);
  const tag = `t${++tagN}`;
  const t0 = Date.now();
  const nav = await call('navigate-tab', { targetId: t.targetId, url: `${base}${pathname}?tag=${tag}` });
  await waitUntil(() => hits.some((h) => h.tag === tag), waitMs);
  return { nav, hit: hits.find((h) => h.tag === tag), ms: Date.now() - t0 };
}
async function evalIn(cdpUrl, expression, ms = 30000) {
  const c = await cdp(cdpUrl);
  try {
    const r = await c.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true }, undefined, ms);
    return r.result?.result?.value ?? r.result?.exceptionDetails?.text;
  } finally { c.close(); }
}
const waitUntil = async (fn, ms) => {
  for (const until = Date.now() + ms; Date.now() < until; await sleep(250)) if (await fn()) return true;
  return !!(await fn());
};

// 0. Tools exposed.
const names = (await rpc('tools/list')).body.result.tools.map((t) => t.name);
const NEW_TOOLS = ['set-proxy-all', 'assign-proxies', 'test-proxy'];
check(NEW_TOOLS.every((n) => names.includes(n)), 'tools/list has the proxy tools', NEW_TOOLS.filter((n) => !names.includes(n)).join(',') || 'all present');

// 1. Each agent through its own proxy; a proxy-less agent goes direct.
const A = 'smoke-px-a', B = 'smoke-px-b', C = 'smoke-px-c';
for (const id of [A, B, C]) { await call('delete-agent', { id }); }
const ca = await call('create-agent', { id: A, name: 'Proxy A', proxy: pxUrl('a') });
made.push(A);
check(ca.proxy?.port === PORT.a && ca.proxy?.hasPassword === false, 'create-agent with proxy (HTTP)', JSON.stringify(ca.proxy ?? ca.error));
const cb = await call('create-agent', { id: B, name: 'Proxy B', proxy: pxUrl('s') });
made.push(B);
check(cb.proxy?.scheme === 'socks5' && cb.proxy?.port === PORT.s, 'create-agent with proxy (SOCKS5)', JSON.stringify(cb.proxy ?? cb.error));
const cc = await call('create-agent', { id: C, name: 'Direct C' });
made.push(C);
check(cc.proxy === null, 'create-agent without proxy: proxy null', JSON.stringify(cc.proxy));

let before = snap();
const ra = await visit(A);
let d = diff(before, snap());
check(!!ra.hit && ra.hit.host === `origin.test:${originPort}` && d.a >= 1 && d.b === 0 && d.s === 0 && d.auth === 0 && d.b === 0,
  'agent A goes through its HTTP proxy only', JSON.stringify({ hit: ra.hit, d }));
before = snap();
const rb = await visit(B);
d = diff(before, snap());
check(!!rb.hit && rb.hit.host === `origin.test:${originPort}` && d.s >= 1 && d.a === 0 && d.b === 0 && d.auth === 0,
  'agent B goes through its SOCKS5 proxy only', JSON.stringify({ hit: rb.hit, d }));
before = snap();
// origin.test only resolves through a proxy, so the proxy-less agent visits the origin by its loopback address.
const rc = await visit(C, '/p', 10000, `http://127.0.0.1:${originPort}`);
d = diff(before, snap());
check(!!rc.hit && rc.hit.host === `127.0.0.1:${originPort}` && Object.values(d).every((x) => x === 0),
  'agent C without proxy goes direct (no proxy saw it)', JSON.stringify({ hit: rc.hit, d }));

// 2. Live change on an open tab: A's tab (opened above) moves to SOCKS, then back to HTTP; each next navigation follows.
before = snap();
const up1 = await call('update-agent', { id: A, proxy: pxUrl('s') });
check(up1.proxy?.scheme === 'socks5', 'update-agent switches A to SOCKS5', JSON.stringify(up1.proxy ?? up1.error));
const live1 = await visit(A);
d = diff(before, snap());
check(!!live1.hit && d.s >= 1 && d.a === 0, 'live change: next request on the open tab uses SOCKS5', JSON.stringify({ d }));
before = snap();
await call('update-agent', { id: A, proxy: pxUrl('a') });
const live2 = await visit(A);
d = diff(before, snap());
check(!!live2.hit && d.a >= 1 && d.s === 0, 'live change back: next request uses the HTTP proxy again', JSON.stringify({ d }));

// 3. SOCKS with credentials refused everywhere, nothing written, no password in the message.
const socksAuth = `socks5://${USER}:${PW}@127.0.0.1:${PORT.s}`;
const refusals = [
  ['create-agent', await call('create-agent', { id: 'smoke-px-bad', proxy: socksAuth })],
  ['create-agent (object form)', await call('create-agent', { id: 'smoke-px-bad', proxy: { scheme: 'socks5', host: '127.0.0.1', port: PORT.s, username: USER, password: PW } })],
  ['update-agent', await call('update-agent', { id: A, proxy: socksAuth })],
  ['set-proxy-all', await call('set-proxy-all', { proxy: socksAuth })],
  ['assign-proxies', await call('assign-proxies', { proxies: [pxUrl('b'), socksAuth], agentIds: [A, B] })],
];
for (const [tool, r] of refusals) {
  check(`SOCKS with auth refused by ${tool}`, !!r.error && r.error.includes(SOCKS_MSG) && !r.error.includes(PW), r.error ?? 'accepted');
}
check('refused create left no agent', !(await listAll()).some((a) => a.id === 'smoke-px-bad'));
check('refused update left A unchanged', (await listAll()).find((a) => a.id === A)?.proxy?.scheme === 'http');

// 3b. socks4 refused everywhere (it resolves DNS locally); a bypass list matching every host refused.
const socks4 = `socks4://127.0.0.1:${PORT.s}`;
const s4 = [
  ['create-agent', await call('create-agent', { id: 'smoke-px-bad', proxy: socks4 })],
  ['create-agent (object form)', await call('create-agent', { id: 'smoke-px-bad', proxy: { scheme: 'socks4', host: '127.0.0.1', port: PORT.s } })],
  ['update-agent', await call('update-agent', { id: A, proxy: socks4 })],
  ['set-proxy-all', await call('set-proxy-all', { proxy: socks4 })],
  ['assign-proxies', await call('assign-proxies', { proxies: [pxUrl('b'), socks4], agentIds: [A, B] })],
];
for (const [tool, r] of s4) {
  check(`socks4 refused by ${tool}`, !!r.error && r.error.includes('socks4 is not supported'), r.error ?? 'accepted');
}
const bypassAll = [
  await call('create-agent', { id: 'smoke-px-bad', proxy: { scheme: 'http', host: '127.0.0.1', port: PORT.b, bypass: ['*'] } }),
  await call('create-agent', { id: 'smoke-px-bad', proxy: { scheme: 'http', host: '127.0.0.1', port: PORT.b, bypass: ['*:*'] } }),
];
for (const r of bypassAll) {
  check('bypass entry matching every host refused', !!r.error && r.error.includes('A bypass entry that matches every host is not allowed'), r.error ?? 'accepted');
}
check('socks4 and bypass refusals wrote nothing',
  !(await listAll()).some((a) => a.id === 'smoke-px-bad') && (await listAll()).find((a) => a.id === A)?.proxy?.scheme === 'http');

// 4. set-proxy-all: one proxy copied to every persistent agent; one later edit changes only that agent.
const preexisting = (await persistent()).map((a) => a.id).filter((id) => ![A, B, C].includes(id));
cleanup(async () => { for (const id of preexisting) await call('update-agent', { id, proxy: null }); });
const all = await persistent();
const sp = await call('set-proxy-all', { proxy: pxUrl('auth', PW) });
const updatedOk = [...(sp.updated ?? [])].sort().join() === all.map((a) => a.id).sort().join();
check('set-proxy-all reports every persistent agent', updatedOk, JSON.stringify(sp.updated ?? sp.error));
let state = await persistent();
check('every persistent agent has the shared proxy (password stored, not shown)',
  state.every((a) => a.proxy?.port === PORT.auth && a.proxy?.username === USER && a.proxy?.hasPassword === true && !('password' in a.proxy)),
  JSON.stringify(state.map((a) => a.proxy)));
await call('update-agent', { id: C, proxy: pxUrl('b') });
state = await persistent();
check('editing C leaves the other agents on the shared proxy',
  state.find((a) => a.id === C)?.proxy?.port === PORT.b && state.filter((a) => a.id !== C).every((a) => a.proxy?.port === PORT.auth),
  JSON.stringify(state.map((a) => [a.id, a.proxy?.port])));

// 5. assign-proxies: enough, fewer (unchanged), fewer with reuse, more (unused), bad entry (nothing written).
const AB_C = [A, B, C];
let r = await call('assign-proxies', { proxies: [pxUrl('a'), pxUrl('b'), pxUrl('s')], agentIds: AB_C });
check('assign-proxies: one proxy per agent, in order',
  r.assigned?.map((x) => x.agentId).join() === AB_C.join() && r.unchanged?.length === 0 && r.unused?.length === 0 &&
  (await listAll()).find((x) => x.id === C)?.proxy?.scheme === 'socks5', JSON.stringify(r.error ?? r));
r = await call('assign-proxies', { proxies: [pxUrl('auth', PW)], agentIds: AB_C });
check('assign-proxies fewer: first agent gets it, the rest unchanged',
  r.assigned?.length === 1 && r.assigned[0].agentId === A && r.unchanged?.includes(B) && r.unchanged?.includes(C),
  JSON.stringify(r.error ?? r));
check('assign-proxies output hides the password', !JSON.stringify(r).includes(PW));
r = await call('assign-proxies', { proxies: [pxUrl('a'), pxUrl('b')], agentIds: AB_C, reuse: true });
const reuseOk = r.assigned?.map((x) => x.proxy?.port).join() === [PORT.a, PORT.b, PORT.a].join();
check('assign-proxies reuse: round-robin', reuseOk, JSON.stringify(r.error ?? r.assigned));
r = await call('assign-proxies', { proxies: [pxUrl('auth', PW), pxUrl('s'), pxUrl('b')], agentIds: [A, B] });
check('assign-proxies more: extra proxy listed as unused',
  r.assigned?.length === 2 && r.unused?.length === 1, JSON.stringify(r.error ?? { n: r.assigned?.length, u: r.unused?.length }));
const beforeBad = view(await listAll());
r = await call('assign-proxies', { proxies: [pxUrl('a'), 'ftp://127.0.0.1:1', pxUrl('b')], agentIds: AB_C });
check('assign-proxies bad entry: error names proxies[1]', !!r.error && r.error.includes('proxies[1]'), r.error ?? 'accepted');
r = await call('assign-proxies', { proxies: [pxUrl('a'), pxUrl('b'), socksAuth], agentIds: AB_C });
check('assign-proxies SOCKS with auth: error names proxies[2]', !!r.error && r.error.includes('proxies[2]') && r.error.includes(SOCKS_MSG), r.error ?? 'accepted');
check('bad lists write nothing', JSON.stringify(view(await listAll())) === JSON.stringify(beforeBad));

// 6. Proxy auth: correct password answered automatically (no prompt: the navigation commits and the origin is reached);
// a wrong one is tried once and then fails, without a 407 loop.
const D = 'smoke-px-d', E = 'smoke-px-e', G = 'smoke-px-g';
await call('create-agent', { id: D, name: 'Auth D', proxy: pxUrl('auth', PW) }); made.push(D);
await call('create-agent', { id: E, name: 'Auth E', proxy: pxUrl('auth', WRONG) }); made.push(E);
const closed = net.createServer();
const deadPort = await listen(closed);
await new Promise((r2) => closed.close(r2));
await call('create-agent', { id: G, name: 'Dead G', proxy: `http://127.0.0.1:${deadPort}` }); made.push(G);

before = snap();
const c407 = challenged('auth');
const rd = await visit(D);
d = diff(before, snap());
const d407 = challenged('auth') - c407;
check(!!rd.hit && rd.nav?.committed !== false && d.auth >= 1 && d407 >= 1 && d407 <= 2,
  'HTTP 407 answered automatically (correct password, one challenge)', JSON.stringify({ hit: !!rd.hit, nav: rd.nav, d407 }));
const t0 = Date.now();
const td = await call('test-proxy', { agentId: D, url: `${ORIGIN}/p?tag=tp-ok` });
check('test-proxy ok: status, body, latency, no password', td.ok === true && td.status === 200 &&
  String(td.body ?? '').includes('ok') && typeof td.latencyMs === 'number' && !JSON.stringify(td).includes(PW),
  JSON.stringify(td).slice(0, 200));
check('test-proxy finished quickly (no dialog wait)', Date.now() - t0 < 15000);

const c407e = challenged('auth');
const f0 = fwd('auth');
const te = await call('test-proxy', { agentId: E, url: `${ORIGIN}/p?tag=tp-bad` });
const e407 = challenged('auth') - c407e;
check('wrong password: test-proxy fails with proxyAuthFailed', te.ok === false && te.error?.proxyAuthFailed === true,
  JSON.stringify(te.error ?? te).slice(0, 200));
check('wrong password: detected per request, no 407 loop', e407 >= 1 && e407 <= 2 && fwd('auth') === f0, `407s=${e407}`);
check('wrong password error does not echo the password', !JSON.stringify(te).includes(PW) && !JSON.stringify(te).includes(WRONG));
// Backoff: the rejection starts a 60 s window with no credentials sent. A navigation inside it reaches the proxy
// without Proxy-Authorization and is challenged; test-proxy would reset the window, so this uses navigation.
const mark = PX.auth.log.length;
const rBackoff = await visit(E, '/p', 3000);
const inBackoff = PX.auth.log.slice(mark);
check('within the 60 s backoff no credentials are sent', inBackoff.length >= 1 && inBackoff.every((e) => e.status === 407 && !e.creds) && !rBackoff.hit,
  JSON.stringify({ nav: rBackoff.nav, hit: !!rBackoff.hit, log: inBackoff.slice(0, 4) }).slice(0, 300));
// Changing the agent's proxy clears the backoff: the same agent's next navigation carries the credentials again.
await call('update-agent', { id: E, proxy: pxUrl('auth', PW) });
const rFix = await visit(E);
check('changing the proxy clears the backoff', !!rFix.hit, JSON.stringify({ nav: rFix.nav, ms: rFix.ms }).slice(0, 160));

// WebSocket (CONNECT tunnel) through the auth proxy, agent D (correct password).
await visit(D, '/p');
const wsUrl = `ws://origin.test:${originPort}/ws?tag=ws-d`;
const wsRes = await evalIn((await tabOf(D)).cdpUrl, `new Promise((res) => { const w = new WebSocket('${wsUrl}');
  w.onopen = () => { res('open'); w.close(); }; w.onerror = () => res('error'); setTimeout(() => res('timeout'), 8000); })`);
check('WebSocket through the auth proxy (CONNECT) opens', wsRes === 'open', String(wsRes));

// test-proxy on a dead proxy: failure shape.
const tg = await call('test-proxy', { agentId: G, url: `${ORIGIN}/p?tag=tp-dead` });
check('test-proxy failure shape: ok false, error.code/message, latencyMs',
  tg.ok === false && typeof tg.error?.code === 'string' && typeof tg.error?.message === 'string' && typeof tg.latencyMs === 'number',
  JSON.stringify(tg).slice(0, 200));
const tu = await call('test-proxy', { agentId: 'smoke-px-nobody' });
check('test-proxy unknown agent: tool error', !!tu.error, JSON.stringify(tu).slice(0, 120));
// test-proxy only reaches public http(s) targets: loopback, localhost and userinfo URLs are refused, nothing is requested.
for (const [label, url] of [
  ['127.0.0.1', `http://127.0.0.1:${originPort}/p?tag=tp-lo`],
  ['localhost', `http://localhost:${originPort}/p?tag=tp-lh`],
  ['userinfo', `http://${USER}:${PW}@origin.test:${originPort}/p?tag=tp-ui`],
]) {
  const r = await call('test-proxy', { agentId: D, url });
  const tag = new URL(url).searchParams.get('tag');
  check(`test-proxy refuses ${label} target`, (!!r.error || r.ok === false) && !hits.some((h) => h.tag === tag),
    JSON.stringify(r).slice(0, 160));
}

// 7. Throwaway agent with a proxy: works, lives in memory only, gone with delete-agent.
const th = await call('create-agent', { throwaway: true, name: 'Proxy throwaway', proxy: pxUrl('b') });
check('throwaway agent takes a proxy', th.throwaway === true && th.proxy?.port === PORT.b, JSON.stringify(th.proxy ?? th.error));
if (th.id) {
  before = snap();
  const rt = await visit(th.id);
  d = diff(before, snap());
  check('throwaway agent goes through its proxy', !!rt.hit && d.b >= 1 && d.a === 0 && d.s === 0 && d.auth === 0, JSON.stringify(d));
  await call('delete-agent', { id: th.id });
  check('delete-agent removes the throwaway with its proxy', !(await listAll()).some((a) => a.id === th.id));
  sessions.delete(th.id);
}

// 8. Password: never in MCP output; in the profile's Preferences only encrypted (when <udd> is given).
const leaks = seen.filter((s) => s.includes(PW) || s.includes(WRONG));
check('password never appears in MCP output or errors', leaks.length === 0, `${leaks.length} outputs`);
if (udd) {
  const prefsDir = fs.existsSync(path.join(udd, 'Default')) ? path.join(udd, 'Default') : udd;
  const prefs = path.join(prefsDir, 'Preferences');
  const readPrefs = () => (fs.existsSync(prefs) ? fs.readFileSync(prefs, 'utf8') : '');
  const written = await waitUntil(async () => readPrefs().includes(D), 10000);
  check('Preferences holds the agent entries', written);
  const text = readPrefs();
  check('password absent from Preferences (plain and base64)', !text.includes(PW) && !text.includes(B64(PW)));
} else {
  skipCheck('password absent from Preferences', 'no <udd> argument');
}

// 9. WebRTC: in a proxied agent tab no candidate has a real local address. Control: an unproxied tab on the same
// machine must show one, else the check is not decidable here.
const realAddrs = Object.values(os.networkInterfaces()).flat()
  .filter((i) => !i.internal).map((i) => i.address.split('%')[0]);
const RTC_JS = `(async () => {
  try {
    const pc = new RTCPeerConnection({ iceServers: [] });
    pc.createDataChannel('x');
    await pc.setLocalDescription(await pc.createOffer());
    await new Promise((res) => { const t = setTimeout(res, 6000);
      pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); res(); } }; });
    const sdp = pc.localDescription.sdp; pc.close();
    return (sdp.match(/^a=candidate:.*$/gm) ?? []).map((l) => l.split(' '));
  } catch (e) { return { error: String(e) }; }
})()`;
async function candidates(agent, url) {
  const t = await tabOf(agent);
  await call('navigate-tab', { targetId: t.targetId, url });
  return evalIn(t.cdpUrl, RTC_JS, 20000);
}
const realOf = (cands) => (Array.isArray(cands) ? cands.filter((p) => realAddrs.includes(p[4])) : []);
const W = 'smoke-px-w', N = 'smoke-px-n';
await call('create-agent', { id: W, name: 'RTC proxied', proxy: pxUrl('a') }); made.push(W);
await call('create-agent', { id: N, name: 'RTC direct' }); made.push(N);
const controlCands = await candidates(N, `http://127.0.0.1:${originPort}/rtc`);
const proxiedCands = await candidates(W, `${ORIGIN}/rtc`);
if (realOf(controlCands).length === 0) {
  skipCheck('WebRTC leaks no real address through a proxied agent',
    `not decidable here: the unproxied control shows no candidate with a local address (${Array.isArray(controlCands) ? controlCands.length : controlCands?.error} candidates)`);
} else {
  check('WebRTC leaks no real address through a proxied agent', realOf(proxiedCands).length === 0 && !proxiedCands?.error,
    JSON.stringify({ proxied: Array.isArray(proxiedCands) ? proxiedCands.map((p) => `${p[7]}:${p[4]}`) : proxiedCands, control: realOf(controlCands).length }));
}

// Fail-closed for an unreadable stored proxy needs a tampered profile while the browser is down: not in this probe.
skipCheck('unreadable stored proxy fails closed', 'needs a tampered profile file (unit-level, not in this probe)');
skipCheck('password proxy refused while OS encryption is not ready', 'needs a launch where os_crypt is unavailable');

finish();

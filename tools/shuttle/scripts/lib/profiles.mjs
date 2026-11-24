// Shared by the isolated-profile probes (smoke-browser-profiles / -profile-routing / -profile-cdp / -profile-network /
// -profile-cookies). Contract: docs/PROFILES.md. These probes only run on a browser launched with
// --shuttle-isolated-profiles (scripts/smoke-all.sh `--with profiles`); they take the launch's user-data-dir as <arg> to
// read ShuttleManagerAdminToken. Every fixture is a throwaway Profile created here and stopped+deleted on cleanup; the
// startup profile and any real data are never touched. Never prints the token, a grant or a persona seed.
import { randomUUID } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { args, check, cleanup, rpc, skip } from './smoke.mjs';

export const udd = args[0];
export const SELECTOR = 'shuttle/profileId';
export const KEY_RE = /^bp_[0-9a-f]{32}$/;
export const FORGED_KEY = 'bp_00000000000000000000000000000000';

export function adminHeaders() {
  try {
    const t = readFileSync(join(udd, 'ShuttleManagerAdminToken'), 'utf8').trim();
    return t ? { 'X-Shuttle-Admin-Token': t } : null;
  } catch { return null; }
}
export const admin = udd ? adminHeaders() : null;
export const forgedAdmin = { 'X-Shuttle-Admin-Token': 'A'.repeat(43) };

// One tool call. Returns {status, ok, error:{code,message}|null, data, tools?}; never throws on a tool error.
export async function call(name, a = {}, { headers = {}, profile, meta } = {}) {
  const params = { name, arguments: a };
  if (profile !== undefined || meta) params._meta = { ...(profile !== undefined ? { [SELECTOR]: profile } : {}), ...meta };
  const { status, body } = await rpc('tools/call', params, headers);
  const r = body?.result;
  return { status, rpcError: body?.error ?? null, ok: !!r && !r.isError, error: r?.isError ? (r.structuredContent?.error ?? { code: null, message: r.content?.[0]?.text }) : null,
    data: r?.structuredContent ?? r ?? null };
}
export const adminCall = (name, a, o = {}) => call(name, a, { ...o, headers: { ...admin, ...(o.headers ?? {}) } });
export const toolNames = async (headers = {}) => (await rpc('tools/list', undefined, headers)).body?.result?.tools?.map((t) => t.name) ?? [];

// Skips the whole probe unless: an isolated-profile browser answers, the admin token was readable, the capability holds.
export async function requireIsolated() {
  if (!udd) await skip('usage: <port> <user-data-dir of an isolated-profiles launch>');
  if (!admin) await skip('no ShuttleManagerAdminToken in the user-data-dir (browser not started with --shuttle-isolated-profiles)');
  let caps;
  try { caps = (await call('get-capabilities')).data; } catch (e) { await skip(`no browser answering MCP (${e.message})`); }
  if (!caps) await skip('get-capabilities missing: this browser build predates isolated profiles');
  if (!(caps.isolatedProfiles?.supported && caps.isolatedProfiles?.enabled && caps.scopedRouting)) {
    await skip(`isolated profiles not enabled on this browser: ${JSON.stringify(caps.isolatedProfiles)} scopedRouting=${caps.scopedRouting}`);
  }
  return caps;
}

const PERSONA = { preset: 'linux-chrome', browserVersion: '150.0.7000.1', locale: 'fr-FR', languages: ['fr-FR', 'fr'], timezone: 'Asia/Tokyo',
  hardwareConcurrency: 6, deviceMemory: 4, screen: { width: 1366, height: 768, devicePixelRatio: 1, colorDepth: 24 } };
export const persona = (patch = {}) => ({ ...PERSONA, ...patch });

// Creates a throwaway isolated Profile (stopped), registers stop+delete cleanup; `load` makes it ready.
export async function makeProfile(workspaceAgent, { persona: p = persona(), proxy, load = true } = {}) {
  const requestId = randomUUID();
  const created = await adminCall('create-browser-profile', { requestId, workspaceAgent, persona: p, ...(proxy ? { proxy } : {}) });
  if (!created.ok) throw new Error(`create-browser-profile: ${JSON.stringify(created.error)}`);
  const key = created.data.profile.coreProfileKey;
  cleanup(async () => {
    await adminCall('stop-browser-profile', { coreProfileKey: key });
    const d = await adminCall('delete-browser-profile', { coreProfileKey: key });
    if (!d.ok && d.error?.code !== 'profile_not_found') throw new Error(`delete ${key}: ${d.error?.code}`);
  });
  if (load) {
    const l = await adminCall('load-browser-profile', { coreProfileKey: key });
    if (!l.ok) throw new Error(`load-browser-profile: ${JSON.stringify(l.error)}`);
  }
  return { key, requestId, workspaceAgent, record: created.data.profile };
}

// A scoped session on a ready profile: {sessionId, cdpEndpoint, targetId, cdpUrl}. Admin selects the profile; the
// returned endpoints carry the session's own grant (so they work without the admin token).
export async function scopedSession(profile, agent) {
  const s = await adminCall('open-session', { agent: agent ?? profile.workspaceAgent }, { profile: profile.key });
  if (!s.ok) throw new Error(`open-session: ${JSON.stringify(s.error)}`);
  const t = await adminCall('allocate-tab', { sessionId: s.data.sessionId }, { profile: profile.key });
  if (!t.ok) throw new Error(`allocate-tab: ${JSON.stringify(t.error)}`);
  cleanup(async () => { await adminCall('close-session', { sessionId: s.data.sessionId }, { profile: profile.key }); });
  return { sessionId: s.data.sessionId, cdpEndpoint: s.data.cdpEndpoint, targetId: t.data.targetId, cdpUrl: t.data.cdpUrl };
}

export const codeIs = (r, code) => !r.ok && r.error?.code === code;
// check() for "this call is refused with <code>" that also prints what came back (code only, never arguments).
export const refused = (name, r, code) => check(name, codeIs(r, code), `${r.status} ${r.error?.code ?? (r.ok ? 'ALLOWED' : r.error?.message)}`);
// No secret may appear in a result: no filesystem path, no seed, no token, no grant.
export function noSecrets(name, value, extra = []) {
  const s = JSON.stringify(value ?? '');
  const bad = [/[A-Za-z]:\\\\/, /\/(home|Users|tmp)\//, /"seed"/i, /ShuttleManagerAdminToken/, /grant=/, ...extra.map((x) => new RegExp(x))].filter((re) => re.test(s));
  return check(name, bad.length === 0, bad.length ? `matched ${bad.map(String).join(' ')}` : '');
}

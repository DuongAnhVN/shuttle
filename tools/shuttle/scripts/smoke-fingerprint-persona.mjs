// Fingerprint persona, profile-level persistence/schema (docs/FINGERPRINT.md):
//   node scripts/smoke-fingerprint-persona.mjs <port> <udd> [corrupt]
// Run by scripts/smoke-all.sh on its own launch of a profile seeded by smoke-fingerprint-persona-seed.mjs (<udd> is that
// profile directory; it is also read for the Preferences checks). Nothing here looks at pages: what pages and requests
// see is smoke-fingerprint-runtime.mjs; this probe checks the MCP surface and what is stored.
// The persona is ONE per Chromium profile (pref shuttle.fingerprint_persona), read and written with get-fingerprint /
// set-fingerprint; it is not an agent field and list-agents does not carry it.
//   default mode (valid seed):
//   1. the seeded persona is read; the legacy per-agent nested value is ignored and removed;
//   2. set / get / list / patch / preset / clear, seed kept, profile-level storage;
//   3. rejections: invalid values, unknown fields, incoherent combinations (nothing is written);
//   4. create-agent / update-agent refuse `fingerprint` (it is profile-level), agents carry no persona field;
//   5. the seed is in Preferences and in no MCP output.
//   corrupt mode (a corrupt seed): the persona is reported {unreadable: true}, never applied in part; a patch without a
//   preset is refused, a preset replaces it, null clears it.
import fs from 'node:fs';
import path from 'node:path';
import { args, check, cleanup, finish, rpc, sleep, skipCheck } from './lib/smoke.mjs';

const udd = args[0];
const corruptMode = args[1] === 'corrupt';
const SEED = '0123456789abcdef';  // the same constant as in smoke-fingerprint-persona-seed.mjs

// MCP calls. A tool error comes back as {error}; protocol errors throw. Every output is kept in `seen` for the seed scan.
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
const getFp = async () => (await call('get-fingerprint')).fingerprint;
const setFp = (fingerprint) => call('set-fingerprint', { fingerprint });
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const waitUntil = async (fn, ms) => {
  for (const until = Date.now() + ms; Date.now() < until; await sleep(250)) if (await fn()) return true;
  return !!(await fn());
};

// Preferences of the profile (null when unreadable or no <udd>). Chromium writes them lazily, so reads wait for a state.
const prefsFile = udd && path.join(fs.existsSync(path.join(udd, 'Default')) ? path.join(udd, 'Default') : udd, 'Preferences');
const readPrefs = () => {
  try { return JSON.parse(fs.readFileSync(prefsFile, 'utf8')); } catch { return null; }
};
const storedPersona = () => readPrefs()?.shuttle?.fingerprint_persona;

const made = [];
cleanup(async () => {
  await call('set-fingerprint', { fingerprint: null });
  for (const id of ['fp-legacy', ...made]) await call('delete-agent', { id });
});

// 0. The tools describe the argument; per-agent arguments are gone.
const tools = (await rpc('tools/list')).body.result.tools;
const setTool = tools.find((x) => x.name === 'set-fingerprint');
check('set-fingerprint schema has fingerprint.preset', !!setTool?.inputSchema?.properties?.fingerprint?.properties?.preset, JSON.stringify(setTool?.inputSchema ?? null).slice(0, 80));
check('set-fingerprint schema has no seed property', !JSON.stringify(setTool?.inputSchema ?? {}).toLowerCase().includes('seed'));
check('set-fingerprint schema refuses unknown keys (fingerprint and screen)',
  setTool?.inputSchema?.properties?.fingerprint?.additionalProperties === false &&
  setTool.inputSchema.properties.fingerprint.properties?.screen?.additionalProperties === false);
check('get-fingerprint exists', !!tools.find((x) => x.name === 'get-fingerprint'));
for (const name of ['create-agent', 'update-agent']) {
  const t = tools.find((x) => x.name === name);
  check(`${name} schema has no per-agent fingerprint`, !t?.inputSchema?.properties?.fingerprint);
}

if (corruptMode) {
  // Corrupt seeded persona.
  check('corrupt persisted persona is unreadable', same(await getFp(), { unreadable: true }), JSON.stringify(await getFp()));
  const noPreset = await setFp({ locale: 'de-DE' });
  check('patching an unreadable persona without a preset is refused', !!noPreset.error && /unreadable/.test(noPreset.error), noPreset.error ?? 'accepted');
  check('...and leaves it unreadable', same(await getFp(), { unreadable: true }));
  const repaired = await setFp({ preset: 'linux-chrome' });
  check('a preset replaces an unreadable persona', repaired.fingerprint?.os === 'linux' && repaired.fingerprint?.platform === 'Linux x86_64', JSON.stringify(repaired.fingerprint ?? repaired.error).slice(0, 120));
  await setFp(null);
  check('clearing keeps the unreadable active persona for this run', same(await getFp(), { unreadable: true }));
  await finish();  // exits the process
}

// 1. Persisted entry, as a previous run left it.
const seeded = await getFp();
check('persisted persona is read', seeded?.preset === 'windows-chrome' && seeded.os === 'windows' && seeded.version === 1, JSON.stringify(seeded ?? null).slice(0, 120));
check('read persona has the MCP shape (userAgent, clientHints, screen) and no seed',
  seeded?.userAgent?.includes('Windows NT 10.0') && seeded.clientHints?.platform === 'Windows' &&
  seeded.screen?.width === 1920 && seeded.browser?.version === '158.0.8127.18' && !('seed' in seeded));
const list0 = await call('list-agents');
check('list-agents carries no persona (get-fingerprint is the only reader)', !('fingerprint' in list0));
check('no agent entry carries a fingerprint field', (list0.agents ?? []).every((a) => !('fingerprint' in a)), JSON.stringify((list0.agents ?? []).map((a) => Object.keys(a))).slice(0, 120));
if (prefsFile) {
  check('the legacy per-agent persona was removed from shuttle.profiles',
    await waitUntil(() => readPrefs()?.shuttle?.profiles?.['fp-legacy'] && !('fingerprint_persona' in readPrefs().shuttle.profiles['fp-legacy']), 15000),
    JSON.stringify(readPrefs()?.shuttle?.profiles?.['fp-legacy'] ?? null).slice(0, 100));
  check('...and was not used as the profile persona (still the profile pref\'s windows persona)', seeded?.os === 'windows');
} else {
  skipCheck('legacy per-agent persona removed', 'no <udd> argument');
}

// 2. Set, get, patch, preset, clear.
const created = await setFp({ preset: 'macos-chrome', locale: 'fr-FR', timezone: 'Europe/Paris', hardwareConcurrency: 10, deviceMemory: 8, screen: { width: 1512, height: 982 } });
const fp = created.fingerprint;
check('set-fingerprint returns the persona', fp?.preset === 'macos-chrome' && fp.os === 'macos' && fp.platform === 'MacIntel' && fp.clientHints?.platform === 'macOS',
  JSON.stringify(fp ?? created.error).slice(0, 160));
check('overrides applied (locale, languages follow it, timezone, cores, memory, screen)',
  fp?.locale === 'fr-FR' && same(fp.languages, ['fr-FR', 'fr']) && fp.timezone === 'Europe/Paris' && fp.hardwareConcurrency === 10 &&
  fp.deviceMemory === 8 && fp.screen?.width === 1512 && fp.screen?.height === 982 && fp.screen?.colorDepth === 24, JSON.stringify(fp?.screen));
check('user agent is the reduced Mac one for the browser version',
  fp?.userAgent === `Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${fp.browser.version.split('.')[0]}.0.0.0 Safari/537.36`, fp?.userAgent);
check('set-fingerprint output has no seed', !('seed' in (fp ?? {})) && !JSON.stringify(created).toLowerCase().includes('"seed"'));
check('set saves for restart while get reports the active persona', created.applied === false && created.pendingRestart === true && same(await getFp(), seeded));

// Profile-level storage; the seed is only in Preferences (written lazily).
let seedA;
if (prefsFile) {
  check('Preferences holds the persona in shuttle.fingerprint_persona (snake_case, versioned)', await waitUntil(() => storedPersona()?.os === 'macos', 15000), JSON.stringify(storedPersona() ?? null).slice(0, 100));
  const stored = storedPersona();
  seedA = stored?.seed;
  check('stored seed is 16 lower-case hex digits', /^[0-9a-f]{16}$/.test(seedA ?? ''), String(seedA));
  check('stored persona is complete', ['version', 'seed', 'preset', 'os', 'platform', 'browser', 'user_agent', 'client_hints', 'locale', 'languages', 'timezone',
    'hardware_concurrency', 'device_memory', 'screen'].every((k) => k in (stored ?? {})) && stored.screen?.device_pixel_ratio === 2);
  check('nothing about the persona is stored per agent', !JSON.stringify(readPrefs()?.shuttle?.profiles ?? {}).includes('fingerprint'));
} else {
  skipCheck('Preferences holds the persona', 'no <udd> argument');
}

// Patch (no preset): only the given fields change; the rest and the seed stay.
const patched = await setFp({ timezone: 'Asia/Tokyo' });
const pfp = patched.fingerprint;
check('patch without a preset changes only the given fields',
  pfp?.timezone === 'Asia/Tokyo' && pfp.browser?.version === fp.browser?.version && pfp.os === 'macos' &&
  pfp.locale === 'fr-FR' && pfp.hardwareConcurrency === 10 && pfp.screen?.width === 1512, JSON.stringify(patched.error ?? pfp).slice(0, 160));
const patchedLocale = await setFp({ locale: 'de-DE' });
check('patching the locale moves the languages with it', patchedLocale.fingerprint?.locale === 'de-DE' && same(patchedLocale.fingerprint?.languages, ['de-DE', 'de']),
  JSON.stringify(patchedLocale.fingerprint?.languages ?? patchedLocale.error));
const patchedLanguages = await setFp({ languages: ['es-ES', 'es', 'en'] });
check('patching the languages moves the locale (the first language)', patchedLanguages.fingerprint?.locale === 'es-ES' && patchedLanguages.fingerprint?.languages?.length === 3,
  JSON.stringify(patchedLanguages.fingerprint?.locale ?? patchedLanguages.error));

// A preset regenerates (overrides dropped) but keeps the profile's seed: a stable identity.
const regenerated = await setFp({ preset: 'windows-chrome' });
check('a preset regenerates the persona (previous overrides dropped)', regenerated.fingerprint?.os === 'windows' && regenerated.fingerprint?.platform === 'Win32' &&
  regenerated.fingerprint?.locale === 'en-US' && regenerated.fingerprint?.timezone === 'America/New_York', JSON.stringify(regenerated.fingerprint?.timezone ?? regenerated.error));
if (prefsFile) {
  await waitUntil(() => storedPersona()?.os === 'windows', 15000);
  check('the seed survives patches and preset changes', storedPersona()?.seed === seedA && storedPersona()?.os === 'windows', `${storedPersona()?.os}`);
}
const rewritten = await setFp({ preset: 'windows-chrome', hardwareConcurrency: 12 });
check('same preset again is deterministic for the profile (same seed gives the same generated fields)',
  rewritten.fingerprint?.screen?.width === regenerated.fingerprint?.screen?.width && rewritten.fingerprint?.hardwareConcurrency === 12 &&
  rewritten.fingerprint?.clientHints?.platformVersion === regenerated.fingerprint?.clientHints?.platformVersion);

// Clear, then set again: a new seed.
const cleared = await setFp(null);
check('null clears the persona', cleared.fingerprint === null && same(await getFp(), seeded), JSON.stringify(cleared.fingerprint ?? cleared.error));
if (prefsFile) {
  check('cleared persona is removed from Preferences', await waitUntil(() => storedPersona() === undefined, 15000), JSON.stringify(storedPersona() ?? null).slice(0, 80));
}
const again = await setFp({ preset: 'linux-chrome' });
check('a persona can be set again after clearing', again.fingerprint?.os === 'linux' && again.fingerprint?.clientHints?.architecture === 'x86', JSON.stringify(again.fingerprint ?? again.error).slice(0, 100));
if (prefsFile && seedA) {
  await waitUntil(() => storedPersona()?.os === 'linux', 15000);
  check('clearing then setting draws a new seed', storedPersona()?.os === 'linux' && storedPersona()?.seed !== seedA, `${storedPersona()?.seed}`);
}
const missing = await call('set-fingerprint', {});
check('set-fingerprint without the argument is refused', !!missing.error && /fingerprint/.test(missing.error), missing.error ?? 'accepted');

// 3. Rejections: nothing is changed.
const before = again.fingerprint;
const REJECT = [
  ['unknown preset', { preset: 'beos-chrome' }, /preset/],
  ['non-string preset', { preset: 5 }, /preset/],
  ['unknown field (seed)', { preset: 'linux-chrome', seed: '0000000000000001' }, /unknown field: seed/],
  ['unknown field (userAgent)', { preset: 'linux-chrome', userAgent: 'x' }, /unknown field: userAgent/],
  ['unknown field (os)', { preset: 'linux-chrome', os: 'windows' }, /unknown field: os/],
  ['unknown field (clientHints)', { preset: 'linux-chrome', clientHints: {} }, /unknown field: clientHints/],
  ['unknown screen field', { preset: 'linux-chrome', screen: { depth: 24 } }, /unknown screen field: depth/],
  ['screen not an object', { screen: 5 }, /screen/],
  ['hardwareConcurrency too low', { hardwareConcurrency: 1 }, /hardwareConcurrency/],
  ['hardwareConcurrency not an integer', { hardwareConcurrency: 8.5 }, /hardwareConcurrency/],
  ['hardwareConcurrency too high', { hardwareConcurrency: 1000 }, /hardwareConcurrency/],
  ['deviceMemory not a power of two', { deviceMemory: 3 }, /deviceMemory/],
  ['deviceMemory a string', { deviceMemory: '8' }, /deviceMemory/],
  ['timezone with spaces', { timezone: 'not a zone!' }, /timezone/],
  ['timezone empty', { timezone: '' }, /timezone/],
  ['locale not a locale', { locale: 'english' }, /locale/],
  ['languages empty', { languages: [] }, /languages/],
  ['languages not strings', { languages: [1, 2] }, /languages/],
  ['languages first is not the locale', { locale: 'en-US', languages: ['fr-FR', 'en-US'] }, /first language/],
  ['languages repeat', { languages: ['en-US', 'en-US'] }, /languages/],
  ['browserVersion too old', { browserVersion: '99.0.0.0' }, /browserVersion/],
  ['browserVersion malformed', { browserVersion: '158.0' }, /browserVersion/],
  ['screen portrait', { screen: { width: 600, height: 900 } }, /landscape/],
  ['screen too small', { screen: { width: 100, height: 100 } }, /screen/],
  ['devicePixelRatio out of range', { screen: { devicePixelRatio: 9 } }, /devicePixelRatio/],
  ['devicePixelRatio not in quarter steps', { screen: { devicePixelRatio: 1.1 } }, /devicePixelRatio/],
  ['colorDepth', { screen: { colorDepth: 16 } }, /colorDepth/],
  // Coherence: combinations that no real browser has (none depends on the architecture a macOS preset draws).
  ['browserVersion with a non-zero minor', { browserVersion: '158.1.8127.18' }, /browserVersion/],
  ['browserVersion newer than the engine', { browserVersion: '300.0.8127.18' }, /browserVersion/],
  ['browserVersion with a tiny build', { browserVersion: '158.0.5.1' }, /browserVersion/],
  ['unknown language', { locale: 'xx-YY' }, /locale/],
  ['base language before its region language', { languages: ['en', 'en-US'] }, /before its base language/],
  ['desktop with 1 GB', { deviceMemory: 1 }, /deviceMemory/],
  ['many cores with little memory', { hardwareConcurrency: 32, deviceMemory: 4 }, /hardwareConcurrency/],
  ['desktop screen too small', { screen: { width: 800, height: 600 } }, /1024x600/],
  ['pixel ratio above 3 on a desktop', { preset: 'linux-chrome', screen: { devicePixelRatio: 3.5 } }, /devicePixelRatio/],
  ['fractional pixel ratio on macOS', { preset: 'macos-chrome', screen: { devicePixelRatio: 1.5 } }, /devicePixelRatio/],
  ['Direct3D WebGL on macOS', { preset: 'macos-chrome', webgl: { vendor: 'Google Inc. (NVIDIA)', renderer: 'ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)' } }, /webgl renderer does not match the os/],
  ['Metal WebGL on Windows', { preset: 'windows-chrome', webgl: { vendor: 'Google Inc. (Apple)', renderer: 'ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)' } }, /webgl renderer does not match the os/],
  ['OpenGL WebGL on Windows', { preset: 'windows-chrome', webgl: { vendor: 'Google Inc. (Intel)', renderer: 'ANGLE (Intel, Mesa Intel(R) UHD Graphics 630 (CFL GT2), OpenGL 4.6 (Core Profile) Mesa 23.2.1)' } }, /webgl renderer does not match the os/],
  ['WebGL mixing Direct3D and OpenGL on Linux', { preset: 'linux-chrome', webgl: { vendor: 'Google Inc. (Intel)', renderer: 'ANGLE (Intel, Direct3D11 vs_5_0 ps_5_0 OpenGL 4.6, D3D11)' } }, /mixes graphics backends/],
  ['WebGL vendor that is not the renderer\'s', { preset: 'linux-chrome', webgl: { vendor: 'Google Inc. (AMD)', renderer: 'ANGLE (NVIDIA Corporation, NVIDIA GeForce GTX 1660/PCIe/SSE2, OpenGL 4.5.0 NVIDIA 535.54.03)' } }, /vendor does not match/],
  ['WebGL renderer without a backend part', { preset: 'linux-chrome', webgl: { vendor: 'Google Inc. (Intel)', renderer: 'ANGLE (Intel, OpenGL 4.6)' } }, /ANGLE \(vendor, gpu, backend\)/],
  ['Apple GPU on Linux', { preset: 'linux-chrome', webgl: { vendor: 'Google Inc. (Apple)', renderer: 'ANGLE (Apple, Mesa OpenGL Apple M1, OpenGL 4.6)' } }, /Apple GPU/],
];
for (const [what, value, re] of REJECT) {
  const r = await setFp(value);
  check(`set-fingerprint rejects ${what}`, !!r.error && r.error.startsWith('fingerprint: ') && re.test(r.error), r.error ?? 'accepted');
}
check('rejected updates left the saved persona intact', same((await setFp({})).fingerprint, before));
const notObject = await setFp('windows-chrome');
check('set-fingerprint rejects a non-object fingerprint', !!notObject.error && notObject.error.startsWith('fingerprint: '), notObject.error ?? 'accepted');
await setFp(null);
const noPreset = await setFp({ locale: 'en-US' });
check('without a persona, a preset is required', !!noPreset.error && /preset is required/.test(noPreset.error), noPreset.error ?? 'accepted');

// 3b. Coherent generation and accepted overrides.
const GPUS = {
  'windows-chrome': { vendor: 'Google Inc. (NVIDIA)', renderer: 'ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002503) Direct3D11 vs_5_0 ps_5_0, D3D11)' },
  'linux-chrome': { vendor: 'Google Inc. (Intel)', renderer: 'ANGLE (Intel, Mesa Intel(R) UHD Graphics 630 (CFL GT2), OpenGL 4.6 (Core Profile) Mesa 23.2.1)' },
  'macos-chrome': { vendor: 'Google Inc. (Apple)', renderer: 'ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)' },
};
for (const preset of Object.keys(GPUS)) {
  // The seed is not settable over MCP, so a macOS draw may be Apple Silicon or Intel: the checks hold for both.
  const g = (await setFp({ preset })).fingerprint;
  check(`${preset}: generated persona is coherent (hints, hardware, screen)`,
    g && g.clientHints.bitness === '64' && g.clientHints.mobile === false && g.screen.width >= 1024 && g.screen.height >= 600 && g.deviceMemory >= 2 &&
    (g.os !== 'macos' || (g.clientHints.architecture !== 'arm' || (g.hardwareConcurrency >= 8 && g.deviceMemory === 8 && g.screen.devicePixelRatio === 2))) &&
    (g.os === 'macos' || g.clientHints.architecture === 'x86'), JSON.stringify(g ?? null).slice(0, 200));
  check(`${preset}: a preset does not generate webgl`, g && !('webgl' in g));
  const w = await setFp({ preset, webgl: GPUS[preset] });
  check(`${preset}: a webgl pair of the os is accepted`, same(w.fingerprint?.webgl, GPUS[preset]), JSON.stringify(w.error ?? w.fingerprint?.webgl));
  check(`${preset}: a preset again clears webgl`, !('webgl' in ((await setFp({ preset })).fingerprint ?? {})));
}
const ja = (await setFp({ preset: 'windows-chrome', locale: 'ja-JP' })).fingerprint;
check('a known locale with a preset brings its usual time zone and languages', ja?.timezone === 'Asia/Tokyo' && same(ja.languages, ['ja-JP', 'ja']), JSON.stringify([ja?.timezone, ja?.languages]));
const jaParis = (await setFp({ preset: 'windows-chrome', locale: 'ja-JP', timezone: 'Europe/Paris' })).fingerprint;
check('an explicit time zone wins over the locale\'s', jaParis?.timezone === 'Europe/Paris' && jaParis.locale === 'ja-JP', JSON.stringify(jaParis?.timezone));
const lisbon = (await setFp({ preset: 'linux-chrome', locale: 'pt-PT' })).fingerprint;
check('a valid locale the generator has no time zone for keeps the default one', lisbon?.locale === 'pt-PT' && lisbon.timezone === 'America/New_York', JSON.stringify([lisbon?.locale, lisbon?.timezone]));
const regional = (await setFp({ languages: ['en-GB', 'en', 'fr'] })).fingerprint;
check('a region language before its base language is accepted', same(regional?.languages, ['en-GB', 'en', 'fr']) && regional.locale === 'en-GB');

// 4. Per-agent fingerprint is gone: the arguments are refused, whatever the agent.
const createRefused = await call('create-agent', { id: 'smoke-fp-agent', fingerprint: { preset: 'linux-chrome' } });
check('create-agent refuses fingerprint (profile-level)', !!createRefused.error && /profile-level/.test(createRefused.error) && /set-fingerprint/.test(createRefused.error), createRefused.error ?? 'accepted');
check('...and creates no agent', !(await call('list-agents')).agents.some((a) => a.id === 'smoke-fp-agent'));
const agent = await call('create-agent', { id: 'smoke-fp-agent', name: 'Plain agent' });
made.push('smoke-fp-agent');
check('an agent has no fingerprint field of its own', agent.error === undefined && !('fingerprint' in agent), JSON.stringify(Object.keys(agent)));
const updateRefused = await call('update-agent', { id: 'smoke-fp-agent', fingerprint: null });
check('update-agent refuses fingerprint (profile-level)', !!updateRefused.error && /profile-level/.test(updateRefused.error), updateRefused.error ?? 'accepted');
const th = await call('create-agent', { throwaway: true, name: 'Throwaway', fingerprint: { preset: 'linux-chrome' } });
check('a throwaway agent has no persona of its own either', !!th.error && /profile-level/.test(th.error), th.error ?? 'accepted');
await setFp({ preset: 'windows-chrome' });
const plain = await call('create-agent', { throwaway: true, name: 'Throwaway shares the profile' });
if (plain.error) {
  skipCheck('throwaway agent shares the profile persona', plain.error);
} else {
  made.push(plain.id);
  check('a throwaway agent reports no persona and the profile keeps its one', !('fingerprint' in plain) && same(await getFp(), seeded));
}

// 5. The seed: in Preferences, in no MCP output.
const seeds = new Set([SEED]);
if (seedA) seeds.add(seedA);
if (prefsFile && storedPersona()?.seed) seeds.add(storedPersona().seed);
const leaks = [...seeds].filter((s) => seen.some((out) => out.includes(s)));
check('no seed appears in any MCP output or error', leaks.length === 0, `${seen.length} outputs, ${seeds.size} seeds`);
check('the seeded persona read back with its stored seed unknown to MCP', !seen.some((out) => out.includes(SEED)));

finish();

// Seeds the fingerprint-persona probe's profile before its launch:
//   node scripts/smoke-fingerprint-persona-seed.mjs <userDataDir> [valid|badversion|badfield|extrakey|incoherent|notdict]
// (run by scripts/smoke-all.sh; smoke-fingerprint-persona.mjs checks how the browser read what is written here).
// The fingerprint persona is ONE per Chromium profile: the profile pref shuttle.fingerprint_persona, written straight
// into Preferences the way a previous run would have stored it. Modes:
//   - valid (default): a valid persona (windows-chrome), seed SEED below;
//   - badversion:      version 2 (unknown): corrupt;
//   - badfield:        hardware_concurrency is a string: corrupt;
//   - extrakey:        a key the schema does not have: corrupt;
//   - incoherent:      every field well formed, but the user agent says Mac while the os is windows: corrupt;
//   - notdict:         the value is a string: corrupt.
// A corrupt persona must be reported as unreadable, never applied in part and never silently dropped.
// It also writes a LEGACY per-agent persona nested in shuttle.profiles["fp-legacy"].fingerprint_persona (a draft
// layout that never shipped): the browser must ignore it and remove it, never read it as the profile's persona.
import fs from 'node:fs';
import path from 'node:path';

const SEED = '0123456789abcdef';  // the same constant as in smoke-fingerprint-persona.mjs
const udd = process.argv[2];
const mode = process.argv[3] ?? 'valid';
if (!udd) { console.error('usage: smoke-fingerprint-persona-seed.mjs <userDataDir> [mode]'); process.exit(2); }

const valid = () => ({
  version: 1,
  seed: SEED,
  preset: 'windows-chrome',
  os: 'windows',
  platform: 'Win32',
  browser: { brand: 'Chrome', version: '158.0.8127.18' },
  user_agent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/158.0.0.0 Safari/537.36',
  client_hints: { platform: 'Windows', platform_version: '10.0.0', architecture: 'x86', bitness: '64', mobile: false },
  locale: 'en-US',
  languages: ['en-US', 'en'],
  timezone: 'America/New_York',
  hardware_concurrency: 8,
  device_memory: 8,
  screen: { width: 1920, height: 1080, device_pixel_ratio: 1, color_depth: 24 },
});

const personas = {
  valid: valid(),
  badversion: { ...valid(), version: 2 },
  badfield: { ...valid(), hardware_concurrency: '8' },
  extrakey: { ...valid(), canvas_noise: true },
  incoherent: {
    ...valid(),
    user_agent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/158.0.0.0 Safari/537.36',
  },
  notdict: 'windows-chrome',
};
if (!(mode in personas)) { console.error(`unknown mode ${mode}`); process.exit(2); }

const prefs = {
  shuttle: {
    fingerprint_persona: personas[mode],
    profiles: { 'fp-legacy': { name: 'Legacy persona', fingerprint_persona: { ...valid(), preset: 'macos-chrome' } } },
  },
};
const prof = path.join(udd, 'Default');
fs.mkdirSync(prof, { recursive: true });
fs.writeFileSync(path.join(prof, 'Preferences'), JSON.stringify(prefs));

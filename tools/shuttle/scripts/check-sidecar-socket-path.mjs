#!/usr/bin/env node
// Static regression check for the page-tool sidecar's unix socket path (no
// build, no browser): <tmp>/acab-<16 hex>/s.sock must fit sun_path, whatever
// TMPDIR is. Models SessionDir() in chrome/browser/shuttle/page_tools.cc and
// fails when the C++ constants it models drift. Usage: node check-sidecar-socket-path.mjs
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const src = join(dirname(fileURLToPath(import.meta.url)), '../../../chrome/browser/shuttle');
const pageTools = readFileSync(join(src, 'page_tools.cc'), 'utf8');
const sidecarH = readFileSync(join(src, 'agent_browser_sidecar.h'), 'utf8');
const sidecarCc = readFileSync(join(src, 'agent_browser_sidecar.cc'), 'utf8');

let failed = 0;
const check = (ok, msg) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${msg}`); if (!ok) failed++; };

// Constants as in the C++ (asserted against the sources below).
const MAX = Number(/kMaxSocketPathBytes = (\d+);/.exec(pageTools)?.[1]);
const SESSION = /kDaemonSessionName\[\] = "([^"]+)";/.exec(sidecarH)?.[1];
const HEX = Number(/"acab-"[\s\S]{0,200}?\.substr\(0, (\d+)\)/.exec(pageTools)?.[1]);

check(MAX === 100, `kMaxSocketPathBytes is 100 (found ${MAX}); sun_path is 104 on macOS, NUL included`);
check(SESSION === 's', `daemon session name is short and constant (found ${SESSION})`);
check(HEX === 16, `session dir uses 16 hex of SHA-256 (found ${HEX})`);
check(/environment\["AGENT_BROWSER_SESSION"\] = kDaemonSessionName;/.test(sidecarCc),
  'sidecar passes the constant AGENT_BROWSER_SESSION, not the session id');
check(/environment\["AGENT_BROWSER_NAMESPACE"\] = "";/.test(sidecarCc),
  'sidecar unsets AGENT_BROWSER_NAMESPACE for the child');
check(!/environment\["AGENT_BROWSER_SESSION"\] = config\.session_id/.test(sidecarCc),
  'AGENT_BROWSER_SESSION no longer the session UUID');

const sessionDir = (temp) => {
  const dir = `${temp.replace(/\/+$/, '')}/acab-${'0'.repeat(HEX)}`;
  return `${dir}/${SESSION}.sock`.length <= MAX ? dir : `/tmp/acab-${'0'.repeat(HEX)}`;
};
const socketLen = (temp) => Buffer.byteLength(`${sessionDir(temp)}/${SESSION}.sock`);

const uuid = '3f2b8c1e-9d4a-4e6b-8a7c-1d2e3f4a5b6c';
const old = (temp) => Buffer.byteLength(`${temp}/acab/${uuid}/${uuid}.sock`);

for (const [label, temp] of [
  ['short Linux', '/tmp'],
  ['macOS per-user', '/var/folders/ab/cdefghijklmnopqrstuvwxyz0123456789/T'],
  ['observed long (44 bytes)', '/' + 'x'.repeat(43)],
  ['71 bytes (last that stays)', '/' + 'x'.repeat(70)],
  ['128 bytes', '/' + 'x'.repeat(127)],
  ['256 bytes', '/' + 'x'.repeat(255)],
  ['trailing slash', '/var/folders/ab/cdefghijklmnopqrstuvwxyz0123456789/T/'],
]) {
  const n = socketLen(temp);
  check(n <= MAX, `${label} (${Buffer.byteLength(temp)} bytes TMPDIR): socket path ${n} bytes (was ${old(temp)}), limit ${MAX}`);
}
check(old('/' + 'x'.repeat(43)) > 103, 'sanity: the old layout overflows at the observed temp dir');
check(socketLen('/tmp') === '/tmp/acab-'.length + HEX + '/s.sock'.length, 'short TMPDIR keeps the temp dir (no fallback)');

process.exit(failed ? 1 : 0);

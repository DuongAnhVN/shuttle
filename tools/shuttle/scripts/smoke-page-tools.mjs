// Page tool screenshot checks (docs/MCP.md "screenshot"): node scripts/smoke-page-tools.mjs <port>
// Run by scripts/smoke-all.sh on its own launch with --force-device-scale-factor=2 (Retina sizes in headless).
//   1. Viewport screenshot in CSS px (default JPEG), and maxSize on the longer edge.
//   2. A full page of noise whose 2x PNG is over agent-browser's 10 MiB inline limit still returns an image (the
//      browser reads the capture file itself): CSS scale 1280x1000, device + maxSize 1568 gives 1568x1225.
//   3. scale device + format png of that page (no rescale, over 10 MB): a clear error, never success without image.
//   4. Six concurrent screenshots: at most 4 admitted, the rest "Busy"; admitted ones carry images; a later call works.
//   0. A snapshot shows the page's body marker (sidecar read path).
//   5. The session's sidecar directory (acab-<hash>, private, <dir>/s.sock, no namespace dir) exists and holds no
//      capture files in its shots directory.
import { createHash } from 'node:crypto';
import { existsSync, readdirSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { check, cleanup, finish, gone, mcp, rpc, skip } from './lib/smoke.mjs';

const AGENT = 'smoke-pt';
let sessionId;
cleanup(async () => {
  if (sessionId) await mcp('close-session', { sessionId }).catch(gone);
  await mcp('delete-agent', { id: AGENT }).catch(gone);
});

// Raw call: keeps isError results (mcp() throws on them).
const call = async (name, args) => (await rpc('tools/call', { name, arguments: args })).body?.result ?? { isError: true };
const text = (res) => String(res?.content?.find((p) => p.type === 'text')?.text ?? '');
const image = (res) => {
  const part = res?.content?.find((p) => p.type === 'image');
  if (!part) return null;
  const buf = Buffer.from(part.data, 'base64');
  if (buf[0] === 0x89) return { mime: part.mimeType, w: buf.readUInt32BE(16), h: buf.readUInt32BE(20), bytes: buf.length };
  for (let i = 2; i + 9 < buf.length;) {
    const m = buf[i + 1], len = buf.readUInt16BE(i + 2);
    if (m >= 0xc0 && m <= 0xc2) return { mime: part.mimeType, w: buf.readUInt16BE(i + 7), h: buf.readUInt16BE(i + 5), bytes: buf.length };
    i += 2 + len;
  }
  return { mime: part.mimeType, w: 0, h: 0, bytes: buf.length };
};
const desc = (res) => JSON.stringify(image(res) ?? { isError: !!res.isError, text: text(res).slice(0, 120) });
// A successful screenshot always carries an image.
const sound = (res) => res.isError || !!image(res);

await mcp('create-agent', { id: AGENT, name: 'Smoke page tools' }).catch(async (e) => {
  if (!/exists/.test(e.message)) throw e;
});
({ sessionId } = await mcp('open-session', { agent: AGENT }));
const { targetId } = await mcp('allocate-tab', { sessionId });
await mcp('set-tab-viewport', { targetId, width: 1280, height: 800 });
const MARKER = 'pt-marker-7f3a91';
const plain = 'data:text/html,' + encodeURIComponent(`<body style="margin:0;font:16px sans-serif"><h1>${MARKER}</h1>` +
  '<p>line</p>'.repeat(20) + '</body>');
await mcp('open', { targetId, url: plain });

// 1. Viewport.
const first = await call('screenshot', { targetId });
if (/agent-browser is not installed/.test(text(first))) skip('no agent-browser sidecar');
const dpr = (await mcp('eval', { targetId, script: 'devicePixelRatio' }).catch(() => null));
const dprText = JSON.stringify(dpr);
check('the launch emulates a 2x display', /\b2\b/.test(dprText), dprText.slice(0, 80));
const v = image(first);
check('viewport screenshot: CSS px JPEG', v?.mime === 'image/jpeg' && v.w === 1280 && v.h === 800, desc(first));
// The sidecar's read path (daemon socket up, tab bound): the snapshot shows the page's body marker.
const snap = await call('snapshot', { targetId });
check('snapshot reads the page body marker', !snap.isError && text(snap).includes(MARKER), text(snap).slice(0, 120));
const small = await call('screenshot', { targetId, maxSize: 640 });
const s = image(small);
check('maxSize 640: longer edge 640', s?.w === 640 && s.h === 400, desc(small));

// 2. Noise: canvas at device resolution, so the 2x PNG doesn't compress (2560x2000 px, ~15-20 MB).
const noise = 'data:text/html,' + encodeURIComponent(`<body style="margin:0"><canvas id=c></canvas><script>
const c = document.getElementById('c'), d = devicePixelRatio, w = 1280, h = 1000;
c.width = w * d; c.height = h * d; c.style.width = w + 'px'; c.style.height = h + 'px'; c.style.display = 'block';
const x = c.getContext('2d'), img = x.createImageData(c.width, c.height), u = new Uint32Array(img.data.buffer);
let r = 12345; for (let i = 0; i < u.length; i++) { r ^= r << 13; r ^= r >>> 17; r ^= r << 5; u[i] = r | 0xff000000; }
x.putImageData(img, 0, 0); document.title = 'noise';
</script></body>`);
await mcp('open', { targetId, url: noise });
const big = await call('screenshot', { targetId, fullPage: true });
const b = image(big);
check('noise full page (2x PNG > 10 MiB), CSS scale: image 1280x1000',
  b?.mime === 'image/jpeg' && Math.abs(b.w - 1280) <= 1 && Math.abs(b.h - 1000) <= 1, desc(big));
const fit = await call('screenshot', { targetId, fullPage: true, scale: 'device', maxSize: 1568 });
const f = image(fit);
check('noise full page, device + maxSize 1568: image 1568x1225', !!f && f.w === 1568 && Math.abs(f.h - 1225) <= 1, desc(fit));

// 3. Device PNG, no rescale: over 10 MB.
const raw = await call('screenshot', { targetId, fullPage: true, scale: 'device', format: 'png' });
check('noise full page, device PNG: a clear error (over 10 MB)', raw.isError && /10 MB/.test(text(raw)), desc(raw));

// 4. Admission: six at once.
const many = await Promise.all(Array.from({ length: 6 }, () => call('screenshot', { targetId, fullPage: true })));
const busy = many.filter((r) => r.isError && /^Busy/.test(text(r))).length;
check('six concurrent screenshots: 2+ busy, the rest images', busy >= 2 && many.every(sound) &&
  many.filter((r) => image(r)).length === 6 - busy, many.map(desc).join(' | ').slice(0, 300));
const after = await call('screenshot', { targetId });
check('a screenshot after the burst works (slots released)', !!image(after), desc(after));

// 5. Files. The sidecar dir (PageToolManager's SessionDir) is acab-<first 16 hex of SHA-256(session id)> in the
// browser's temp dir, or in /tmp when <tmp>/acab-<16>/s.sock would pass 100 bytes. The browser and this probe share
// TMPDIR (smoke-all.sh); look in both and fail when the directory is nowhere (the old <tmp>/acab/<id> is gone).
const SOCKET_MAX = 100;
const dirName = `acab-${createHash('sha256').update(sessionId).digest('hex').slice(0, 16)}`;
const dir = [join(tmpdir(), dirName), join('/tmp', dirName)].find((d) => existsSync(d));
check('the session sidecar directory exists (hashed name, temp dir or /tmp)', !!dir, `${dirName} in ${tmpdir()} or /tmp`);
if (dir) {
  const mode = statSync(dir).mode & 0o7777;
  check('sidecar directory is private (0700)', mode === 0o700, mode.toString(8));
  // agent-browser 0.41.2: <socket dir>/<session>.sock; a namespace would nest <socket dir>/<ns>/<session>.sock.
  const entries = readdirSync(dir, { withFileTypes: true });
  const nested = entries.filter((e) => e.isDirectory() && e.name !== 'shots').map((e) => e.name);
  check('daemon socket is <dir>/s.sock: constant session, no namespace directory', nested.length === 0 &&
    entries.some((e) => e.name === 's.sock'), entries.map((e) => e.name).join(' '));
  check(`socket path fits ${SOCKET_MAX} bytes`, Buffer.byteLength(join(dir, 's.sock')) <= SOCKET_MAX, join(dir, 's.sock'));
  const shots = join(dir, 'shots');
  const left = existsSync(shots) ? readdirSync(shots).filter((n) => n.startsWith('shot-')) : [];
  check('no capture files left in the shots directory', left.length === 0, left.join(' '));
} else {
  check('no capture files left in the shots directory', false, 'sidecar directory not found');
}

finish();

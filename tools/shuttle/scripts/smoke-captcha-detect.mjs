// CAPTCHA native detection smoke probe (docs/MCP.md: "CAPTCHA detection").
// node scripts/smoke-captcha-detect.mjs <port>
// Runs on smoke-all's main launch and needs NO agent-browser sidecar: the
// seven captcha tools work inside the browser (isolated-world scripts +
// trusted input through the widget host). Local fixtures on two HTTP ports
// (so the iframe rule matches a real frame URL):
//   /main   puzzle-slide widget in the main frame. The ratio maths is exact:
//           p=0.5 travels 0.5*300*(300-40)/(300-60) = 162.5 px, the fixture
//           maps that to a 150 px piece move and the page shows SOLVED-4k.
//   /embed  the same widget inside an <iframe> served from the OTHER port;
//           the frame page narrows the background at 2.5 s (a live state
//           change: same challengeId, new stateRevision, one re-fire).
//   /late   the widget mounted 1.2 s after load (no further navigation).
// Checks: set/get rules, atomic rejection, probe, challenge-ready events
// (main + iframe + late), get-captcha-challenge (images + geometry), a
// verified solve, the tab lock over session CDP (Input refused while
// solving), the revision bump on a changed picture, a stale answer with the
// old revision (no input), a second attempt refused captcha-in-progress,
// cancel releasing the tab, and gone events (navigation, tab close).
import { cdp, check, cleanup, finish, gone, mcp, port, rpc, sleep } from './lib/smoke.mjs';

const AGENT = 'smoke-captcha';
const PNG1 =
  'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';

// One fixture builder for all pages. The widget mounts after `delay` ms
// (late-mount case) and `swap` narrows the background at 2.5 s (the
// image-changed case). Ratio maths: the track is 300 wide with a 40 handle,
// the background 300 with a 60 piece; the piece follows the handle by
// 240/260, so a 162.5 px handle travel lands the piece at exactly 150 px and
// the fixture shows SOLVED-4k.
const page = ({ delay = 0, swap = false } = {}) => `<!doctype html>
<body style="margin:0;font:14px monospace"><div id="hello">captcha-free page</div>
<div id="holder"></div>
<script>
const mount = () => {
  document.getElementById('holder').innerHTML =
    '<div id="pz" style="position:relative;width:300px;margin:40px 0 0 40px">' +
    '<img id="bg" src="${PNG1}" style="width:300px;height:120px;display:block">' +
    '<img id="piece" src="${PNG1}" style="position:absolute;left:0;top:30px;' +
    'width:60px;height:60px">' +
    '<div id="track" style="position:relative;width:300px;height:20px;' +
    'background:#ccc;margin-top:8px">' +
    '<div id="handle" style="position:absolute;left:0;top:0;width:40px;' +
    'height:20px;background:#36c"></div><div id="log"></div></div>';
  const handle = document.getElementById('handle');
  const piece = document.getElementById('piece');
  const log = document.getElementById('log');
  let down = null, dx = 0;
  document.addEventListener('mousedown', (e) => {
    if (e.target === handle) { down = e.clientX; dx = 0; }
  });
  document.addEventListener('mousemove', (e) => {
    if (down === null) return;
    dx = Math.max(0, Math.min(260, e.clientX - down));
    handle.style.left = dx + 'px';
    piece.style.left = Math.round(dx * 240 / 260) + 'px';
  });
  document.addEventListener('mouseup', () => {
    if (down === null) return;
    if (dx >= 150) {
      const ok = document.createElement('div');
      ok.id = 'ok';
      ok.textContent = 'SOLVED-4k';
      document.body.appendChild(ok);
      log.textContent = 'SOLVED-4k';
    }
    down = null;
  });
};
setTimeout(mount, ${delay});
${swap ? `setTimeout(() => {
  const bg = document.getElementById('bg');
  if (bg) bg.style.width = '280px';
  const pc = document.getElementById('piece');
  if (pc) pc.style.left = '6px';
}, 2500);` : ''}
</script></body>`;

let sessionId = '', targetId = '';
const servers = [];

// node:http is imported lazily so the fixture helper above stays readable.
// /frame is served by its own process port (a real cross-origin iframe);
// /embed references it with that port at request time.
async function startServers() {
  const { createServer } = await import('node:http');
  const mk = (bodies) =>
    new Promise((resolve) => {
      const s = createServer((req, res) => {
        const path = new URL(req.url, 'http://x').pathname;
        const body = bodies[path];
        if (!body) { res.writeHead(404); res.end(); return; }
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(body());
      });
      s.listen(0, '127.0.0.1', () => { servers.push(s); resolve(s); });
    });
  const frame = await mk({ '/frame': () => page({ swap: true, delay: 300 }) });
  const framePort = frame.address().port;
  const main = await mk({
    '/main': () => page({ delay: 0 }),
    '/late': () => page({ delay: 1200 }),
    '/embed': () =>
      `<!doctype html><body style="margin:0"><iframe id="fr" width="440" ` +
      `height="340" src="http://127.0.0.1:${framePort}/frame"></iframe></body>`,
  });
  return [main.address().port, framePort];
}
const [portA, portB] = await startServers();

cleanup(async () => {
  if (sessionId) await mcp('close-session', { sessionId }).catch(gone);
  await mcp('set-captcha-rules', { revision: 99999, rules: [] }).catch(gone);
  await mcp('delete-agent', { id: AGENT }).catch(gone);
  for (const s of servers) s.close();
});

// Raw call: keeps isError results (mcp() throws on them).
const call = async (name, args) =>
  (await rpc('tools/call', { name, arguments: args })).body?.result ?? { isError: true };
const text = (res) => String(res?.content?.find((p) => p.type === 'text')?.text ?? '');
const out = (res) => {
  try {
    return JSON.parse(text(res));
  } catch {
    return { raw: text(res).slice(0, 80) };
  }
};

// Polls the shared cursor until `pred` matches; {event} or {miss}.
let cursor = 0;
let bootId;
async function waitEvent(pred, budgetMs = 22000) {
  const until = Date.now() + budgetMs;
  while (Date.now() < until) {
    const r = out(await call('wait-captcha-events', { cursor, waitMs: 6000 }));
    if (!Array.isArray(r.events)) {
      return { miss: `wait-captcha-events: ${JSON.stringify(r).slice(0, 140)}` };
    }
    check('wait-captcha-events bootId is non-empty and stable',
      typeof r.bootId === 'string' && r.bootId.length > 0 &&
      (bootId === undefined || bootId === r.bootId), String(r.bootId));
    bootId ??= r.bootId;
    cursor = r.cursor;
    const hit = r.events.find(pred);
    if (hit) return { event: hit };
  }
  return { miss: 'event budget expired' };
}

// 1. Rules snapshot round-trip.
const rule = (id, match, extra = {}) => ({
  id, name: `smoke ${id}`, enabled: true, match,
  detect: {
    root: '#pz',
    requiredSelectors: ['#bg', '#piece', '#handle', '#track'],
    excludedSelectors: ['#ok'],
  },
  type: 'puzzle-slide',
  binding: { background: '#bg', piece: '#piece', handle: '#handle', track: '#track' },
  success: { text: 'SOLVED-4k' },
  ...extra,
});
const set = out(await call('set-captcha-rules', {
  revision: 1,
  rules: [
    rule('smoke-slide', { page: { hosts: ['127.0.0.1'], pathPrefixes: ['/main'] } }),
    rule('smoke-frame', { frame: { scope: 'iframe', hosts: ['127.0.0.1'],
                                   pathPrefixes: ['/frame'] } }),
    rule('smoke-late', { page: { hosts: ['127.0.0.1'], pathPrefixes: ['/late'] } }),
  ],
}));
if (!check('set-captcha-rules applied', set.ok === true && set.applied === 3 &&
  set.revision === 1, JSON.stringify(set).slice(0, 160))) finish();
const status = out(await call('get-captcha-rules', {}));
check('get-captcha-rules status', status.revision === 1 && status.enabledCount === 3 &&
  status.applied === 3, JSON.stringify(status).slice(0, 120));
const bad = out(await call('set-captcha-rules', {
  revision: 2,
  rules: [{ ...rule('bad', { page: { hosts: ['127.0.0.1'] } }),
            detect: { ...rule('bad', {}).detect, javascript: 'alert(1)' } }],
}));
check('a "javascript" field is not a rule key: refused atomically',
  bad.ok === false && bad.error === 'invalid-rules' &&
  out(await call('get-captcha-rules', {})).revision === 1, JSON.stringify(bad).slice(0, 160));
const low = out(await call('set-captcha-rules', { revision: 0, rules: [] }));
check('revision below 1 refused', low.ok === false, JSON.stringify(low).slice(0, 120));

// 2. The agent tab.
await mcp('create-agent', { id: AGENT, name: 'Smoke captcha' }).catch(async (e) => {
  if (!/exists/.test(e.message)) throw e;
});
({ sessionId } = await mcp('open-session', { agent: AGENT }));
({ targetId } = await mcp('allocate-tab', { sessionId }));
await mcp('set-tab-viewport', { targetId, width: 800, height: 600 });

// 3. Main-frame widget: ready -> probe -> challenge -> verified solve + lock.
await mcp('navigate-tab', { targetId, url: `http://127.0.0.1:${portA}/main` });
const ready = await waitEvent((e) => e.type === 'challenge-ready' && e.ruleId === 'smoke-slide');
if (check('challenge-ready for the main-frame widget', !!ready.event,
  JSON.stringify(ready).slice(0, 160))) {
  check('event carries the tool-usable targetId', ready.event.targetId === targetId,
    `${ready.event.targetId} vs ${targetId}`);
  const chId = ready.event.challengeId;
  check('challengeId shape ch_<hex>', /^ch_[0-9a-f]{16}$/.test(chId), chId);

  const probe = out(await call('probe-captcha', { targetId }));
  const mainRow = (probe.frames ?? []).find((f) => f.url?.includes('/main'));
  check('probe-captcha reports the ready frame',
    mainRow?.state === 'ready' && mainRow?.ruleId === 'smoke-slide',
    JSON.stringify(probe).slice(0, 220));

  const ch = out(await call('get-captcha-challenge', { challengeId: chId }));
  check('get-captcha-challenge: images, geometry, frameUrl',
    !!ch.images?.background?.b64 && ch.geometry?.handle?.width === 40 &&
      ch.geometry?.track?.width === 300 && ch.geometry?.background?.width === 300 &&
      (ch.frameUrl ?? '').includes('/main') && ch.stateRevision >= 1 &&
      ch.type === 'puzzle-slide',
    JSON.stringify({ b64: ch.images?.background?.b64?.length, g: ch.geometry,
                     rev: ch.stateRevision }).slice(0, 240));
  const rev = ch.stateRevision;

  // Solve (p = 0.5). While it runs: a second attempt is refused
  // captcha-in-progress, and session CDP Input on the tab is refused.
  const solveP = call('solve-captcha-challenge', {
    challengeId: chId, stateRevision: rev,
    solution: { slideXProportion: 0.5 }, verifyTimeoutMs: 8000, deadlineMs: 12000,
  });
  await sleep(40);
  let cdpRefused = false;
  try {
    const client = await cdp(`ws://127.0.0.1:${port}/devtools/page/${targetId}`);
    const r = await client.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', x: 10, y: 10, button: 'left', clickCount: 1,
    });
    cdpRefused = String(r?.error?.message ?? '').includes('captcha-in-progress');
    client.close();
  } catch (e) {
    cdpRefused = String(e?.message ?? '').includes('captcha-in-progress');
  }
  check('session CDP Input refused while the tab is locked', cdpRefused, String(cdpRefused));
  const second = out(await call('solve-captcha-challenge', {
    challengeId: chId, stateRevision: rev, solution: { slideXProportion: 0.2 },
  }));
  check('second attempt refused while locked (captcha-in-progress)',
    second.outcome === 'error' && second.reason === 'captcha-in-progress' &&
    second.acted === false, JSON.stringify(second).slice(0, 160));
  await sleep(250);
  const readOkStill = out(await call('wait-captcha-events', { cursor, waitMs: 0 }));
  check('reads are not locked (wait answers during a solve)',
    Array.isArray(readOkStill.events), JSON.stringify(readOkStill).slice(0, 80));

  const res = out(await solveP);
  check('solve verified (ratio maths, trusted drag, no CDP)',
    res.outcome === 'verified' && res.verified === true && res.acted === true &&
     res.timings?.captureMs >= 0 && res.timings?.totalMs >= 0 && res.reason === undefined,
    JSON.stringify(res).slice(0, 220));
  const done = await waitEvent((e) => e.type === 'solve-finished' &&
    e.challengeId === chId && e.outcome === 'verified');
  check('solve-finished event (verified)', !!done.event, JSON.stringify(done).slice(0, 160));
  const after = await call('get-captcha-challenge', { challengeId: chId });
  check('a solved challenge reads as gone', after.isError === true &&
    text(after).startsWith('challenge-gone: '), text(after).slice(0, 120));

  // CDP input works again (lock released).
  let freeAgain = false;
  try {
    const client = await cdp(`ws://127.0.0.1:${port}/devtools/page/${targetId}`);
    const r = await client.send('Input.dispatchMouseEvent',
      { type: 'mouseMoved', x: 5, y: 5 });
    freeAgain = !String(r?.error?.message ?? '').includes('captcha-in-progress');
    client.close();
  } catch {}
  check('tab unlocked after the solve', freeAgain, String(freeAgain));

  // 4. New document: re-navigation, a fresh challenge (new id).
  await mcp('navigate-tab', { targetId, url: `http://127.0.0.1:${portA}/main?again` });
  const again = await waitEvent((e) => e.type === 'challenge-ready' &&
    e.ruleId === 'smoke-slide' && e.challengeId !== chId);
  check('widget re-detected after navigation with a new id', !!again.event,
    JSON.stringify(again).slice(0, 160));
  const goneNav = await waitEvent((e) => e.type === 'challenge-gone' &&
    e.challengeId === again.event?.challengeId, 1500);
  check('the navigated-away instance spoke gone only for the old id',
    !goneNav.event, 'unexpected gone for the fresh id');
}

// 5. iframe widget (other port) + timed picture change: same id, rev + 1.
await mcp('navigate-tab', { targetId, url: `http://127.0.0.1:${portA}/embed` });
const fr = await waitEvent((e) => e.type === 'challenge-ready' && e.ruleId === 'smoke-frame');
if (check('challenge-ready for the iframe widget', !!fr.event, JSON.stringify(fr).slice(0, 160))) {
  const ch = out(await call('get-captcha-challenge', { challengeId: fr.event.challengeId }));
  check('iframe challenge frameUrl names the iframe URL',
    (ch.frameUrl ?? '').includes(`127.0.0.1:${portB}/frame`), ch.frameUrl);
  const bump = await waitEvent((e) => e.type === 'challenge-ready' &&
    e.challengeId === fr.event.challengeId, 22000);
  const ch2 = bump.event
    ? out(await call('get-captcha-challenge', { challengeId: fr.event.challengeId }))
    : {};
  check('picture change: one re-fire, SAME challengeId, revision + 1',
    !!bump.event && ch2.stateRevision === (ch.stateRevision ?? 0) + 1 &&
    ch2.geometry?.background?.width === 280,
    JSON.stringify({ had: !!bump.event, a: ch.stateRevision, b: ch2.stateRevision })
      .slice(0, 160));

  // Stale: solve with the OLD revision: no input sent.
  const stale = out(await call('solve-captcha-challenge', {
    challengeId: fr.event.challengeId, stateRevision: ch.stateRevision,
    solution: { slideXProportion: 0.5 },
  }));
  check('older stateRevision: stale with no input',
    stale.outcome === 'stale' && stale.acted === false && stale.verified === false,
    JSON.stringify(stale).slice(0, 160));
  const badSol = out(await call('solve-captcha-challenge', {
    challengeId: fr.event.challengeId, stateRevision: ch2.stateRevision,
    solution: { slideXProportion: 1.5 },
  }));
  check('solution out of range: bad-solution', badSol.outcome === 'error' &&
    badSol.reason === 'bad-solution' && badSol.acted === false,
    JSON.stringify(badSol).slice(0, 160));
}

// 6. Late-mounted widget (no navigation after the mount).
await mcp('navigate-tab', { targetId, url: `http://127.0.0.1:${portA}/late` });
const late = await waitEvent((e) => e.type === 'challenge-ready' && e.ruleId === 'smoke-late');
check('widget mounted 1.2 s after load is detected (timed re-checks)',
  !!late.event, JSON.stringify(late).slice(0, 160));

// 7. Cancel: an attempt that cannot verify is cancelled and releases the tab.
if (late.event) {
  const ch = out(await call('get-captcha-challenge', { challengeId: late.event.challengeId }));
  // p = 0.1 travels 32.5 px: the fixture never shows SOLVED, so the attempt
  // waits for the verdict until cancelled.
  const solveP = call('solve-captcha-challenge', {
    challengeId: late.event.challengeId, stateRevision: ch.stateRevision,
    solution: { slideXProportion: 0.1 }, verifyTimeoutMs: 15000, deadlineMs: 20000,
  });
  await sleep(900);
  const cancel = out(await call('cancel-captcha-challenge',
    { challengeId: late.event.challengeId }));
  const res = out(await solveP);
  check('cancel answers {ok:true,state} and the solve ends cancelled',
    cancel.ok === true && res.outcome === 'error' && res.reason === 'cancelled',
    JSON.stringify({ cancel, res }).slice(0, 200));
  let free = false;
  try {
    const client = await cdp(`ws://127.0.0.1:${port}/devtools/page/${targetId}`);
    const r = await client.send('Input.dispatchMouseEvent',
      { type: 'mouseMoved', x: 5, y: 5 });
    free = !String(r?.error?.message ?? '').includes('captcha-in-progress');
    client.close();
  } catch {}
  check('tab unlocked after cancel (CDP input passes again)', free, String(free));
}

// 8. Closing the tab speaks challenge-gone (reason tab-closed).
const live = out(await call('get-captcha-rules', {}));
if (live.enabledCount === 3) {
  await mcp('release-tab', { targetId });
  const goneTab = await waitEvent((e) => e.type === 'challenge-gone' &&
    e.targetId === targetId, 12000);
  check('tab close spoke challenge-gone', !!goneTab.event &&
    goneTab.event.reason === 'tab-closed', JSON.stringify(goneTab).slice(0, 160));
  targetId = '';
}

// 9. An empty snapshot stops detection; rules stay readable.
const emptied = out(await call('set-captcha-rules', { revision: 3, rules: [] }));
check('empty snapshot applied', emptied.ok === true && emptied.applied === 0,
  JSON.stringify(emptied).slice(0, 120));
check('get-captcha-rules after clear', out(await call('get-captcha-rules', {}))
  .enabledCount === 0);

finish();

// Many assets created at once, as a script would (user feedback 2026-10-19, item 10: the UI skipped frames during a
// batch of 1,000 tokens): run_sidechain.py --script ba_bulk.js [--js-var COUNT=1000]. The page measures while the
// node takes the batch: the frames (gaps of the page's animation frames over 50 ms, the longest), the UI thread of
// the host (window.state answered), and every call of the page to the host by method. Prints "PERF <name> <value>"
// lines (run on the old and the new build to compare) and checks them against limits.
(async () => {
  const { rpc, host, sleep, log, T, waitFor, check, snapshot, finish } = E2E;
  const COUNT = Number(window.__E2E_COUNT || 300);
  const W = 'bulk';
  const perf = (name, v) => log(`PERF ${name} ${typeof v === 'number' ? Math.round(v) : v}`);

  // Every call of the page to the host, by method; the script's own are counted apart.
  const calls = new Map();
  // When each answer came (what the page did after it may be what took the frame).
  const done = [];
  const events = [];
  const onEvents = window.__chainsEvents;
  if (onEvents) {
    window.__chainsEvents = (batch) => {
      const t0 = performance.now();
      onEvents(batch);
      try {
        for (const e of JSON.parse(batch)) events.push([t0, `event:${e.event || e.name || e.type || '?'}`, performance.now() - t0]);
      } catch {
        /* not ours */
      }
    };
  }
  let ownCalls = 0;
  let mine = false;
  const send = window.__chains;
  window.__chains = (req) => {
    try {
      const r = typeof req === 'string' ? JSON.parse(req) : req;
      if (!mine) {
        const key = `${r.kind || ''}:${r.method || ''}${r.params && r.params.method ? `:${r.params.method}` : ''}`;
        calls.set(key, (calls.get(key) || 0) + 1);
        const p = send(req);
        Promise.resolve(p).then(() => done.push([performance.now(), key]), () => undefined);
        return p;
      }
    } catch {
      /* not JSON: counted nowhere */
    }
    return send(req);
  };
  const own = async (fn) => {
    mine = true;
    ownCalls++;
    try {
      const p = fn();
      mine = false;
      return await p;
    } finally {
      mine = false;
    }
  };
  const ownRpc = (m, p, w) => own(() => rpc(m, p, w));

  // With PROFILE: the time spent in a few built-ins the pages lean on, to tell where the work goes.
  const spent = new Map();
  if (window.__E2E_PROFILE) {
    const wrap = (obj, name, label) => {
      const f = obj[name];
      obj[name] = function (...args) {
        const t0 = performance.now();
        try {
          return f.apply(this, args);
        } finally {
          const k = label;
          const d = performance.now() - t0;
          const e = spent.get(k) || [0, 0];
          spent.set(k, [e[0] + d, e[1] + 1]);
        }
      };
    };
    wrap(Array.prototype, 'sort', 'Array.sort');
    wrap(Array.prototype, 'filter', 'Array.filter');
    wrap(Array.prototype, 'map', 'Array.map');
    wrap(JSON, 'stringify', 'JSON.stringify');
    wrap(JSON, 'parse', 'JSON.parse');
    wrap(String.prototype, 'localeCompare', 'String.localeCompare');
    wrap(String.prototype, 'toLowerCase', 'String.toLowerCase');
    wrap(BigInt.prototype, 'toString', 'BigInt.toString');
  }
  // The frames of the page.
  let measuring = false;
  const gaps = [];
  let last = 0;
  let frameStart = 0;
  const frame = (now) => {
    if (measuring) {
      if (last) gaps.push(now - last);
      last = now;
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  // The UI thread of the host.
  let maxHost = 0;
  let probing = false;
  const probe = async () => {
    while (probing) {
      const t0 = performance.now();
      await own(() => host('window.state'));
      maxHost = Math.max(maxHost, performance.now() - t0);
      await sleep(200);
    }
  };

  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '50' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 50, 240000);
    // Coins split so that a thousand transactions do not wait on one chain of change.
    const outs = {};
    for (let i = 0; i < 50; i++) outs[await rpc('getnewaddress', [], W)] = 0.9;
    await rpc('sendmany', ['', outs], W);
    await waitFor('the coins split', async () => (await rpc('listunspent', [1], W)).length >= 50, 120000);

    T().savePrefs({ layout: 'dashboard', theme: 'midnight' });
    T().go(window.__E2E_PAGE || 'home');
    await sleep(3000);
    calls.clear();
    measuring = true;
    frameStart = performance.now();
    probing = true;
    const probing0 = probe();
    const t0 = performance.now();

    // The batch: reservations, then registrations once deep enough.
    const names = Array.from({ length: COUNT }, (_, i) => `BULK${String(i).padStart(5, '0')}`);
    for (const name of names) await ownRpc('reserveasset', [name], W);
    perf('bulk.reserveMs', performance.now() - t0);
    const depth = ((await ownRpc('getbitassetsinfo')) || {}).reveal_depth || 1;
    const h0 = await ownRpc('getblockcount');
    await waitFor('the reservations deep enough', async () => (await ownRpc('getblockcount')) >= h0 + depth + 1, 240000);
    let registered = 0;
    for (const name of names) {
      try {
        await ownRpc('registerasset', [name, 1000, 0], W);
        registered++;
      } catch (e) {
        await log(`register ${name}: ${e && e.message ? e.message : e}`);
        await sleep(1000);
      }
    }
    perf('bulk.registered', registered);
    await waitFor('the assets on the chain', async () => (await ownRpc('getbitassetsinfo')).assets >= registered, 600000).catch(() => undefined);
    await sleep(15000);
    measuring = false;
    probing = false;
    await probing0;
    const total = performance.now() - t0;

    const long = gaps.filter((g) => g > 50);
    perf('bulk.seconds', total / 1000);
    perf('bulk.frames', gaps.length);
    perf('bulk.longFrames', long.length);
    perf('bulk.longFrameMsTotal', long.reduce((a, b) => a + b, 0));
    perf('bulk.worstFrame', Math.max(0, ...gaps));
    perf('bulk.hostMax', maxHost);
    const byMethod = [...calls.entries()].sort((a, b) => b[1] - a[1]);
    perf('bulk.pageCalls', byMethod.reduce((a, [, n]) => a + n, 0));
    for (const [m, n] of byMethod.slice(0, 15)) await log(`CALLS ${n} ${m}`);
    // What came just before each long frame.
    const blame = new Map();
    let t = 0;
    for (const g of gaps) t += g;
    const frames = [];
    let at = frameStart;
    for (const g of gaps) {
      at += g;
      if (g > 50) frames.push([at - g, at]);
    }
    for (const [a, b] of frames) {
      const seen = new Set();
      for (const [ts, key] of [...done, ...events.map((e) => [e[0], e[1]])]) if (ts >= a - 30 && ts <= b) seen.add(key);
      for (const k of seen) blame.set(k, (blame.get(k) || 0) + 1);
    }
    for (const [k, n] of [...blame.entries()].sort((x, y) => y[1] - x[1]).slice(0, 15)) await log(`BLAME ${n} ${k}`);
    const slowEvents = events.filter((e) => e[2] > 30).length;
    perf('bulk.slowEventBatches', slowEvents);
    for (const [k, [ms, n]] of [...spent.entries()].sort((a, b) => b[1][0] - a[1][0])) await log(`SPENT ${Math.round(ms)} ms ${n} calls ${k}`);
    perf('bulk.eventHandlerMax', Math.max(0, ...events.map((e) => e[2])));
    await snapshot('ba-bulk-after');
    const limit = Number(window.__E2E_MAXLONG || 0);
    if (limit) await check(`at most ${limit} long frames while ${COUNT} assets are made`, long.length <= limit, `${long.length}`);
    await check('the UI thread of the host answered within a second throughout', maxHost < 1000, `${Math.round(maxHost)} ms`);
  } catch (e) {
    await check('no exception', false, String(e && e.stack ? e.stack : e));
  }
  await finish();
})();

// Pictures of BitAssets' pages changed by the user feedback of 2026-10-19 (run_sidechain.py --script ba_feedback.js),
// in Midnight: the chart of a price of many decimals (an asset of a huge supply pooled against a little CHN) and of a
// huge price, the asset picker of Send searched by name, Browse assets, My assets with Register new asset as a tab,
// the Simple navigation, the Transactions page with an asset's row and its details.
(async () => {
  const { rpc, host, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const W = 'ba';
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = $$('button', overlay).find((b) => b.textContent.trim() === 'Hide');
      if (hide) hide.click();
    }
    await sleep(900);
    await snapshot(name);
  }
  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);
    T().savePrefs({ layout: 'dashboard', theme: 'midnight', uiMode: 'advanced', tipsDone: true });

    // Assets: a tiny price (a trillion units against a hundredth of a CHN), a huge one, and two of one name.
    const specs = [['TINY', 1e12, 0], ['BIG', 0.0000001, 8], ['GOLD', 1000, 2], ['GOLDEN-LONG-TICKER-NAME', 500, 0]];
    for (const [name] of specs) await rpc('reserveasset', [name], W);
    const depth = (await rpc('getbitassetsinfo')).reveal_depth || 1;
    const h0 = await rpc('getblockcount');
    await waitFor('the reservations deep enough', async () => (await rpc('getblockcount')) >= h0 + depth + 1, 120000);
    const ids = {};
    for (const [name, supply, decimals] of specs) ids[name] = (await rpc('registerasset', [name, supply, decimals], W)).asset;
    await waitFor('the assets registered, in a block', async () => (await rpc('getbitassetsinfo')).assets >= specs.length, 180000);
    await rpc('addliquidity', ['TINY', 1e12, 'CHN', 0.01], W);
    await rpc('addliquidity', ['BIG', 0.0000001, 'CHN', 15], W);
    await waitFor('the pools', async () => ((await rpc('listpools')) || []).length >= 2, 120000);
    await rpc('mintasset', ['GOLD', 10], W);
    await sleep(4000);

    // The chart of a tiny price: compact labels that fit.
    T().go('assets');
    await sleep(800);
    T().go('assets');
    const nav = T();
    nav.nav.focus = { page: 'assets', key: ids.TINY };
    await waitFor('the chart of TINY', () => $('.ba-chart svg'), 30000);
    const labels = $$('.ba-chart text.axis').map((t) => [...t.childNodes].filter((n) => n.nodeType === 3).map((n) => n.textContent).join(''));
    await check('the axis of a tiny price in a few characters', labels.length > 0 && labels.every((l) => l.length <= 10), JSON.stringify(labels));
    $('.ba-chart').scrollIntoView({ block: 'center' });
    await shot('ba-chart-tiny');
    nav.nav.focus = { page: 'assets', key: ids.BIG };
    await sleep(1500);
    await waitFor('the chart of BIG', () => $('.ba-chart svg'), 30000);
    $('.ba-chart').scrollIntoView({ block: 'center' });
    await shot('ba-chart-huge');

    // Browse assets, and My assets with its tab Register new asset.
    T().go('home');
    T().go('assets');
    await sleep(1500);
    await shot('ba-browse-assets');
    T().go('issue');
    await waitFor('Register new asset, a tab of My assets', () => T().nav.page === 'my-assets' && $('.issue'), 20000);
    await shot('ba-my-assets-register-tab');

    // The picker of Send, searched.
    T().go('send');
    await waitFor('the picker', () => $('#send-what'), 20000);
    $('#send-what').click();
    const search = await waitFor('its search', () => $('.asset-picker input[type="search"]'));
    set(search, 'gol');
    await waitFor('GOLD found', () => $(`[role="option"][data-id="${ids.GOLD}"]`), 20000);
    await shot('ba-send-picker-search');
    $(`[role="option"][data-id="${ids.GOLD}"]`).click();
    await sleep(500);
    await shot('ba-send-asset');

    // Transactions: the asset's name whole, the address shortened; the details of a mint.
    T().go('transactions');
    const row = await waitFor('a row "Minted"', () => $$('[role="row"]').find((r) => r.textContent.includes('Minted')), 60000);
    await shot('ba-transactions');
    row.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    await waitFor('its asset movement', () => $('[data-testid="asset-movement"]'), 20000);
    await shot('ba-details-mint');
    const close = $$('dialog[open] button').find((b) => /^Close$/.test(b.textContent.trim()));
    if (close) close.click();

    // Simple mode: its navigation.
    T().savePrefs({ uiMode: 'simple' });
    T().go('home');
    await shot('ba-simple-nav');
    T().savePrefs({ uiMode: 'advanced', sidebarCollapsed: true });
    await shot('ba-advanced-collapsed');
    T().savePrefs({ sidebarCollapsed: false });
    await shot('ba-advanced-expanded');
    await check('done', !!text('nav.sidebar'));
    await rpc('setbmm', [false]);
    await finish();
  } catch (e) {
    await finish(e instanceof Error ? e : new Error(e && e.message ? `${e.message} (${e.code})` : JSON.stringify(e)));
  }
})();

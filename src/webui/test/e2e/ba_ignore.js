// A spam asset received, ignored from its notice, gone from the wallet's lists, and back once unignored (user feedback
// 2026-10-19, item 19): run_sidechain.py --script ba_ignore.js. Another wallet of the node makes an asset and sends it
// to the wallet shown; the notice of it offers "Ignore this asset"; then Home, My assets, the Transactions page (one
// line for the hidden transaction) and Settings → Asset filters, with pictures; unignored, it shows again.
(async () => {
  const { rpc, host, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const W = 'ba';
  const S = 'spammer';
  const NAME = 'FREE-AIRDROP';
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = $$('button', overlay).find((b) => b.textContent.trim() === 'Hide');
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const go = async (page, tab = '') => {
    T().go(page, tab);
    await sleep(700);
  };
  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    await rpc('createwallet', [S]);
    await sleep(1000);
    if (T().wallets.current !== W) T().selectWallet(W);
    await waitFor('the wallet shown', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);
    await rpc('sendtoaddress', [await rpc('getnewaddress', [], S), 5], W);
    await waitFor('the spammer funded', async () => (await rpc('getbalances', [], S)).mine.trusted >= 5, 120000);
    T().savePrefs({ layout: 'dashboard', theme: 'midnight' });

    // The spam: an asset made by the other wallet, sent to this one.
    await rpc('reserveasset', [NAME], S);
    const depth = (await rpc('getbitassetsinfo')).reveal_depth || 1;
    const h0 = await rpc('getblockcount');
    await waitFor('the reservation deep enough', async () => (await rpc('getblockcount')) >= h0 + depth + 1, 120000);
    const reg = await rpc('registerasset', [NAME, 1000000, 0], S);
    await waitFor('the spam registered', async () => ((await rpc('listmyassets', [], S)).assets || []).some((a) => a.asset === reg.asset && a.balance > 0), 120000);
    await go('home');
    const to = await rpc('getnewaddress', [], W);
    await rpc('sendasset', [to, `0x${reg.asset}`, 500], S);

    // Its notice offers to ignore it.
    const ignore = await waitFor('the notice of the asset received, with "Ignore this asset"', () => $$('button').find((b) => b.textContent.trim() === 'Ignore this asset'), 120000);
    await shot('ba-ignore-notice');
    ignore.click();
    await waitFor('the spam received in the wallet', async () => ((await rpc('listmyassets', [], W)).assets || []).some((a) => a.asset === reg.asset), 120000);
    await sleep(2500);
    await check('ignored: not on Home', !text('[data-testid="held-assets"]').includes(NAME), text('[data-testid="held-assets"]'));
    await shot('ba-ignore-home');
    await go('my-assets');
    await waitFor('My assets', () => $('[aria-label="Holdings"]') || /Nothing yet/.test(text('.my-assets')), 20000).catch(() => undefined);
    await check('ignored: not in My assets', !text('.my-assets').includes(NAME) && /hidden by your asset filters/.test(text('.my-assets')), text('.my-assets').slice(0, 300));
    await go('transactions');
    await waitFor('one line for the hidden transaction', () => $('[data-testid="hidden-transactions"]'), 30000);
    await check('ignored: its transaction folded in the Transactions page', !text('[role="grid"]').includes(NAME), text('[data-testid="hidden-transactions"]'));
    await shot('ba-ignore-transactions');
    await go('settings', 'asset-filters');
    await waitFor('the ignored asset in Settings', () => text('[aria-label="Ignored assets"]').includes(NAME), 20000);
    await shot('ba-asset-filters');

    // Unignored: back everywhere.
    $$('[aria-label="Ignored assets"] button').find((b) => b.textContent.trim() === 'Unignore').click();
    await go('home');
    await waitFor('back on Home', () => text('[data-testid="held-assets"]').includes(NAME), 30000);
    await go('transactions');
    await waitFor('its transaction back in the Transactions page', () => !$('[data-testid="hidden-transactions"]'), 30000);
    await check('unignored: back on Home and in the Transactions page', true);
    await rpc('setbmm', [false]);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();

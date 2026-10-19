// Tickers and display names, against a live mainchain (run_sidechain.py --script ba_tickers.js, the app with
// -testactivationheight=ticker@0): the wizard takes a ticker (uppercased as typed) and a display name, its one
// confirmation shows both; the list of assets, the asset's page, the Home and the Overview (one compact row per asset:
// logo, crown, ticker, number, display name, balance, value) show them; the controller changes the display name from My
// assets; in Transactions the "this wallet" chip names the wallet's address and opens it in the explorer, and the details
// of the registration list the wallet's addresses with what each got. Pictures in Midnight, Advanced.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    }, 30000);
    b.click();
    await sleep(200);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay && button('Hide', overlay)) {
      button('Hide', overlay).click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const W = 'ba';
  const dialog = (action, more = {}) => host('test.dialog', { action, ...more });
  const DISPLAY = 'Gold Bar Token, 1 g';

  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    T().savePrefs({ layout: 'dashboard', tipsDone: true, theme: 'midnight', uiMode: 'advanced' });

    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);
    const info = await rpc('getbitassetsinfo');
    await check('tickers are in the rules (ticker@0)', info.ticker_height === 0 && info.tickers_active === true, JSON.stringify(info));

    // --- The wizard: a ticker typed in lowercase, a display name ---
    await go('issue');
    await waitFor('the ticker field', () => /Ticker/.test(text('.issue')) && $('#issue-name'), 30000);
    set($('#issue-name'), 'gold');
    await check('the ticker is uppercased as typed', $('#issue-name').value === 'GOLD', $('#issue-name').value);
    await waitFor('the ticker free', () => text('.issue').includes('is free'), 20000);
    set(await waitFor('the display name field', () => $('#issue-display')), 'Gold‮Bar');
    await waitFor('a bidirectional override refused', () => text('.issue').includes('U+202E'), 5000);
    set($('#issue-display'), DISPLAY);
    await sleep(500);
    await shot('register-form');
    await click('Next');
    set(await waitFor('the supply field', () => $('#issue-supply')), '1000');
    await click('Next');
    await click('Next');
    await waitFor('the review', () => text('.issue').includes('Review'));
    await check('the review shows the ticker and the display name', text('.issue').includes('Ticker') && text('.issue').includes(DISPLAY), text('.issue').slice(0, 300));
    await host('test.confirmAnswer', { always: false });
    await host('test.confirmAnswer', { show: true });
    await click('Reserve and issue');
    await waitFor('the themed dialog', async () => (await dialog('kind')) === 'themed', 30000);
    await sleep(1200);
    const facts = await dialog('eval', { script: 'document.body.innerText' });
    await check('the confirmation shows the ticker and the display name, on their own rows', /Ticker\s*GOLD/.test(facts) && /Display name\s*Gold Bar Token, 1 g/.test(facts), facts.slice(0, 600));
    await dialog('snapshot', { name: 'register-confirmation' });
    await dialog('eval', { script: `document.getElementById('dialog-accept').click(); true` });
    await host('test.confirmAnswer', { always: true });
    const gold = await waitFor('GOLD registered with its display name', async () => {
      try {
        const a = await rpc('getasset', ['GOLD']);
        return a.display_name === DISPLAY ? a : null;
      } catch {
        return null;
      }
    }, 240000);
    await waitFor('GOLD in the wallet', async () => (await rpc('listmyassets', [], W)).assets.some((h) => h.label === 'GOLD'), 60000);

    // --- Where assets show ---
    await go('assets');
    await waitFor('the display name in the list of assets', () => text('.ba-page').includes(DISPLAY), 60000);
    await sleep(800);
    await shot('asset-list');
    const tile = $$('.ba-page .tile').find((t) => t.textContent.includes('GOLD'));
    tile.click();
    await waitFor('the asset page', () => $('[data-testid="asset-display-name"]'), 30000);
    await check('the page shows the ticker, the display name under it', $('.asset-view h2.name').textContent.trim() === 'GOLD' && $('[data-testid="asset-display-name"]').textContent.includes(DISPLAY), '');
    await sleep(800);
    await shot('asset-page');

    await go('home');
    await waitFor('Your assets on the Home, a compact row', () => $('[data-testid="held-row"]') && text('[data-testid="held-assets"]').includes(DISPLAY), 60000);
    const row = $('[data-testid="held-row"]');
    const height = row.getBoundingClientRect().height;
    await check('one row per asset, about 36 px, the crown inline before the ticker', height <= 42 && !!$('.c-asset [title="You control this asset"]', row) && /GOLD\s*#/.test(row.textContent), `${height} px: ${row.textContent}`);
    await sleep(500);
    await shot('home-your-assets');
    await go('overview');
    await waitFor('Your assets on the Overview', () => $('[data-testid="held-row"]'), 60000);
    await sleep(500);
    await shot('overview-your-assets');

    // --- The controller changes the display name ---
    await go('my-assets', 'holdings');
    const panel = () => $('section[aria-label="Manage GOLD"]');
    await click('Change its display name', panel);
    set(await waitFor('the display name field', () => $('#manage-display')), 'Gold Bar');
    await sleep(400);
    await shot('edit-display-name');
    await click('Save the display name', panel);
    await waitFor('the display name changed on the chain', async () => (await rpc('getasset', ['GOLD'])).display_name === 'Gold Bar', 120000);
    await waitFor('the page shows it', () => panel() && panel().textContent.includes('Gold Bar') && !panel().textContent.includes('1 g'), 60000);
    await check('the display name changed by its controller', true);

    // --- Transactions: this wallet's address, opened in the explorer; the registration's details ---
    await go('transactions');
    set(await waitFor('the type filter', () => $('select[aria-label="Type"]')), 'chain');
    const chip = await waitFor('a "this wallet" chip with an address', () => $$('[data-testid="this-wallet"]').find((c) => /this wallet · \S+/.test(c.textContent)), 60000);
    const regs = (await rpc('listassettransactions', [], W)).find((e) => e.category === 'register');
    const own = regs.to.filter((p) => p.mine).map((p) => p.address);
    await check('the chip names an address of the wallet, all of them in its tooltip', own.every((a) => chip.title.includes(a)), `${chip.title} / ${own}`);
    await sleep(500);
    await shot('transactions-this-wallet');
    chip.focus();
    chip.click();
    await waitFor('the explorer, at that address', () => T().nav.page === 'explorer' && own.some((a) => document.body.textContent.includes(a)), 30000);
    await sleep(1200);
    await shot('explorer-wallet-address');
    await go('transactions');
    set(await waitFor('the type filter', () => $('select[aria-label="Type"]')), 'chain');
    const reg = await waitFor('the row "Registered"', () => $$('[role="row"]').find((r) => r.textContent.includes('Registered')), 60000);
    reg.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    const ownList = await waitFor('the wallet\'s addresses in the details', () => $('[data-testid="own-addresses"]'), 20000);
    const movement = $('[data-testid="asset-movement"]').textContent;
    await check('the details: the headline, each address with what it got, the fee once', /Registered GOLD · Gold Bar · 1,000\.00 created/.test(movement) && /asset/.test(ownList.textContent) && /control coin/.test(ownList.textContent) && !/Network fee/.test(movement), movement.slice(0, 400));
    const all = $('dialog[open]') ? $('dialog[open]').textContent : document.body.textContent;
    await check('no Debit of nothing, the output index under Raw', !/Debit\s*0\.00000000/.test(all) && !!$('[data-testid="tx-raw"]'), '');
    await sleep(600);
    await shot('registration-details');
    void gold;
    await finish();
  } catch (e) {
    await finish(e);
  }
})();

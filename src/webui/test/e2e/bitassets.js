// BitAssets against a live mainchain (run_sidechain.py --script bitassets.js): the assets driven through the
// pages as a user would. The wallet gets coins (a deposit by the helper of the mainchain wallet, merged
// mining on, a block for every block of the mainchain). Then: the Issue wizard reserves GOLD, which the app
// registers by itself once the reservation is deep enough; My assets mints more; Trade makes the GOLD/CHN
// pool and swaps CHN for GOLD; Auctions sells GOLD by Dutch auction, bids on it and buys all that is left,
// and My assets collects it; Send sends GOLD to an address. Pictures of every page of BitAssets on the way,
// in Midnight and Light, the dashboard and the workspace.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  // An asset in a searchable picker (ba/AssetPicker.svelte): open it, type its name, click it.
  const pickAsset = async (id, asset, name, keepOpenShot = false) => {
    $(`#${id}`).click();
    const search = await waitFor(`the search of ${id}`, () => $('.asset-picker input[type="search"]'));
    set(search, name);
    const option = await waitFor(`${name} in ${id}`, () => $(`[role="option"][data-id="${asset}"]`), 20000);
    if (keepOpenShot) await shot('ba-asset-picker');
    option.click();
    await waitFor(`${name} picked in ${id}`, () => $(`#${id}`).dataset.value === asset);
  };

  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(150);
  }
  /** The submit button of the form of a field, once it can be pressed. */
  async function submit(fieldSel, what) {
    const b = await waitFor(what, () => {
      const f = $(fieldSel);
      const el = f && f.closest('form') && $('button[type="submit"]', f.closest('form'));
      return el && !el.disabled ? el : null;
    }, 30000);
    b.click();
    await sleep(150);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    }, 20000);
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(350);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const W = 'ba';
  const held = async (label) => ((await rpc('listmyassets', [], W)).assets.find((a) => a.label === label) || { balance: 0 }).balance;

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check('BitAssets, the dashboard and Midnight by default', T().app.info.name === 'BitAssets' && T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().app.info.name} ${T().prefs.layout}`);

    // --- A wallet with coins ---
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    // The wallet of the mainchain node pays: on the desktop's word (audit port-1 #4), accepted here.
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);

    await go('home');
    await waitFor('the cards of BitAssets on the home', () => text('.home').includes('My assets') && text('.home').includes('Auctions'), 30000);
    await sleep(1200);
    await shot('ba-home-empty');
    await go('assets');
    await sleep(800);
    await shot('ba-assets-empty');

    // --- Issue: the wizard reserves GOLD; the app registers it by itself once deep enough ---
    await go('issue');
    set(await waitFor('the name field', () => $('#issue-name')), 'CHN');
    await waitFor('the name refused', () => text('.issue').includes('CHN is the coin of the chain.'));
    set($('#issue-name'), 'GOLD');
    await waitFor('the name free', () => text('.issue').includes('is free'), 20000);
    await shot('ba-issue-name');
    await submit('#issue-name', 'Next');
    set(await waitFor('the supply field', () => $('#issue-supply')), '1000');
    await sleep(200);
    await shot('ba-issue-supply');
    await submit('#issue-supply', 'Next');
    set(await waitFor('the description field', () => $('#issue-info')), 'One gram of gold, held in a vault. https://example.invalid/gold');
    await sleep(200);
    await submit('#issue-info', 'Next');
    await waitFor('the review', () => text('.issue').includes('On the way'));
    await shot('ba-issue-review');
    // One question of the host for both steps, in its themed window (never the dialog of the system), shown at once
    // (made ahead by the review, dialog.prepare); the page and the host stay responsive meanwhile; then no second question.
    const dialog = (action, more = {}) => host('test.dialog', { action, ...more });
    let maxGap = 0;
    let lastTick = performance.now();
    const ticker = setInterval(() => {
      const n = performance.now();
      maxGap = Math.max(maxGap, n - lastTick - 50);
      lastTick = n;
    }, 50);
    let maxHost = 0;
    let probing = true;
    const probe = (async () => {
      while (probing) {
        const t0 = performance.now();
        // A call on the UI thread of the host: a frozen window would never answer it (said, not waited for forever).
        const answered = await Promise.race([host('window.state').then(() => true), sleep(20000).then(() => false)]);
        maxHost = Math.max(maxHost, performance.now() - t0);
        if (!answered) await log('the UI thread of the host did not answer window.state in 20 s');
        await sleep(100);
      }
    })();
    await host('test.confirmAnswer', { always: false });
    await host('test.confirmAnswer', { show: true });
    const asked0 = (await host('test.confirmed', {})).length;
    const t0 = performance.now();
    await click('Reserve and issue');
    const kinds = [];
    let kind = '';
    for (let i = 0; i < 300 && kind !== 'themed' && kind !== 'native'; i++) {
      kind = await dialog('kind');
      if (kinds[kinds.length - 1] !== kind) kinds.push(kind);
      if (kind !== 'themed') await sleep(50);
    }
    const shownIn = Math.round(performance.now() - t0);
    await check('the registration is asked in the themed dialog of the host, never the dialog of the system', kind === 'themed' && !kinds.includes('native'), JSON.stringify(kinds));
    await check('the dialog shows at once (made ahead)', shownIn < 3000, `${shownIn} ms`);
    await waitFor('the page to say it waits for the confirmation', () => text('.issue').includes('Waiting for your confirmation'));
    const facts = await dialog('eval', { script: `({ title: document.querySelector('#dialog-title').textContent, text: document.body.innerText, theme: document.documentElement.dataset.theme })` });
    await check('one question shows both steps, the details and the fees', /Step 1 of 2/.test(facts.text) && /Step 2 of 2/.test(facts.text) && /registerasset/.test(facts.text) && /1000/.test(facts.text) && /One gram of gold/.test(facts.text) && /Step 2, at most/.test(facts.text), facts.text);
    await check('in the theme of the app (Midnight)', facts.theme === 'midnight', facts.theme);
    await dialog('snapshot', { name: 'ba-issue-one-confirmation' });
    await sleep(1500);
    await dialog('eval', { script: `document.getElementById('dialog-accept').click(); true` });
    await waitFor('the reservation made', () => text('.issue').includes('GOLD is reserved'), 30000);
    await host('test.confirmAnswer', { always: true });
    await shot('ba-issue-reserved');
    const journal = await host('bitassets.store.load');
    await check('what to register kept by the app in its journal', !!journal && journal.pending.some((p) => p.name === 'GOLD' && p.supply === '1000'), JSON.stringify(journal));
    await go('my-assets', 'issuing');
    await sleep(1500);
    await shot('ba-my-issuing');
    await waitFor('the progress card', () => document.querySelector('.pending-card'));
    await waitFor('the reservation in a block', () => /block \d of \d|Registration sent/.test(text('.pending-card')), 60000);
    await shot('ba-pending-card');
    const gold = await waitFor('GOLD registered by the app by itself', async () => (await rpc('listassets')).find((a) => a.label === 'GOLD'), 180000);
    const again = (await host('test.confirmed', {})).slice(asked0);
    await check('one question in all for both steps: the registration was not asked again', again.length === 1 && !again.some((q) => q.title === 'Register an asset'), JSON.stringify(again.map((q) => q.title)));
    probing = false;
    await Promise.race([probe, sleep(25000)]);
    clearInterval(ticker);
    await check('the page and the host stayed responsive throughout (no frozen window)', maxGap < 1000 && maxHost < 1000, `page ${Math.round(maxGap)} ms, host ${Math.round(maxHost)} ms`);
    // Done: said in the top bar (and a toast), its card registered.
    const pill = await waitFor('the indicator of the top bar saying it is registered', () => {
      const b = $('.pending-pill');
      return b && /Registered/.test(b.getAttribute('aria-label') || '') ? b : null;
    }, 60000);
    pill.click();
    await waitFor('its card', () => $('.pending-ind .pending-card[data-stage="done"]'));
    await shot('ba-registered');
    pill.click();
    await go('my-assets', 'issuing');
    await sleep(800);
    await check('GOLD registered with its supply and description', gold.supply === 1000 && gold.decimals === 2 && gold.data.info.startsWith('One gram'), JSON.stringify(gold));
    await waitFor('GOLD in the wallet', async () => (await held('GOLD')) === 1000, 60000);

    // --- My assets: mint more ---
    await go('my-assets', 'holdings');
    await click('Mint more', () => $('section[aria-label="Manage GOLD"]'));
    set(await waitFor('the mint field', () => $('#mint-amount')), '10');
    await submit('#mint-amount', 'Mint');
    await answerDialog('Mint GOLD', 'Mint');
    await waitFor('GOLD minted', async () => (await held('GOLD')) === 1010, 60000);
    await check('10 GOLD minted from the panel of the asset', true);
    await sleep(1500);
    await shot('ba-my-assets');

    // --- Trade: a pool of GOLD and CHN, then a swap ---
    await go('trade', 'liquidity');
    await waitFor('the liquidity form', () => $('#lq-a'));
    await pickAsset('lq-a', gold.asset, 'GOLD');
    await sleep(300);
    set($('#lq-amount-a'), '500');
    set($('#lq-amount-b'), '5');
    await waitFor('the preview of the pool', () => text('.trade').includes('Share of the pool'), 20000);
    await shot('ba-trade-liquidity');
    await submit('#lq-amount-a', 'Make the pool');
    await answerDialog('Make a pool', 'Make it');
    const pool = await waitFor('the pool made', async () => (await rpc('listpools'))[0], 60000);
    await check('the GOLD/CHN pool made at 0.01 CHN', pool.asset_a === 'GOLD' && Math.abs(pool.price - 0.01) < 1e-9, JSON.stringify(pool));
    await waitFor('the pool shares in the wallet', async () => (await rpc('listmyassets', [], W)).liquidity.length === 1, 60000);

    await go('trade', 'swap');
    await waitFor('the pair chosen', () => $('#swap-get') && $('#swap-get').dataset.value === gold.asset, 20000);
    set($('#swap-amount'), '0.04');
    await waitFor('the quote of the node', () => text('.trade').includes('At least'), 20000);
    await sleep(500);
    await shot('ba-trade-swap');
    const before = await held('GOLD');
    await submit('#swap-amount', 'Swap');
    await answerDialog('Swap', 'Swap');
    await waitFor('GOLD bought', async () => (await held('GOLD')) > before, 60000);
    await check('GOLD bought with CHN in the pool', true);
    await go('trade', 'pools');
    (await waitFor('the pool in the table', () => $('table[aria-label="Pools"] tbody tr'))).click();
    await sleep(1200);
    await shot('ba-trade-pool');
    await waitFor('the pool shown', () => text('.trade').includes('The curve'));

    // --- Auctions: sell 100 GOLD, bid, buy all that is left, collect ---
    await go('auctions', 'sell');
    await waitFor('GOLD to sell', () => $('#sell-asset') && $('#sell-asset').dataset.value === gold.asset, 20000);
    set($('#sell-amount'), '100');
    set($('#sell-start'), '2');
    set($('#sell-end'), '1');
    set($('#sell-duration'), '60');
    await waitFor('the price line', () => $('svg[aria-label="The price of one unit over the auction"]'), 20000);
    await shot('ba-auction-sell');
    await submit('#sell-amount', 'Start the auction');
    await answerDialog('Start an auction', 'Start the auction');
    const auction = await waitFor('the auction open', async () => (await rpc('listauctions')).find((a) => a.status === 'open'), 90000);
    await go('auctions');
    set(await waitFor('the bid field', () => $('#bid-amount'), 30000), '0.2');
    await waitFor('the quote of the bid', () => text('.auctions').includes('in the next block, more if it waits'), 20000);
    await sleep(500);
    await shot('ba-auction-bid');
    await submit('#bid-amount', 'Bid');
    await answerDialog('Bid', 'Bid');
    await waitFor('the bid taken', async () => (await rpc('getauction', [auction.auction])).bids === 1, 60000);
    await click('Buy all that is left');
    await answerDialog('Buy all', 'Buy all');
    await waitFor('the auction sold out', async () => (await rpc('getauction', [auction.auction])).status === 'sold out', 60000);
    await check('a bid, then all that was left bought', true);
    await go('my-assets', 'auctions');
    await click('Collect');
    await answerDialog('Collect the auction', 'Collect');
    await waitFor('the auction collected', async () => (await rpc('listmyassets', [], W)).receipts.length === 0, 60000);
    await check('the auction collected from My assets', true);

    // --- Send: GOLD to an address, from the Send page ---
    const to = await rpc('getnewaddress', ['friend'], W);
    await go('send');
    await waitFor('the picker of what to send', () => $('#send-what'), 20000);
    await pickAsset('send-what', gold.asset, 'GOLD', true);
    await shot('ba-send-picker');
    set(await waitFor('the address field', () => $('#asset-to')), to);
    set($('#asset-amount'), '12.5');
    await sleep(800);
    await shot('ba-send-asset');
    await submit('#asset-amount', 'Send');
    await answerDialog('Send GOLD', 'Send');
    await waitFor('the transfer in a block', async () => (await rpc('listassetactivity', [], W)).some((a) => a.operation === 'transfer' && a.confirmations > 0), 60000);
    await check('GOLD sent from the Send page', true);

    // --- Every page of BitAssets, in the layouts and looks ---
    await go('assets');
    await sleep(1000);
    await shot('ba-assets');
    T().go('assets');
    (await waitFor('GOLD in the list', () => $$('ul[aria-label="Assets"] button').find((b) => b.textContent.includes('GOLD')))).click();
    await waitFor('GOLD whole', () => text('.assets-page').includes('History of its data'), 20000);
    await sleep(1200);
    await shot('ba-asset-gold');
    for (const tab of ['liquidity', 'activity']) {
      await go('my-assets', tab);
      await sleep(900);
      await shot(`ba-my-${tab}`);
    }
    await go('home');
    await sleep(1500);
    await shot('ba-home-midnight');
    T().savePrefs({ theme: 'light' });
    for (const [page, tab] of [['home', ''], ['assets', ''], ['my-assets', ''], ['trade', 'swap'], ['auctions', ''], ['issue', '']]) {
      await go(page, tab);
      await sleep(900);
      await shot(`ba-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    for (const [page, tab] of [['home', ''], ['assets', ''], ['trade', 'pools'], ['auctions', '']]) {
      await go(page, tab);
      await sleep(1100);
      await shot(`ba-workspace-${page}`);
    }
    T().savePrefs({ theme: 'light' });
    await go('my-assets');
    await sleep(900);
    await shot('ba-workspace-my-assets-light');
    T().savePrefs({ layout: 'dashboard', theme: 'midnight' });

    await rpc('setbmm', [false]);
    await log(`ASSET ${gold.asset}`);
    {
      // The confirmations of the desktop: what the chain's pages paid or signed, each with its facts.
      const titles = ["Create an asset", "Add liquidity", "Bid in an auction", "Burn an asset", "Collect an auction", "Create an auction", "Fix the supply of an asset", "Mint an asset", "Register an asset", "Release a reservation", "Release an asset", "Remove liquidity", "Reserve an asset", "Send an asset", "Swap", "Transfer the control of an asset", "Update an asset"];
      const asked = (await host('test.confirmed', {})).filter((q) => titles.includes(q.title));
      await check(`the desktop confirmed what the pages paid or signed (${asked.length})`, asked.length > 0 && asked.every((q) => q.rows.length > 1), JSON.stringify(asked.map((q) => q.title)));
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();

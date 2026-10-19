// BitAssets in Simple mode, as a user chooses it (run_sidechain.py --script bitassets_simple.js
// --js-var SIMPLE=true): the wallet gets coins (a deposit by the helper of the mainchain wallet, merged mining
// on); Create a token makes GOLD through its wizard, which the app registers by itself; a market of GOLD and CHN
// is opened (by RPC: liquidity is Advanced's); Swap buys GOLD for CHN from its plain quote; My assets shows it.
// Pictures of Create a token, Swap and My assets in Midnight and Light.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
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
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await sleep(400);
    await snapshot(name);
  }
  const W = 'ba';
  const held = async (label) => ((await rpc('listmyassets', [], W)).assets.find((a) => a.label === label) || { balance: 0 }).balance;

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Advanced on a first start (user decision), Simple once chosen', window.__E2E_FIRST_MODE === 'advanced' && T().prefs.uiMode === 'simple', `${window.__E2E_FIRST_MODE} ${T().prefs.uiMode}`);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    T().savePrefs({ layout: 'dashboard', tipsDone: true });

    // --- A wallet with coins ---
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);

    // --- Create a token: GOLD through the wizard of Simple mode ---
    await go('issue');
    await waitFor('Create a token', () => text('.issue h1') === 'Create a token');
    await check('the page says Create a token', true);
    set(await waitFor('the name field', () => $('#issue-name')), 'GOLD');
    await waitFor('the name free', () => text('.issue').includes('is free'), 20000);
    await shot('ba-simple-create-name-midnight');
    await click('Next');
    set(await waitFor('the supply field', () => $('#issue-supply')), '1000');
    await check('the decimals under More options', !$('#issue-decimals'));
    await sleep(200);
    await shot('ba-simple-create-amount-midnight');
    await click('Next');
    set(await waitFor('the description field', () => $('#issue-info')), 'One gram of gold, held in a vault.');
    await click('Next');
    await waitFor('the review', () => text('.issue').includes('On the way'));
    await shot('ba-simple-create-review-midnight');
    await click('Create the token');
    await waitFor('the reservation made', () => text('.issue').includes('GOLD is reserved'), 30000);
    const gold = await waitFor('GOLD registered by the app by itself', async () => (await rpc('listassets')).find((a) => a.label === 'GOLD'), 180000);
    await waitFor('GOLD in the wallet', async () => (await held('GOLD')) === 1000, 60000);
    await check('GOLD created with its supply', gold.supply === 1000, JSON.stringify(gold));

    // --- A market of GOLD and CHN (liquidity is Advanced's: by RPC) ---
    await rpc('addliquidity', [`0x${gold.asset}`, '500', 'CHN', '5', 1], W);
    await waitFor('the market open', async () => (await rpc('listpools')).length === 1, 60000);

    // --- Swap: CHN for GOLD from its plain quote ---
    await go('trade', 'swap');
    await check('Swap without the tabs of the pools', !$$('[role="tab"]').some((t) => t.textContent.trim() === 'Pools'));
    await waitFor('the pair chosen', () => $('#swap-get') && $('#swap-get').dataset.value === gold.asset, 30000);
    set($('#swap-amount'), '0.04');
    await waitFor('the quote in plain words', () => text('[data-testid="swap-plain"]').includes('and get about'), 20000);
    await shot('ba-simple-swap-midnight');
    const before = await held('GOLD');
    await click('Swap', () => $('.swap form'));
    await answerDialog('Swap', 'Swap');
    await waitFor('GOLD bought', async () => (await held('GOLD')) > before, 90000);
    await check('GOLD bought with CHN from Swap', true);

    // --- My assets ---
    await go('my-assets');
    await waitFor('GOLD in My assets', () => text('.my-assets').includes('GOLD'), 30000);
    const tabs = $$('[role="tab"]').map((t) => t.textContent.trim());
    await check('My assets: holdings, being created, activity', JSON.stringify(tabs) === JSON.stringify(['Holdings', 'Being created', 'Activity']), JSON.stringify(tabs));
    await shot('ba-simple-my-assets-midnight');
    await go('home');
    await waitFor('the cards of BitAssets', () => text('.home').includes('My assets'), 30000);
    await check('no auctions card on the home', !$$('h2').some((h) => h.textContent.trim() === 'Auctions'));
    await shot('ba-simple-home-midnight');

    // --- The same pages in Light ---
    T().savePrefs({ theme: 'light' });
    await go('trade', 'swap');
    set(await waitFor('the amount', () => $('#swap-amount')), '0.04');
    await waitFor('the quote in plain words', () => text('[data-testid="swap-plain"]').includes('and get about'), 20000);
    await shot('ba-simple-swap-light');
    await go('my-assets');
    await waitFor('GOLD in My assets', () => text('.my-assets').includes('GOLD'), 30000);
    await shot('ba-simple-my-assets-light');
    await go('issue');
    await waitFor('the wizard again', () => text('.issue').includes('Create a token'));
    const another = button('Issue another');
    if (another) another.click();
    set(await waitFor('the name field', () => $('#issue-name')), 'SILVER');
    await waitFor('the name free', () => text('.issue').includes('is free'), 20000);
    await shot('ba-simple-create-name-light');
    await go('home');
    await waitFor('the cards of BitAssets', () => text('.home').includes('My assets'), 30000);
    await shot('ba-simple-home-light');
    await rpc('setbmm', [false]);
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

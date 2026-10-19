// The logos of assets and the asset transfers, against a live mainchain (run_sidechain.py --script bitassets_logo.js):
// the wizard registers LOGO with a picture the picker crops and encodes (picked as a file, as a user does), its one
// confirmation shows the logo in the host's dialog; the asset list, the Home and the Overview show it, with what the wallet
// holds; LOGO sent to a second wallet is a "Sent asset" row with the destination in the Transactions page, and a
// "Received asset" row with the sender in the other wallet's (the address shortened in the middle, whole in its tooltip). Pictures in Midnight, Advanced (window.__E2E_SHOTS names
// them for doc and media).
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
      const el = button(label, root);
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
  const OTHER = 'other';
  const dialog = (action, more = {}) => host('test.dialog', { action, ...more });

  /** A picture as a user would pick it: a 640 x 480 PNG drawn here, given to the file field. */
  async function pickPicture(input) {
    const c = document.createElement('canvas');
    c.width = 640;
    c.height = 480;
    const g = c.getContext('2d');
    const grad = g.createLinearGradient(0, 0, 640, 480);
    grad.addColorStop(0, '#1b2a6b');
    grad.addColorStop(1, '#4fc3f7');
    g.fillStyle = grad;
    g.fillRect(0, 0, 640, 480);
    g.fillStyle = '#ffd54f';
    g.beginPath();
    g.arc(320, 240, 170, 0, Math.PI * 2);
    g.fill();
    g.fillStyle = '#1b2a6b';
    g.font = 'bold 150px sans-serif';
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.fillText('L', 320, 250);
    const blob = await new Promise((resolve) => c.toBlob(resolve, 'image/png'));
    const dt = new DataTransfer();
    dt.items.add(new File([blob], 'my-logo.png', { type: 'image/png' }));
    input.files = dt.files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }

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
    await check('logos are in the rules of regtest', info.logo_height === 0 && info.max_logo_bytes === 32768, JSON.stringify(info));

    // --- The wizard, with a logo ---
    await go('issue');
    set(await waitFor('the name field', () => $('#issue-name')), 'LOGO');
    await waitFor('the name free', () => text('.issue').includes('is free'), 20000);
    await click('Next');
    set(await waitFor('the supply field', () => $('#issue-supply')), '1000');
    await click('Next');
    const file = await waitFor('the logo picker', () => $('#issue-logo-file'));
    await pickPicture(file);
    await waitFor('the logo encoded', () => $('[data-testid="logo-size"]'), 30000);
    const sizeText = $('[data-testid="logo-size"]').textContent;
    await check('the picker made a logo of at most 32 KB', /KB of at most 32\.0 KB/.test(sizeText), sizeText);
    await sleep(500);
    await shot('logo-picker-crop');
    await click('Next');
    await waitFor('the review', () => text('.issue').includes('Review'));
    await check('the review shows the logo', !!$('.issue img.logo-review') && $('.issue img.logo-review').src.startsWith('data:image/'), '');
    await host('test.confirmAnswer', { always: false });
    await host('test.confirmAnswer', { show: true });
    await click('Reserve and issue');
    await waitFor('the themed dialog', async () => (await dialog('kind')) === 'themed', 30000);
    await sleep(1200);
    const facts = await dialog('eval', { script: `({ text: document.body.innerText, img: (document.querySelector('#dialog-rows img') || {}).src || '' })` });
    await check('the confirmation shows the logo as a picture, its size and hash', facts.img.startsWith('data:image/') && /Logo file/.test(facts.text) && /Logo SHA-256/.test(facts.text) && /registerasset/.test(facts.text), facts.text.slice(0, 400));
    await check('the logo is no text in it (never its base64)', !/iVBORw0KGgo|\/9j\//.test(facts.text), '');
    await dialog('snapshot', { name: 'logo-confirmation' });
    await dialog('eval', { script: `document.getElementById('dialog-accept').click(); true` });
    await host('test.confirmAnswer', { always: true });
    await waitFor('LOGO registered with its logo', async () => {
      try {
        const a = await rpc('getasset', ['LOGO']);
        return a.logo && a.logo.size > 0 ? a : null;
      } catch {
        return null;
      }
    }, 240000);

    // --- Where assets show ---
    await go('assets');
    await waitFor('the logo in the list of assets', () => $$('.ba-page img.mark.logo').length > 0, 60000);
    await sleep(800);
    await shot('asset-list-logos');
    await go('home');
    await waitFor('Your assets on the Home', () => $('[data-testid="held-assets"]') && text('[data-testid="held-assets"]').includes('LOGO') && $('[data-testid="held-assets"] img.mark.logo'), 60000);
    await check('the Home lists the asset held, with its balance', text('[data-testid="held-assets"]').includes('1,000') || text('[data-testid="held-assets"]').includes('1000'), text('[data-testid="held-assets"]'));
    await sleep(500);
    await shot('home-your-assets');
    await go('overview');
    await waitFor('Your assets on the Overview', () => $('[data-testid="held-assets"]') && text('[data-testid="held-assets"]').includes('LOGO'), 60000);
    await sleep(500);
    await shot('overview-your-assets');

    // --- A transfer to another wallet ---
    await rpc('createwallet', [OTHER]);
    const to = await rpc('getnewaddress', [], OTHER);
    T().wallets.current = W;
    await sleep(300);
    const sent = await rpc('sendasset', [to, 'LOGO', 25], W);
    await waitFor('the send in a block', async () => (await rpc('listassettransactions', [], W)).some((e) => e.txid === sent.txid && e.confirmations > 0), 120000);
    await go('transactions');
    const typeSelect = await waitFor('the type filter', () => $('select[aria-label="Type"]'));
    set(typeSelect, 'chain');
    // The other side of a move in the table: its address shortened in the middle (user feedback 2026-10-19, as
    // shortAddress of chain/txinfo.svelte.ts), the whole address in its tooltip.
    const rowsOf = () => $$('.transactions [role="row"]').slice(1);
    const shortOf = (address) => (address.length > 16 ? `${address.slice(0, 8)}…${address.slice(-4)}` : address);
    const showsParty = (word, address) => rowsOf().some((r) => {
      const party = r.textContent.includes(word) && $('[data-testid="asset-moves"] .party', r);
      return !!party && party.textContent.includes(shortOf(address)) && party.title.split(' ').includes(address);
    });
    await waitFor('a Sent asset row with its destination', () => showsParty('Sent asset', to), 60000);
    await sleep(600);
    // One row per transaction (the wallet's own halves of it merged), and the amount always whole in view.
    const count = (word) => rowsOf().filter((r) => r.textContent.includes(word)).length;
    await check('one row per transaction: one send, one registration, one reservation', count('Sent asset') === 1 && count('Registered') === 1 && count('Name reserved') === 1, JSON.stringify(rowsOf().map((r) => r.textContent.replace(/\s+/g, ' ').slice(0, 80))));
    const amountsInView = () => {
      const table = $('.transactions [role="grid"], .transactions .table');
      const right = table.getBoundingClientRect().right;
      return rowsOf().every((r) => {
        const cells = $$('[role="gridcell"]', r);
        const last = cells[cells.length - 1];
        return last && last.getBoundingClientRect().right <= right + 1 && last.scrollWidth <= last.clientWidth + 1;
      });
    };
    await check('the amount of every row is whole in view (dashboard)', amountsInView(), `${window.innerWidth} px`);
    await shot('transactions-sent-asset');
    T().savePrefs({ layout: 'workspace' });
    await sleep(800);
    await go('transactions');
    await waitFor('the transactions in the workspace', () => text('.transactions').includes('Sent asset'), 30000);
    await sleep(600);
    await check('the amount of every row is whole in view (workspace)', amountsInView(), '');
    await shot('transactions-workspace');
    T().savePrefs({ layout: 'dashboard' });
    await sleep(800);
    T().wallets.current = OTHER;
    await sleep(500);
    await go('transactions');
    set(await waitFor('the type filter', () => $('select[aria-label="Type"]')), 'chain');
    const senders = (await rpc('listassettransactions', [], OTHER)).find((e) => e.txid === sent.txid).from.map((f) => f.address);
    await check('the receiver knows who paid', senders.length > 0 && senders[0] !== 'unknown', JSON.stringify(senders));
    await waitFor('a Received asset row with the sender', () => showsParty('Received asset', senders[0]), 60000);
    await check('one row for what came in', $$('.transactions [role="row"]').slice(1).filter((r) => r.textContent.includes('Received asset')).length === 1, '');
    await sleep(600);
    await shot('transactions-received-asset');
    T().wallets.current = W;
    await finish();
  } catch (e) {
    await finish(e);
  }
})();

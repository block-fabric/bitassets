// The asset a transaction moved, in the Transactions page and in its details (user feedback 2026-10-19, items 13–15):
// for every kind of move, its label, the other side without the wallet's own outputs listed one by one ("this
// wallet", once), and the "Asset movement" section of the details with the asset, the exact amount, the direction
// and what the operation did.
import { render } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import LocalTxCell from './LocalTxCell.svelte';
import LocalTxDetails from './LocalTxDetails.svelte';
import { ba } from './ba/store.svelte';
import type { AssetTx } from './ba/model';
import { categoryText, chainTxDetails, chainTxOf, counterparty, middleAddress, ownAddresses, shortAddress } from './txinfo.svelte';
import { fireEvent } from '@testing-library/svelte';
import { nav } from '../lib/nav.svelte';
import { setTransport } from '../lib/bridge';
import { wallets } from '../lib/state.svelte';

const GOLD = 'a'.repeat(64);
const ME1 = 'rsc1qmineaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1';
const ME2 = 'rsc1qminebbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2';
const OTHER = 'rsc1qotherccccccccccccccccccccccccccccc9';

let n = 0;
function move(category: string, sign: -1 | 0 | 1, o: Partial<AssetTx> = {}): AssetTx {
  n++;
  return { txid: n.toString(16).padStart(64, '0'), time: n, confirmations: 1, category, asset: GOLD, label: 'GOLDEN-LONG-TICKER', seq: '1739-0029', decimals: 2, amount: '12.50', sign, to: [], from: [], abandoned: false, ...o };
}

const mine = (address: string) => ({ address, mine: true });
const theirs = (address: string) => ({ address, mine: false });

beforeEach(() => {
  setTransport(async () => []);
  wallets.current = 'w';
  ba.transfersWallet = 'w';
});
afterEach(() => {
  setTransport(null);
  ba.transfers = [];
  ba.activity = [];
  wallets.current = null;
});

describe('the other side of a move', () => {
  it('lists each address once, and the wallet\'s own outputs as "this wallet", never two of its addresses', () => {
    // A mint and a registration pay the coins and the control coin to the wallet.
    for (const category of ['mint', 'register']) {
      const c = counterparty(move(category, 1, { to: [mine(ME1), mine(ME2)] }));
      expect(c, category).toEqual({ dir: 'to', parties: [], self: true, own: [mine(ME1), mine(ME2)] });
    }
    // A send: to the other side only (the wallet's asset change is no destination), each address once.
    expect(counterparty(move('send', -1, { to: [theirs(OTHER), mine(ME1), theirs(OTHER)] }))).toEqual({ dir: 'to', parties: [theirs(OTHER)], self: false, own: [] });
    // A swap: its result and its change both to the wallet.
    expect(counterparty(move('swap', 1, { to: [mine(ME1), mine(ME2)] })).parties).toEqual([]);
    // A receive: who paid it.
    expect(counterparty(move('receive', 1, { to: [mine(ME1)], from: [theirs(OTHER), theirs(OTHER)] }))).toEqual({ dir: 'from', parties: [theirs(OTHER)], self: false, own: [] });
    // A send to the wallet itself: to this wallet.
    expect(counterparty(move('send', -1, { to: [mine(ME1)] }))).toEqual({ dir: 'to', parties: [], self: true, own: [mine(ME1)] });
  });

  it('shortens an address in the middle for the table', () => {
    expect(shortAddress(OTHER)).toBe('rsc1qoth…ccc9');
    expect(shortAddress('rsc1qshort')).toBe('rsc1qshort');
  });
});

describe('every kind of move', () => {
  const kinds: [string, -1 | 0 | 1, string][] = [
    ['register', 1, 'Registered'],
    ['mint', 1, 'Minted'],
    ['send', -1, 'Sent asset'],
    ['receive', 1, 'Received asset'],
    ['swap', 1, 'Swap'],
    ['pool_add', -1, 'Pool add'],
    ['pool_remove', 1, 'Pool remove'],
    ['burn', -1, 'Burned'],
    ['logo', 0, 'Logo set'],
    ['auction', -1, 'Auction'],
    ['bid', 1, 'Auction bid'],
    ['collect', 1, 'Auction collect'],
  ];

  it('has its label in the table, the asset whole, the wallet once', () => {
    for (const [category, sign, label] of kinds) {
      const e = move(category, sign, { to: sign < 0 ? [theirs(OTHER), mine(ME1)] : [mine(ME1), mine(ME2)], from: category === 'receive' ? [theirs(OTHER)] : [] });
      ba.transfers = [e];
      expect(categoryText(e)).toBe(label);
      expect(chainTxOf(e.txid)!.type).toBe(label);
      const cell = render(LocalTxCell, { txid: e.txid });
      const text = cell.container.textContent ?? '';
      expect(text, category).not.toContain(ME1);
      expect(text, category).not.toContain(ME2);
      expect((text.match(/this wallet/g) ?? []).length, category).toBeLessThanOrEqual(1);
      if (sign < 0) expect(text, category).toContain(shortAddress(OTHER));
      cell.unmount();
    }
  });

  it('has an Asset movement in its details: the asset, the exact amount, the direction, the other side, what it did', async () => {
    for (const [category, sign, label] of kinds) {
      const e = move(category, sign, { amount: '1234567.89', to: sign < 0 ? [theirs(OTHER)] : [mine(ME1), mine(ME2)] });
      ba.transfers = [e];
      ba.activity = [{ txid: e.txid, time: 1, confirmations: 1, operation: category, summary: `What ${category} did` }];
      const view = render(LocalTxDetails, { wallet: 'w', txid: e.txid });
      const section = view.getByRole('region', { name: 'Asset movement' });
      const text = section.textContent ?? '';
      expect(text, category).toContain(label);
      expect(text, category).toContain('GOLDEN-LONG-TICKER');
      expect(text, category).toContain('1739-0029');
      expect(text, category).toContain('1,234,567.89');
      expect(text, category).toContain(sign > 0 ? 'Into this wallet' : sign < 0 ? 'Out of this wallet' : 'Neither in nor out');
      // The app's own headline where it reads better (a registration, a mint, a burn), else the node's words.
      if (['register', 'mint', 'burn'].includes(category)) expect(text, category).toContain(category === 'register' ? 'Registered GOLDEN-LONG-TICKER · 1,234,567.89 created' : `${label} 1,234,567.89 GOLDEN-LONG-TICKER`);
      else expect(text, category).toContain(`What ${category} did`);
      // The wallet's own addresses, each once, with what it got; no fee here (the CHN side says it once).
      if (sign > 0) expect((text.match(new RegExp(ME1, 'g')) ?? []).length, category).toBe(1);
      expect(text, category).not.toContain('Network fee');
      if (sign < 0) expect(text, category).toContain(OTHER);
      view.unmount();
    }
  });

  it('says the supply and decimals of a registration', () => {
    const e = move('register', 1, { to: [mine(ME1), mine(ME2)] });
    ba.transfers = [e];
    ba.assets = [{ asset: GOLD, label: 'GOLDEN-LONG-TICKER', seq: '1739-0029', decimals: 2, supply: 1000, minted: 1000, burned: 0, fixed: false, registered: 1, registration: e.txid, releasable: false }];
    const view = render(LocalTxDetails, { wallet: 'w', txid: e.txid });
    const text = view.getByRole('region', { name: 'Asset movement' }).textContent ?? '';
    expect(text).toContain('Supply');
    expect(text).toContain('1,000.00');
    expect(text).toContain('This wallet');
    view.unmount();
    ba.assets = [];
  });
});

describe('the wallet\'s own addresses of a move', () => {
  it('says what each got (the asset, the control coin, the change of CHN), and opens it in the explorer', async () => {
    const e = move('register', 1, { display_name: 'Gold Bar Token, 1 g', to: [{ address: ME1, mine: true, roles: ['control coin'] }, { address: ME2, mine: true, roles: ['asset'] }], change: [OTHER] });
    expect(ownAddresses(e)).toEqual([
      { address: ME1, roles: ['control coin'] },
      { address: ME2, roles: ['asset'] },
      { address: OTHER, roles: ['change'] },
    ]);
    ba.transfers = [e];
    // The cell: one chip, "this wallet" and the first address shortened in the middle, all of them in its tooltip.
    const cell = render(LocalTxCell, { txid: e.txid });
    const chip = cell.getByTestId('this-wallet');
    expect(chip.textContent).toContain(`this wallet · ${middleAddress(ME1)}`);
    expect(chip.getAttribute('title')).toBe(`${ME1}\n${ME2}`);
    await fireEvent.click(chip);
    expect(nav.page).toBe('explorer');
    expect(nav.tab).toBe(`search:${ME1}`);
    cell.unmount();
    // The details: the headline with the display name, every address with its role.
    const view = render(LocalTxDetails, { wallet: 'w', txid: e.txid });
    const text = view.getByRole('region', { name: 'Asset movement' }).textContent ?? '';
    expect(text).toContain('Registered GOLDEN-LONG-TICKER · Gold Bar Token, 1 g · 12.50 created');
    const own = view.getByTestId('own-addresses').textContent ?? '';
    for (const part of [ME1, 'control coin', ME2, 'asset', OTHER, 'change']) expect(own).toContain(part);
    view.unmount();
  });

  it('leaves the zero Debit and Credit and the output index of the CHN record out of an asset operation', () => {
    const e = move('register', 1, { to: [mine(ME1)] });
    ba.transfers = [e];
    expect(chainTxDetails(e.txid)).toEqual({ hideZero: true, raw: { 'Output index': 'Output index of the CHN record' }, simpleRows: ['Status', 'Date', 'Transaction fee'] });
    expect(chainTxDetails('f'.repeat(64))).toBeUndefined();
  });
});

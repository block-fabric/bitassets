// The pages of BitAssets: what model.ts works out (amounts with the decimals of an asset, the rules of names
// and data, the pools and the auctions as the chain does them), the journal kept in the file of the host, and
// the pages driven against a pretend node: the assets and an asset opened, an asset of the wallet minted,
// changed and burned, an asset issued by the wizard then registered by itself, a swap, a new pool, a bid and
// buying all of an auction, an auction started, an asset sent from the Send page, the cards of the home, and
// where the pages are in the menus.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { createHash } from 'node:crypto';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import { checkLogo } from './ba/logo';
import LocalOverview from './LocalOverview.svelte';
import LocalSend from './LocalSend.svelte';
import DialogHost from '../components/DialogHost.svelte';
import Assets from './pages/Assets.svelte';
import Auctions from './pages/Auctions.svelte';
import Issue from './pages/Issue.svelte';
import MyAssets from './pages/MyAssets.svelte';
import Trade from './pages/Trade.svelte';
import {
  assetArg,
  auctionBuysAt,
  auctionCostOfRemaining,
  auctionPriceAt,
  auctionProblems,
  CHN_ID,
  dataProblems,
  dataUpdates,
  formatPrice,
  formatUnits,
  hashName,
  isAbandoned,
  lessSlippage,
  nameProblem,
  pairedAmount,
  parseUnits,
  priceImpact,
  provide,
  receiptAction,
  swapIn,
  swapOut,
  unitPrice,
  unitsAtLeast,
  unitsAtMost,
  unitsOf,
  nameMimicsNumber,
  withdraw,
  type Asset,
  type Auction,
  type Holdings,
  type Pool,
} from './ba/model';
import { pendingList } from '../lib/pending.svelte';
import { ACTIVITY_PAGE, ASSET_PAGE, ba, baView, MAX_ASSETS, nameOf, notePrices, priceSeries, readAllAssets, refreshBitAssets, resetBitAssets, sanitizeJournal, sendIntent } from './ba/store.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { handleShortcut } from '../lib/keyboard';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from '../test/bech32';

const HRP = CHAIN.networks.regtest.hrp;
const OTHER = encode(HRP, [0, ...Array.from({ length: 32 }, (_, i) => (i * 7 + 3) % 32)], false);
const ME = encode(HRP, [0, ...program(20)], false);
const id = (c: string) => c.repeat(64);
const GOLD = id('a');
const SILVER = id('b');
const PEBBLES = id('c');

function asset(a: string, extra: Partial<Asset>): Asset {
  return { asset: a, label: 'X', name: 'X', seq: '1739-0029', decimals: 2, supply: 1000, minted: 1000, burned: 0, fixed: false, registered: 5, registration: id('9'), releasable: false, ...extra };
}

let assets: Asset[] = [];
let pools: Pool[] = [];
let auctions: Auction[] = [];
let mine: Holdings = { assets: [], reservations: [], liquidity: [], receipts: [] };
let store: unknown = null;
let locked = false;
let hidden: Asset[] = [];
let activityCount = 1;
const calls: BridgeRequest[] = [];

function fresh() {
  assets = [
    asset(GOLD, { label: 'GOLD', name: 'GOLD', seq: '1739-0029', data: { info: 'One gram of gold', commitment: id('e') }, control: { txid: id('9'), vout: 0, address: ME } }),
    asset(SILVER, { label: 'SILVER', name: 'SILVER', seq: '4218-0372', decimals: 0, supply: 50, minted: 60, burned: 10, fixed: true, registered: 7 }),
    asset(PEBBLES, { label: 'PEBBLES', name: 'PEBBLES', seq: '9031-1144', decimals: 0, supply: 0, minted: 5, burned: 5, fixed: true, registered: 8, releasable: true, release_fee: 0.0001 }),
  ];
  pools = [
    { pool: id('p'), asset_a: 'GOLD', asset_b: 'CHN', asset_a_id: GOLD, asset_b_id: CHN_ID, reserve_a: 500, reserve_b: 50, price: 0.1, shares: 15811388, volume_a: 20, volume_b: 2, swaps: 4, created: 9, abandoned: false },
  ];
  auctions = [
    { auction: id('d'), base: 'GOLD', quote: 'CHN', base_id: GOLD, quote_id: CHN_ID, amount: 100, remaining: 60, proceeds: 4, start_price: 10, end_price: 5, unit_price: 0.09, price: 9, cost_of_remaining: 5.4, start_height: 10, end_height: 59, bids: 1, status: 'open' },
  ];
  mine = {
    assets: [{ asset: GOLD, label: 'GOLD', decimals: 2, balance: 200, pending: 0, outputs: 1, control: true, control_pending: false, supply: 1000, fixed: false },
      { asset: SILVER, label: 'SILVER', decimals: 0, balance: 30, pending: 0, outputs: 1, control: false, control_pending: false, supply: 50, fixed: true },
    ],
    reservations: [{ txid: id('r'), name: 'IRON', taken: false, height: 18, registers_from: 20, wait: 0 }],
    liquidity: [{ pool: id('p'), asset_a: 'GOLD', asset_b: 'CHN', asset_a_id: GOLD, asset_b_id: CHN_ID, shares: 1000000, percent: 6.3245, value_a: 31.62, value_b: 3.162 }],
    receipts: [{ auction: id('d') }],
  };
}

/** Pick an asset in the searchable picker of a field (ba/AssetPicker.svelte), as a user does: open it, click the asset. */
async function pickAsset(id: string, asset: string): Promise<void> {
  await fireEvent.click(document.querySelector(`#${id}`)!);
  const option = await waitFor(() => {
    const o = document.querySelector<HTMLElement>(`[role="option"][data-id="${asset}"]`);
    if (!o) throw new Error(`no option ${asset} in ${id}`);
    return o;
  });
  await fireEvent.click(option);
}

function units(n: number | string, d: number): bigint {
  return unitsOf(n, d);
}

// What pays or signs goes through the host (rpc.confirmed, local.cpp ConfirmedRpcMethods), which asks on the desktop and runs
// the call: here, at once, as the node would.
const CONFIRMED = ['reserveasset', 'registerasset', 'releaseassetreservation', 'sendasset', 'mintasset', 'burnasset', 'updateasset', 'transferassetcontrol', 'fixassetsupply', 'releaseasset', 'swapasset', 'addliquidity', 'removeliquidity', 'createauction', 'bidauction', 'collectauction', 'setassetlogo'];

// The questions the host asked (each a dialog on the desktop), the second steps it was told to run without one
// (confirm.h Authorizations: [wallet, method, params] as JSON), and how long a question takes to be answered.
const asked: string[] = [];
const authorized: string[] = [];
let answerDelay = 0;

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  if (r.method === 'rpc.confirmedMethods') return { confirmed: CONFIRMED, keyed: ['listmyassets', 'listassetactivity'] };
  if (r.method === 'rpc.confirmed') {
    const c = r.params as { method: string; params: unknown[]; wallet?: string; then?: { method: string; params: unknown[] }; authorizedOnly?: boolean };
    expect(CONFIRMED).toContain(c.method);
    const key = JSON.stringify([c.wallet, c.method, c.params]);
    const i = authorized.indexOf(key);
    if (i >= 0) {
      authorized.splice(i, 1);
    } else if (c.authorizedOnly) {
      throw Object.assign(new Error(`${c.method} needs your confirmation`), { code: -32801 });
    } else {
      asked.push(c.then ? `${c.method}+${c.then.method}` : c.method);
      if (answerDelay) await new Promise((resolve) => setTimeout(resolve, answerDelay));
    }
    const result = await fakeNode({ ...r, kind: 'rpc', method: c.method, params: c.params, wallet: c.wallet } as BridgeRequest);
    if (!c.then) return result;
    authorized.push(JSON.stringify([c.wallet, c.then.method, c.then.params]));
    return { ...(result as object), authorized: 3600 };
  }
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  const fail = (message: string, code = -8) => {
    throw Object.assign(new Error(message), { code });
  };
  const byArg = (arg: unknown) => assets.find((a) => assetArg(a.asset) === arg || a.label === arg);
  switch (r.method) {
    case 'getbitassetsinfo':
      return { assets: assets.length + hidden.length, reservations: 1, pools: pools.length, auctions: auctions.length, height: 21, reveal_depth: 2, logo_height: 0, max_logo_bytes: 32768 };
    case 'listassets': {
      // [count, skip, match], as the node takes them; `hidden` are the newest past what the page reads.
      const all = [...assets, ...hidden].filter((a) => p[2] === undefined || a.label.toLowerCase().includes(String(p[2]).toLowerCase()));
      return p[2] === undefined ? assets.slice(Number(p[1] ?? 0), Number(p[1] ?? 0) + Number(p[0] ?? 100)) : all.slice(0, Number(p[0]));
    }
    case 'getasset': {
      const a = byArg(p[0]);
      if (!a) fail('No such asset is registered');
      return a;
    }
    case 'getassethistory':
      return p[0] === assetArg(GOLD) ? { info: [{ value: null, txid: id('9'), height: 5 }, { value: 'One gram of gold', txid: id('8'), height: 12 }], commitment: [{ value: null, txid: id('9'), height: 5 }] } : {};
    case 'listpools': {
      // [asset, count, skip], as the node takes them (paged: audit 7 E-BA3).
      const list = p[0] != null ? pools.filter((x) => assetArg(x.asset_a_id) === p[0] || assetArg(x.asset_b_id) === p[0]) : pools;
      return list.slice(Number(p[2] ?? 0), Number(p[2] ?? 0) + Number(p[1] ?? 100));
    }
    case 'getpool': {
      const x = pools.find((q) => [assetArg(q.asset_a_id), assetArg(q.asset_b_id)].sort().join() === [p[0], p[1]].sort().join());
      if (!x) fail('There is no pool of these assets');
      return x;
    }
    case 'quoteswap': {
      const x = pools[0];
      const payChn = p[0] === 'CHN';
      const rin = payChn ? units(x.reserve_b, 8) : units(x.reserve_a, 2);
      const rout = payChn ? units(x.reserve_a, 2) : units(x.reserve_b, 8);
      const din = payChn ? 8 : 2;
      const dout = payChn ? 2 : 8;
      const ain = parseUnits(String(p[1]), din);
      const aout = swapOut(rin, rout, ain.ok ? ain.units : 0n);
      return { amount_in: Number(formatUnits(ain.ok ? ain.units : 0n, din, { group: false })), amount_out: Number(formatUnits(aout, dout, { group: false })), price: 1, spot_price: 1, price_impact: priceImpact(rin, rout, ain.ok ? ain.units : 0n, aout), fee: 0.003, abandoned: false };
    }
    case 'listauctions': {
      // [include_closed, count, skip, asset].
      const list = (p[0] ? [...auctions, { ...auctions[0], auction: id('f'), status: 'closed', remaining: 0 }] : auctions).filter((a) => p[3] == null || assetArg(a.base_id) === p[3] || assetArg(a.quote_id) === p[3]);
      return list.slice(Number(p[2] ?? 0), Number(p[2] ?? 0) + Number(p[1] ?? 100));
    }
    case 'getmempoolentry':
      return fail('Transaction not in mempool', -5);
    case 'getauction':
      return auctions.find((a) => a.auction === p[0]) ?? (p[0] === id('e') ? { ...auctions[0], auction: id('e') } : fail('No such auction'));
    case 'quotebid':
      return { buys: 10, remaining: 60, cost_of_remaining: 5.4, open: true };
    case 'listmyassets':
      if (locked) fail('Please enter the wallet passphrase with walletpassphrase first.', -13);
      return structuredClone(mine);
    case 'listassetactivity':
      if (locked) fail('Please enter the wallet passphrase with walletpassphrase first.', -13);
      return Array.from({ length: Math.min(Number(p[0] ?? 50), activityCount) }, (_, i) => ({ txid: `5${(i + 1).toString(16).padStart(63, '0')}`, time: 1_700_000_000 - i, confirmations: 3, operation: 'mint', summary: 'Minted 10.00 GOLD' }));
    case 'reserveasset':
      return { txid: id('6'), asset: 'f'.repeat(64) };
    case 'registerasset':
    case 'mintasset':
    case 'burnasset':
    case 'updateasset':
    case 'transferassetcontrol':
    case 'fixassetsupply':
    case 'releaseasset':
    case 'sendasset':
    case 'createauction':
    case 'collectauction':
    case 'releaseassetreservation':
    case 'setassetlogo':
      return { txid: id('7') };
    case 'swapasset':
      return { txid: id('7'), quote: 4.98, min_out: 4.95 };
    case 'addliquidity':
      return { txid: id('7'), amount_a: 10, amount_b: 1, shares: 9000, sets_price: true };
    case 'removeliquidity':
      return { txid: id('7'), shares: 500000, amount_a: 15.81, amount_b: 1.581 };
    case 'bidauction':
      return { txid: id('7'), pays: p[2] ? 5.4 : 1, buys: p[2] ? 60 : 10 };
    case 'bitassets.store.load':
      return structuredClone(store);
    case 'bitassets.store.save':
      store = structuredClone((r.params as { value: unknown }).value);
      return true;
    case 'getnewaddress':
      return OTHER;
    case 'validateaddress':
      return { isvalid: true, address: p[0] };
    case 'prefs.get':
      return {};
    case 'prefs.set':
      return {};
    default:
      return null;
  }
}

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  (globalThis as Record<string, unknown>).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 1_000_000_000n, untrustedPending: 0n, immature: 0n };
  fresh();
  hidden = [];
  activityCount = 1;
  store = null;
  locked = false;
  calls.length = 0;
  asked.length = 0;
  authorized.length = 0;
  answerDelay = 0;
  resetBitAssets();
  setTransport(fakeNode);
});

afterEach(() => {
  resetBitAssets();
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
  nav.focus = null;
});

async function answer(choice: string, timeout = 1000) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  await waitFor(() => expect((document.querySelector(`button[data-choice="${choice}"]`) as HTMLButtonElement).disabled).toBe(false), { timeout });
  await fireEvent.click(document.querySelector(`button[data-choice="${choice}"]`)!);
}
const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(ba.loaded && ba.assets.length > 0 && ba.mineState === 'ok').toBe(true));

describe('amounts, with the decimals of an asset', () => {
  it('parses and writes them exactly, as the chain does', () => {
    expect(parseUnits('12.5', 2)).toEqual({ ok: true, units: 1250n });
    expect(parseUnits('1,000', 0)).toEqual({ ok: true, units: 1000n });
    expect(parseUnits('0.001', 2).ok).toBe(false);
    expect(parseUnits('1.5', 0).ok).toBe(false);
    expect(parseUnits('0', 2).ok).toBe(false);
    expect(parseUnits('0', 2, { allowZero: true })).toEqual({ ok: true, units: 0n });
    expect(parseUnits('92233720368547758.08', 2).ok).toBe(false);
    expect(parseUnits('abc', 2).ok).toBe(false);
    expect(formatUnits(123456789n, 2)).toBe('1,234,567.89');
    expect(formatUnits(1200n, 2)).toBe('12');
    expect(formatUnits(1200n, 2, { trim: false, group: false })).toBe('12.00');
    expect(unitsOf(0.1, 8)).toBe(10_000_000n);
    expect(unitsOf(1e-8, 8)).toBe(1n);
    expect(unitsOf('5.5', 2)).toBe(550n);
    expect(formatPrice(0.000012345678)).toBe('0.0000123457');
    expect(formatPrice(1234.5)).toBe('1,234.5');
    expect(unitPrice(0.00001)).toEqual({ kind: 'buys', amount: '100,000' });
    expect(unitPrice(0.5)).toEqual({ kind: 'each', price: '0.5' });
    expect(lessSlippage(1000n, 0.5)).toBe(995n);
    expect(lessSlippage(1n, 50)).toBe(1n);
  });

  it('knows an asset by the hash of its name, written as the node writes it', async () => {
    const hex = Buffer.from(createHash('sha256').update('GOLD').digest()).reverse().toString('hex');
    expect(await hashName('GOLD')).toBe(hex);
    expect(assetArg(CHN_ID)).toBe('CHN');
    expect(assetArg(GOLD)).toBe(`0x${GOLD}`);
  });
});

describe('names and data, as the chain checks them', () => {
  it('refuses names that read as another asset, or are not plain', () => {
    expect(nameProblem('GOLD')).toBe('');
    expect(nameProblem('Gold bars 2026')).toBe('');
    expect(nameProblem('chn')).not.toBe('');
    expect(nameProblem('0xabc')).not.toBe('');
    expect(nameProblem('1739-0029')).not.toBe('');
    expect(nameProblem('0001-1739-0029')).not.toBe('');
    expect(nameProblem('1739-002')).toBe('');
    expect(nameProblem(' GOLD')).not.toBe('');
    expect(nameProblem('GÖLD')).not.toBe('');
    expect(nameProblem('x'.repeat(65))).not.toBe('');
  });

  it('checks each field of the data, and writes the changes as updateasset takes them', () => {
    expect(dataProblems({ commitment: id('e'), ipv4: '203.0.113.7:8333', ipv6: '[2001:db8::7]:80', encryptionkey: `02${id('1')}`, signingkey: id('2') })).toEqual({});
    const bad = dataProblems({ info: 'x'.repeat(513), commitment: 'ab', ipv4: '300.1.1.1', ipv6: '2001:db8', encryptionkey: `04${id('1')}`, signingkey: 'zz' });
    expect(Object.keys(bad).sort()).toEqual(['commitment', 'encryptionkey', 'info', 'ipv4', 'ipv6', 'signingkey']);
    expect(dataUpdates({ info: 'a', commitment: id('e') }, { info: 'b', commitment: '', ipv4: '1.2.3.4' })).toEqual({ info: 'b', commitment: null, ipv4: '1.2.3.4' });
    expect(dataUpdates({ info: 'a' }, { info: 'a' })).toEqual({});
  });
});

describe('the pools and the auctions, as the chain works them out', () => {
  it('swaps on a constant product, the fee left in the pool', () => {
    const out = swapOut(1_000_000n, 1_000_000n, 1000n);
    expect(out).toBe(996n);
    // What has to go in for an amount out buys at least that amount.
    const need = swapIn(1_000_000n, 1_000_000n, 996n)!;
    expect(swapOut(1_000_000n, 1_000_000n, need)).toBeGreaterThanOrEqual(996n);
    expect(swapIn(10n, 10n, 10n)).toBeNull();
    expect(priceImpact(1_000_000n, 1_000_000n, 1000n, out)).toBeCloseTo(0.4, 3);
  });

  it('gives shares for a deposit: a new pool, a pool with providers (its price taking only what it takes), a pool everyone left', () => {
    const opened = provide({ reserve0: 0n, reserve1: 0n, shares: 0n }, 1_000_000n, 1_000_000n)!;
    expect(opened).toEqual({ shares: 999_000n, take0: 1_000_000n, take1: 1_000_000n, opens: true });
    const pool = { reserve0: 1_000_000n, reserve1: 2_000_000n, shares: 1_414_213n };
    const d = provide(pool, 1000n, 5000n)!;
    expect(d.opens).toBe(false);
    expect(d.take0).toBe(1000n);
    expect(d.take1).toBe(2000n);
    expect(pairedAmount(1000n, 1_000_000n, 2_000_000n)).toBe(2000n);
    // CHN dust that would come back stays in the pool.
    expect(provide({ ...pool, chn1: true }, 1000n, 5000n)!.take1).toBe(5000n);
    expect(isAbandoned(1000n)).toBe(true);
    const reopened = provide({ reserve0: 10n, reserve1: 10n, shares: 1000n }, 1_000_000n, 1_000_000n)!;
    expect(reopened.opens).toBe(true);
    expect(withdraw({ reserve0: 1000n, reserve1: 100_000n, shares: 100n, chn0: true }, 10n)).toEqual([0n, 10_000n]);
  });

  it('prices an auction in a straight line, rounded for the seller, and what a bid buys', () => {
    const a = { baseAmount: 100n, remaining: 60n, startPrice: 1000n, endPrice: 500n, startHeight: 10, endHeight: 59 };
    expect(auctionPriceAt(a, 5)).toBe(1000n);
    expect(auctionPriceAt(a, 10)).toBe(1000n);
    expect(auctionPriceAt(a, 59)).toBe(500n);
    expect(auctionPriceAt(a, 34)).toBe(1000n - (500n * 24n) / 49n);
    expect(auctionBuysAt(a, 59, 50n)).toBe(10n);
    expect(auctionCostOfRemaining(a, 59)).toBe(300n);
    expect(receiptAction({ status: 'open', bids: 0 })).toBe('cancel');
    expect(receiptAction({ status: 'open', bids: 2 })).toBe('wait');
    expect(receiptAction({ status: 'ended', bids: 2 })).toBe('collect');
    const ok = { sameAsset: false, amount: parseUnits('1', 2), start: parseUnits('10', 8), end: parseUnits('5', 8), duration: 60, startIn: 1, sellsChn: false };
    expect(auctionProblems(ok)).toEqual([]);
    expect(auctionProblems({ ...ok, end: parseUnits('20', 8) })).toEqual(['The price falls: the end price is at most the start price.']);
    expect(auctionProblems({ ...ok, sameAsset: true, duration: 0, startIn: 60000 }).length).toBe(3);
    expect(auctionProblems({ ...ok, sellsChn: true, amount: parseUnits('0.0001', 8) }).length).toBe(1);
  });
});

describe('the journal of the app', () => {
  it('keeps the prices it saw, either way round, and reads back only what has its shape', () => {
    expect(notePrices(pools, 21, 1)).toBe(true);
    expect(notePrices(pools, 21, 2)).toBe(false);
    expect(notePrices([{ ...pools[0], price: 0.2 }], 22, 3)).toBe(true);
    expect(priceSeries(pools[0], GOLD).map((p) => p.p)).toEqual([0.1, 0.2]);
    expect(priceSeries(pools[0], CHN_ID).map((p) => p.p)).toEqual([10, 5]);
    const j = sanitizeJournal({ pending: [{ wallet: 'w', name: 'IRON', asset: id('f'), supply: '1', decimals: 0 }, { name: 3 }], prices: { x: { a: GOLD, points: [{ h: 1, t: 1, p: 1 }, 'bad'] } } });
    expect(j.pending.length).toBe(1);
    expect(j.pending[0].public).toBe(true);
    expect(j.prices.x.points.length).toBe(1);
    expect(sanitizeJournal('nonsense')).toEqual({ pending: [], prices: {} });
  });
});

describe.runIf(CHAIN.name === 'BitAssets')('the pages of BitAssets', () => {
  it('are in the Chain section and the menus, before the tools, with their quick actions', () => {
    const own = (LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id);
    expect(own).toEqual(['assets', 'my-assets', 'issue', 'trade', 'auctions']);
    // Register new asset is a tab of My assets (user feedback 2026-10-19): not in the navigation, reached through it.
    const navPages = own.filter((id) => id !== 'issue');
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', ...navPages, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', ...navPages, 'explorer']);
    go('issue');
    expect([nav.page, nav.tab]).toEqual(['my-assets', 'register']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toEqual(expect.arrayContaining(['Browse assets', 'Register new asset', 'Swap']));
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'Add liquidity…')!.run();
    expect([nav.page, nav.tab]).toEqual(['trade', 'liquidity']);
    // Alt+6 opens Assets, as it did in bitassets-qt (audit port-2 #8).
    expect(handleShortcut(new KeyboardEvent('keydown', { key: '6', altKey: true }))).toBe(true);
    expect(nav.page).toBe('assets');
    expect(CHAIN.uriScheme).toBe('bitassets');
  });

  it('Assets: the cards, searched and filtered; an asset opened whole, its markets and the history of its data', async () => {
    render(Assets);
    await loaded();
    const list = await screen.findByRole('list', { name: 'Assets' });
    expect(within(list).getAllByRole('button').length).toBe(3);
    await fireEvent.click(screen.getByRole('button', { name: /^Dead/ }));
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('button').length).toBe(1));
    await fireEvent.click(screen.getByRole('button', { name: /^All/ }));
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'gram' } });
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('button').length).toBe(1));
    await fireEvent.click(within(screen.getByRole('list', { name: 'Assets' })).getByRole('button'));
    await waitFor(() => expect(screen.getByRole('heading', { name: 'GOLD' })).toBeTruthy());
    expect(called('getasset').some((c) => (c.params as unknown[])[0] === assetArg(GOLD))).toBe(true);
    expect(screen.getByText('One gram of gold', { selector: '.info span' })).toBeTruthy();
    expect(screen.getByRole('table', { name: 'History of its data' })).toBeTruthy();
    expect(screen.getByText('You control it')).toBeTruthy();
    // A private name is found by its hash.
    await fireEvent.click(screen.getByRole('button', { name: /All assets/ }));
  });

  it('Assets: a dead asset is retired, after a warning', async () => {
    render(Assets);
    render(DialogHost);
    await loaded();
    nav.focus = { page: 'assets', key: PEBBLES };
    await waitFor(() => expect(screen.getByRole('heading', { name: 'This asset is dead' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Retire it/ }));
    await answer('yes');
    await waitFor(() => expect(called('releaseasset')[0]?.params).toEqual([assetArg(PEBBLES)]));
  });

  it('My assets: the controller adds a logo (picked, cropped, encoded), changes it and removes it', async () => {
    // A picture as the web view decodes it, and a canvas that draws it (jsdom has neither).
    const pixels = new Uint8ClampedArray(256 * 256 * 4).map((_, i) => (i % 4 === 3 ? 255 : i % 4 === 0 ? 240 : 40));
    const proto = HTMLCanvasElement.prototype as unknown as Record<string, unknown>;
    const saved = { getContext: proto.getContext, toBlob: proto.toBlob, bitmap: (globalThis as Record<string, unknown>).createImageBitmap };
    proto.getContext = () => ({ clearRect() {}, drawImage() {}, fillRect() {}, fillStyle: '', imageSmoothingEnabled: true, imageSmoothingQuality: 'high', getImageData: () => ({ data: pixels }) });
    proto.toBlob = (cb: (b: Blob | null) => void) => cb(null);
    (globalThis as Record<string, unknown>).createImageBitmap = async () => ({ width: 640, height: 480, close() {} });
    try {
      render(MyAssets);
      render(DialogHost);
      await loaded();
      const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
      await fireEvent.click(within(panel).getByRole('button', { name: 'Add a logo' }));
      const save = within(panel).getByRole('button', { name: 'Save the logo' }) as HTMLButtonElement;
      expect(save.disabled).toBe(true);
      const input = panel.querySelector('#manage-logo-file') as HTMLInputElement;
      Object.defineProperty(input, 'files', { value: [new File([new Uint8Array([1, 2, 3])], 'logo.png', { type: 'image/png' })], configurable: true });
      await fireEvent.change(input);
      await waitFor(() => expect(panel.querySelector('[data-testid="logo-size"]')?.textContent).toMatch(/PNG, 1 colour:/), { timeout: 3000 });
      await waitFor(() => expect(save.disabled).toBe(false));
      // The logo as it will look, beside the asset as it is.
      expect(panel.querySelectorAll('.logo-before-after img').length).toBe(1);
      await fireEvent.click(save);
      await waitFor(() => expect(called('setassetlogo')).toHaveLength(1));
      const [arg, base64] = called('setassetlogo')[0].params as [string, string];
      expect(arg).toBe(assetArg(GOLD));
      const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
      expect(checkLogo(bytes)).toEqual({ ok: true, format: 'png' });
      // Once listed with it: "Change its logo", and it can be removed.
      assets[0] = { ...assets[0], logo: { hash: 'f'.repeat(64), mime: 'image/png', size: bytes.length, height: 21, txid: id('7') } };
      await refreshBitAssets();
      await waitFor(() => expect(within(panel).queryByRole('button', { name: 'Change its logo' })).not.toBeNull());
      await fireEvent.click(within(panel).getByRole('button', { name: 'Change its logo' }));
      await fireEvent.click(await within(panel).findByRole('button', { name: 'Remove the logo' }));
      await waitFor(() => expect(called('setassetlogo')[1]?.params).toEqual([assetArg(GOLD), '']));
    } finally {
      proto.getContext = saved.getContext;
      proto.toBlob = saved.toBlob;
      (globalThis as Record<string, unknown>).createImageBitmap = saved.bitmap;
    }
  });

  it('My assets: mints, changes the data and burns an asset it controls', async () => {
    render(MyAssets);
    render(DialogHost);
    await loaded();
    const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Mint more' }));
    await fireEvent.input(panel.querySelector('#mint-amount')!, { target: { value: '10.5' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Mint' }));
    await answer('yes');
    await waitFor(() => expect(called('mintasset')[0]?.params).toEqual([assetArg(GOLD), '10.5']));
    // Done: the form closes (the host confirmed and ran it).
    await waitFor(() => expect(panel.querySelector('#mint-amount')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Change its data' }));
    await waitFor(() => expect((panel.querySelector('#data-info') as HTMLTextAreaElement).value).toBe('One gram of gold'));
    await fireEvent.input(panel.querySelector('#data-info')!, { target: { value: 'One gram of gold, in Zurich' } });
    await fireEvent.input(panel.querySelector('#data-commitment')!, { target: { value: '' } });
    await fireEvent.input(panel.querySelector('#data-ipv4')!, { target: { value: '300.1.1.1' } });
    expect(within(panel).getByRole('button', { name: 'Save' })).toHaveProperty('disabled', true);
    await fireEvent.input(panel.querySelector('#data-ipv4')!, { target: { value: '203.0.113.7' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Save' }));
    await answer('yes');
    await waitFor(() => expect(called('updateasset')[0]?.params).toEqual([assetArg(GOLD), { info: 'One gram of gold, in Zurich', commitment: null, ipv4: '203.0.113.7' }]));
    await waitFor(() => expect(panel.querySelector('#data-info')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Burn some' }));
    await fireEvent.input(panel.querySelector('#burn-amount')!, { target: { value: '500' } });
    expect(within(panel).getByText('More than this wallet holds.')).toBeTruthy();
    await fireEvent.input(panel.querySelector('#burn-amount')!, { target: { value: '5' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Burn' }));
    await answer('yes');
    await waitFor(() => expect(called('burnasset')[0]?.params).toEqual([assetArg(GOLD), '5']));
    await waitFor(() => expect(panel.querySelector('#burn-amount')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Control and supply' }));
    await fireEvent.click(within(panel).getByRole('button', { name: 'Fix the supply for good' }));
    await answer('yes');
    await waitFor(() => expect(called('fixassetsupply')[0]?.params).toEqual([assetArg(GOLD), true]));
  });

  it('My assets: pool shares taken out, a receipt, the activity; a locked wallet shows them once unlocked', async () => {
    render(MyAssets);
    render(DialogHost);
    await loaded();
    nav.tab = 'liquidity';
    await fireEvent.click(await screen.findByRole('button', { name: 'Take out…' }));
    await fireEvent.click(screen.getByRole('button', { name: '50%' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Take out' }));
    await answer('yes');
    await waitFor(() => expect(called('removeliquidity')[0]?.params).toEqual([assetArg(GOLD), 'CHN', 50, 1]));
    nav.tab = 'auctions';
    await waitFor(() => expect(screen.getByText(/It can be collected once it ends/)).toBeTruthy());
    nav.tab = 'activity';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Activity' })).toBeTruthy());
    expect(screen.getByText('Minted 10.00 GOLD')).toBeTruthy();
  });

  it('a locked wallet: its assets once unlocked for that alone', async () => {
    locked = true;
    wallets.list = [{ name: 'w', encrypted: true, locked: true, privateKeysDisabled: false, hd: true }];
    render(MyAssets);
    await waitFor(() => expect(ba.mineState).toBe('locked'));
    expect(screen.getByRole('button', { name: /Show my assets/ })).toBeTruthy();
  });

  it('Issue: the wizard reserves a free name, journals what to register, and it is registered by itself once deep enough', async () => {
    render(Issue);
    render(DialogHost);
    await waitFor(() => expect(ba.loaded).toBe(true));
    const name = screen.getByLabelText('Its name');
    await fireEvent.input(name, { target: { value: 'GOLD' } });
    await waitFor(() => expect(screen.getByText(/is taken/)).toBeTruthy());
    await fireEvent.input(name, { target: { value: '1739-0029' } });
    expect(screen.getByText(/does not read as the number of an asset/)).toBeTruthy();
    await fireEvent.input(name, { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000.25' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-info')!, { target: { value: 'Copper, by the gram' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Review' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and issue/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    await waitFor(() => expect(screen.getByRole('heading', { name: /COPPER is reserved/ })).toBeTruthy());
    expect(store).toMatchObject({ pending: [{ wallet: 'w', name: 'COPPER', asset: 'f'.repeat(64), supply: '1000.25', decimals: 2, data: { info: 'Copper, by the gram' }, public: true }] });
    // One question for both steps: the reservation and the registration exactly as it will be sent.
    expect(asked).toEqual(['reserveasset+registerasset']);
    // Deep enough: registered by itself, without a second question.
    mine.reservations.push({ txid: id('6'), name: 'COPPER', taken: false, height: 21, registers_from: 23, wait: 0 });
    await refreshBitAssets();
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(called('registerasset')[0].params).toEqual(['COPPER', '1000.25', 2, { info: 'Copper, by the gram' }, true]);
    expect(asked).toEqual(['reserveasset+registerasset']);
    expect(authorized).toEqual([]);
  });

  it('a registration with no authorization (the app started again): its card asks the user, never a dialog by itself', async () => {
    store = { pending: [{ wallet: 'w', name: 'TIN', asset: id('f'), supply: '5', decimals: 0, data: {}, public: true, time: 1, reservation: id('6') }], prices: {} };
    mine.reservations.push({ txid: id('6'), name: 'TIN', taken: false, height: 15, registers_from: 17, wait: 0 });
    render(MyAssets);
    render(DialogHost);
    nav.tab = 'issuing';
    await waitFor(() => expect(screen.getByRole('button', { name: 'Confirm registration' })).toBeTruthy());
    expect(asked).toEqual([]);
    expect(called('registerasset')).toEqual([]);
    expect(pendingList().find((o) => o.name === 'TIN')?.stage).toBe('confirm');
    await fireEvent.click(screen.getByRole('button', { name: 'Confirm registration' }));
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(asked).toEqual(['registerasset']);
    await waitFor(() => expect(pendingList().find((o) => o.name === 'TIN')?.stage).toBe('registering'));
  });

  it('the pages go on while a question of the host waits (a slow dialog, a slow host): the refresh is never held', async () => {
    answerDelay = 1500;
    render(Issue);
    await waitFor(() => expect(ba.loaded).toBe(true));
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'ZINC' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '7' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(await screen.findByRole('button', { name: /Reserve and issue/ }));
    // The question shows: the page says it waits for it, and the lists are still asked again, at once.
    await waitFor(() => expect(screen.getByRole('button', { name: /Waiting for your confirmation/ })).toBeTruthy());
    const version = ba.version;
    const t0 = Date.now();
    await refreshBitAssets();
    expect(Date.now() - t0).toBeLessThan(500);
    expect(ba.version).toBeGreaterThan(version);
    await waitFor(() => expect(screen.getByRole('heading', { name: /ZINC is reserved/ })).toBeTruthy(), { timeout: 4000 });
    // Its progress on the page, and in the top bar.
    expect(document.querySelector('.pending-card')).toBeTruthy();
    expect(pendingList().find((o) => o.name === 'ZINC')?.stage).toBe('reserved');
  });

  it('Trade: a swap quoted by the node, confirmed with its least, and a new pool with both amounts', async () => {
    render(Trade);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(GOLD));
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(called('quoteswap').length).toBeGreaterThan(0));
    await waitFor(() => expect(screen.getByText('At least')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: '1%' }));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    // The asset named with its number first: a look-alike label (G0LD) is told apart (audit port-2 #5), and a
    // label holding another number reads as text (audit 7 E-BA4).
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('#1739-0029 · "GOLD"'));
    // The least confirmed is the least signed (min_out, audit 7 E-BA2).
    expect(document.querySelector('dialog[open]')?.textContent).toContain('At least 4.88 #1739-0029');
    await answer('yes');
    await waitFor(() => expect(called('swapasset')[0]?.params).toEqual(['CHN', '0.5', assetArg(GOLD), 1, 10, '4.88']));
    // Done (the host confirmed and ran it): the form is cleared.
    await waitFor(() => expect((document.querySelector('#swap-amount') as HTMLInputElement).value).toBe(''));
    // A larger trade moves the price more than 2%: asked about first.
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '2' } });
    await waitFor(() => expect(called('quoteswap').some((c) => (c.params as unknown[])[1] === '2')).toBe(true));
    await waitFor(() => expect(screen.getByRole('button', { name: /^Swap$/ })).toHaveProperty('disabled', false));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Price impact' })).toBeTruthy());
    await answer('no');
    expect(called('swapasset').length).toBe(1);

    nav.tab = 'liquidity';
    await waitFor(() => expect(document.querySelector('#lq-a')).not.toBeNull());
    await pickAsset('lq-a', SILVER);
    await fireEvent.input(document.querySelector('#lq-amount-a')!, { target: { value: '20' } });
    await fireEvent.input(document.querySelector('#lq-amount-b')!, { target: { value: '1' } });
    // 20 SILVER and 1 CHN make too few shares to open a pool.
    await waitFor(() => expect(screen.getByText(/Too little to open a pool/)).toBeTruthy());
    await fireEvent.input(document.querySelector('#lq-amount-a')!, { target: { value: '30' } });
    await fireEvent.input(document.querySelector('#lq-amount-b')!, { target: { value: '5' } });
    await waitFor(() => expect(screen.getByText('Share of the pool')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Make the pool/ }));
    await answer('yes');
    // Both amounts, and that they set the price (audit 7 E-BA1).
    await waitFor(() => expect(called('addliquidity')[0]?.params).toEqual([assetArg(SILVER), '30', 'CHN', '5', 1, true]));

    nav.tab = 'pools';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Pools' })).toBeTruthy());
  });

  it('Auctions: a bid quoted by the node, buying all that is left, and an auction started', async () => {
    render(Auctions);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('img', { name: 'The price of one unit, block by block' })).toBeTruthy());
    await fireEvent.input(document.querySelector('#bid-amount')!, { target: { value: '1' } });
    await waitFor(() => expect(called('quotebid').length).toBeGreaterThan(0));
    await waitFor(() => expect(screen.getByRole('button', { name: 'Bid' })).toHaveProperty('disabled', false));
    await fireEvent.click(screen.getByRole('button', { name: 'Bid' }));
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('#1739-0029 · "GOLD"'));
    await answer('yes');
    await waitFor(() => expect(called('bidauction')[0]?.params).toEqual([id('d'), '1']));
    // Done (the host confirmed and ran it): the form is cleared.
    await waitFor(() => expect((document.querySelector('#bid-amount') as HTMLInputElement).value).toBe(''));
    await fireEvent.click(screen.getByRole('button', { name: 'Buy all that is left' }));
    await answer('yes');
    await waitFor(() => expect(called('bidauction')[1]?.params).toEqual([id('d'), '5.4', true]));

    nav.tab = 'sell';
    await waitFor(() => expect(document.querySelector('#sell-amount')).not.toBeNull());
    await waitFor(() => expect((document.querySelector('#sell-asset') as HTMLElement).dataset.value).toBe(GOLD));
    await fireEvent.input(document.querySelector('#sell-amount')!, { target: { value: '50' } });
    await fireEvent.input(document.querySelector('#sell-start')!, { target: { value: '10' } });
    await fireEvent.input(document.querySelector('#sell-end')!, { target: { value: '20' } });
    await waitFor(() => expect(screen.getByText('The price falls: the end price is at most the start price.')).toBeTruthy());
    await fireEvent.input(document.querySelector('#sell-end')!, { target: { value: '4' } });
    await waitFor(() => expect(screen.getByRole('img', { name: 'The price of one unit over the auction' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Start the auction/ }));
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('#1739-0029 · "GOLD"'));
    await answer('yes');
    await waitFor(() => expect(called('createauction')[0]?.params).toEqual([assetArg(GOLD), '50', 'CHN', '10', '4', 60, 1]));
  });

  it('Send: an asset chosen at the top of the page, sent with sendasset', async () => {
    sendIntent.asset = GOLD;
    render(LocalSend);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#asset-amount')).not.toBeNull());
    await fireEvent.input(document.querySelector('#asset-to')!, { target: { value: OTHER } });
    await fireEvent.input(document.querySelector('#asset-amount')!, { target: { value: '12.5' } });
    const send = screen.getByRole('button', { name: /^Send$/ });
    await waitFor(() => expect(send).toHaveProperty('disabled', false), { timeout: 3000 });
    await fireEvent.click(send);
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('Send 12.5 #1739-0029 · "GOLD" to this address?'));
    await answer('yes', 4000);
    await waitFor(() => expect(called('sendasset')[0]?.params).toEqual([OTHER, assetArg(GOLD), '12.5']));
  });

  it('Make the pool: what was confirmed is signed, though the pool appears while the dialog is open (audit 7 E-BA1)', async () => {
    render(Trade);
    render(DialogHost);
    await loaded();
    nav.tab = 'liquidity';
    await waitFor(() => expect(document.querySelector('#lq-a')).not.toBeNull());
    await pickAsset('lq-a', SILVER);
    await fireEvent.input(document.querySelector('#lq-amount-a')!, { target: { value: '30' } });
    await fireEvent.input(document.querySelector('#lq-amount-b')!, { target: { value: '5' } });
    await waitFor(() => expect(screen.getByText('Share of the pool')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Make the pool/ }));
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('Make the #4218-0372 · "SILVER" / CHN pool with 30'));
    // Someone opens SILVER/CHN at 1 SILVER = 1000 CHN, and the store refreshes, while the dialog is open.
    pools.push({ pool: id('q'), asset_a: 'SILVER', asset_b: 'CHN', asset_a_id: SILVER, asset_b_id: CHN_ID, reserve_a: 10, reserve_b: 10000, price: 1000, shares: 316227, volume_a: 0, volume_b: 0, swaps: 0, created: 20, abandoned: false });
    await refreshBitAssets();
    await answer('yes');
    // The amounts confirmed, and that they set the price: the wallet refuses it now, never 30,000 CHN.
    await waitFor(() => expect(called('addliquidity')[0]?.params).toEqual([assetArg(SILVER), '30', 'CHN', '5', 1, true]));
  });

  it('pools and auctions are read a page at a time, with more on demand (audit 7 E-BA3)', async () => {
    auctions = Array.from({ length: 250 }, (_, i) => ({ ...auctions[0], auction: (i + 0x1000).toString(16).padStart(64, '0') }));
    render(Auctions);
    await loaded();
    expect(ba.auctions.length).toBe(200);
    expect(called('listauctions').every((c) => (c.params as unknown[])[0] === false && Number((c.params as unknown[])[1]) <= 1000)).toBe(true);
    await waitFor(() => expect(screen.getByText(/Showing the newest 200 of 250 auctions/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Show more' }));
    await waitFor(() => expect(ba.auctions.length).toBe(250));
    expect(screen.queryByText(/Showing the newest/)).toBeNull();
    // A receipt of an auction not in the page read (the fake node's getauction knows it): asked for by itself.
    mine.receipts = [{ auction: id('e') }];
    await refreshBitAssets();
    await waitFor(() => expect(ba.receiptAuctions.map((a) => a.auction)).toEqual([id('e')]));
    // The wallet's part every few seconds; the lists of the chain only with a block.
    const before = called('listpools').length;
    await refreshBitAssets({ node: false });
    expect(called('listpools').length).toBe(before);
    expect(called('listmyassets').length).toBeGreaterThan(1);
  });

  it('a registration dropped from the mempool is sent again (audit 7 E-BA7)', async () => {
    store = { pending: [{ wallet: 'w', name: 'IRON', asset: id('f'), supply: '1', decimals: 0, data: {}, public: true, time: 1, reservation: id('r'), registration: id('6'), registeredAt: 18 }], prices: {} };
    // Accepted with its reservation: the host sends it again without a question, as its transaction left the mempool.
    authorized.push(JSON.stringify(['w', 'registerasset', ['IRON', '1', 0, {}, true]]));
    await refreshBitAssets();
    // Two blocks on (21 >= 18 + 2), not in the mempool (the fake node knows no getmempoolentry): registered again.
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(called('getmempoolentry')[0].params).toEqual([id('6')]);
    expect(ba.journal.pending[0].registration).toBe(id('7'));
    expect(ba.journal.pending[0].registeredAt).toBe(21);
    expect(asked).toEqual([]);
  });

  it('names an asset in a confirmation by its label and number, or the start of its hash (audit port-2 #5)', async () => {
    await refreshBitAssets();
    expect(nameOf(GOLD)).toBe('#1739-0029 · "GOLD"');
    expect(nameOf(CHN_ID)).toBe('CHN');
    // Not listed (a private name): the start and end of its hash.
    expect(nameOf('9'.repeat(60) + 'abcd', 'G0LD')).toBe('0x99999999…abcd · "G0LD"');
    expect(nameOf('8'.repeat(64))).toBe('0x88888888…8888');
  });

  it('Assets: every asset, past the first 1000, and the newest past the most read (audit port-2 #4)', async () => {
    assets = Array.from({ length: 1025 }, (_, i) => asset(i.toString(16).padStart(64, '0').replace(/^0/, '7'), { label: `F${String(i + 1).padStart(4, '0')}`, name: `F${i + 1}`, seq: `${1000 + i}-0001`, registered: i + 1 }));
    render(Assets);
    await waitFor(() => expect(ba.assets.length).toBe(1025), { timeout: 10000 });
    expect(called('listassets').map((c) => c.params)).toEqual([[ASSET_PAGE, 0], [ASSET_PAGE, ASSET_PAGE]]);
    await waitFor(() => expect(screen.getByRole('button', { name: /^All/ }).textContent).toContain('1025'));
    // The newest first, findable by typing.
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'F1025' } });
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').length).toBe(1));
    // Drawn a few hundred at a time.
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: '' } });
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').length).toBe(240));
    await fireEvent.click(screen.getByRole('button', { name: /Show more/ }));
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').length).toBe(480));
    // Past the most read, the newest are read, and typing asks the node for the others.
    calls.length = 0;
    await readAllAssets(MAX_ASSETS + 5000);
    expect(called('listassets').map((c) => (c.params as number[])[1])).toEqual([0, 5000]);
    hidden = [asset('6'.repeat(64), { label: 'ZINC', name: 'ZINC', seq: '5555-0001', registered: 2000 })];
    await refreshBitAssets();
    await waitFor(() => expect(screen.getByText(/oldest of the 1\D?026 assets are not listed here/)).toBeTruthy());
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'zin' } });
    await waitFor(() => expect(called('listassets').some((c) => (c.params as unknown[])[2] === 'zin')).toBe(true));
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getByText('ZINC')).toBeTruthy());
  }, 30000);

  it('My assets: older activity read on asking; tables are cards when narrow; no faint text under 4.5:1 (audit port-2 #13)', async () => {
    activityCount = 450;
    render(MyAssets);
    await loaded();
    nav.tab = 'activity';
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Activity' })).getAllByRole('row').length).toBe(ACTIVITY_PAGE + 1));
    await fireEvent.click(screen.getByRole('button', { name: 'Show older activity' }));
    await waitFor(() => expect(baView.activity).toBe(2 * ACTIVITY_PAGE));
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Activity' })).getAllByRole('row').length).toBe(2 * ACTIVITY_PAGE + 1));
    const css = (await import('node:fs')).readFileSync(`${__dirname}/ba/ba.css`, 'utf8');
    expect(css).toMatch(/\.ba-wrap \{[^}]*container-type: inline-size/);
    expect(css).toMatch(/@container \(max-width: 56rem\) \{[\s\S]*\.ba-table thead \{\s*display: none;/);
    expect(css).toMatch(/\.ba-page \.faint \{\s*color: var\(--text-muted\);/);
  });

  it('the cards of the home: holdings, markets, auctions', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'My assets' })).toBeTruthy());
    expect(screen.getByRole('heading', { name: 'Markets' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Auctions' })).toBeTruthy();
    // 200 GOLD at 0.1 CHN.
    expect(screen.getByText('20 CHN')).toBeTruthy();
  });

  it('the assets the wallet holds, on the Home and the Overview: by worth, with their numbers and balances', async () => {
    for (const which of ['home', 'overview']) {
      const shown = which === 'home' ? render(LocalCards) : render(LocalOverview);
      await loaded();
      const held = await waitFor(() => screen.getByTestId('held-assets'));
      expect(within(held).getByRole('heading', { name: 'Your assets' })).toBeTruthy();
      const rows = [...held.querySelectorAll('tbody tr')].map((r) => r.textContent ?? '');
      // GOLD first (a pool prices it: 200 at 0.1 CHN), then SILVER (no price: –).
      expect(rows[0]).toContain('GOLD');
      expect(rows[0]).toContain('#1739-0029');
      expect(rows[0]).toContain('200');
      expect(rows[0]).toContain('20');
      expect(rows[1]).toContain('SILVER');
      expect(rows[1]).toContain('–');
      shown.unmount();
    }
  });

  it('no assets yet: the way to get some', async () => {
    mine = { assets: [], reservations: [], liquidity: [], receipts: [] };
    render(LocalOverview);
    await waitFor(() => expect(ba.mineState).toBe('ok'));
    const held = await waitFor(() => screen.getByTestId('held-assets'));
    await waitFor(() => expect(within(held).getByRole('button', { name: 'Register new asset' })).toBeTruthy());
    expect(within(held).getByRole('button', { name: 'Get assets in Trade' })).toBeTruthy();
  });
});

// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): Swap with a plain quote, My assets, Create a
// token; pools, liquidity and auctions in Advanced. The same calls as Advanced.
describe('Simple mode', () => {
  beforeEach(() => {
    prefs.uiMode = 'simple';
  });

  it('leaves the auctions out of the navigation, and has three first steps of BitAssets', async () => {
    expect((LOCAL_PAGES as readonly LocalPage[]).find((p) => p.id === 'auctions')?.advanced).toBe(true);
    expect((LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.advanced).map((p) => p.id)).toEqual(['auctions']);
    const { FIRST_STEPS } = await import('./firststeps');
    expect(FIRST_STEPS.map((s) => s.action?.page)).toEqual(['peg', 'trade', 'issue']);
  });

  it('Swap: the quote in plain words, the slippage under More options, with the call of Advanced', async () => {
    render(Trade);
    render(DialogHost);
    await loaded();
    expect(screen.getByRole('heading', { name: 'Swap' })).toBeTruthy();
    // No tabs of pools nor liquidity, nor the curve of the pool.
    expect(screen.queryByRole('tab', { name: 'Pools' })).toBeNull();
    expect(screen.queryByRole('heading', { name: 'The pool' })).toBeNull();
    expect(screen.getByRole('heading', { name: 'How a swap works' })).toBeTruthy();
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(GOLD));
    expect(screen.queryByRole('group', { name: 'Slippage' })).toBeNull();
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(screen.getByTestId('swap-plain').textContent).toMatch(/You pay 0\.5\s*CHN and get about/));
    expect(screen.getByTestId('swap-plain').textContent).toMatch(/At least .* if the price moves first/);
    // The plain summary names the asset with its number (audit 7 E-BA4).
    expect(screen.getByTestId('swap-plain').textContent).toContain('#1739-0029 · "GOLD"');
    await fireEvent.click(screen.getByRole('button', { name: /More options/ }));
    await fireEvent.click(screen.getByRole('button', { name: '1%' }));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    await answer('yes');
    await waitFor(() => expect(called('swapasset')[0]?.params).toEqual(['CHN', '0.5', assetArg(GOLD), 1, 10, '4.88']));
    // Done (the host confirmed and ran it): the form is cleared.
    await waitFor(() => expect((document.querySelector('#swap-amount') as HTMLInputElement).value).toBe(''));
    // Asked for by a link: the pools are shown.
    nav.tab = 'pools';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Pools' })).toBeTruthy());
  });

  it('Create a token: the wizard of Advanced in plain words, the same calls', async () => {
    render(Issue);
    render(DialogHost);
    await waitFor(() => expect(ba.loaded).toBe(true));
    expect(screen.getByRole('heading', { name: 'Create a token' })).toBeTruthy();
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // The decimals under More options (2 by default).
    expect(document.querySelector('#issue-decimals')).toBeNull();
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000.25' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    expect(screen.queryByRole('checkbox', { name: /Publish the name/ })).toBeNull();
    await fireEvent.input(document.querySelector('#issue-info')!, { target: { value: 'Copper, by the gram' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: /Create the token/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    await waitFor(() => expect(store).toMatchObject({ pending: [{ wallet: 'w', name: 'COPPER', supply: '1000.25', decimals: 2, data: { info: 'Copper, by the gram' }, public: true }] }));
  });

  it('My assets: holdings, what is being created and the activity; pool shares and auctions in Advanced', async () => {
    render(MyAssets);
    await loaded();
    const tabs = () => screen.getAllByRole('tab').map((x) => x.textContent?.trim());
    await waitFor(() => expect(tabs()).toEqual(['Holdings', 'Create a token', 'Being created', 'Activity']));
    expect(screen.getByRole('list', { name: 'Holdings' })).toBeTruthy();
    expect(screen.getByRole('button', { name: /Create a token/ })).toBeTruthy();
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(tabs()).toEqual(['Holdings', 'Register new asset', 'Being issued', 'Pool shares', 'My auctions', 'Activity']));
  });

  it('the cards of the home: no auctions card', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'My assets' })).toBeTruthy());
    expect(screen.queryByRole('heading', { name: 'Auctions' })).toBeNull();
    expect(screen.getByRole('button', { name: /Create a token/ })).toBeTruthy();
  });
});

describe('audit 7 E-BA6: amounts past 2^53 units, exact from the node to the dialog', () => {
  // 2^60 + 1 units of an asset with 8 decimals: no double holds it (1152921504606846977).
  const BIG = '11529215046.06846977';
  const NUM = '\u0000num:';
  /** The node writes these as JSON numbers: in the fixtures, marked, then written bare in the text of the reply. */
  const num = (digits: string) => `${NUM}${digits}` as unknown as number;
  const rawJson = (value: unknown) => JSON.stringify(value).replace(new RegExp(`"\\\\u0000num:([^"]+)"`, 'g'), '$1');

  beforeEach(() => {
    assets[0] = { ...assets[0], decimals: 8, supply: num('23058430092.13693953'), minted: num('23058430092.13693953') };
    mine.assets[0] = { ...mine.assets[0], decimals: 8, balance: num(BIG), supply: num('23058430092.13693953') };
    auctions[0] = { ...auctions[0], base: 'GOLD', quote: 'CHN', amount: num('11529215046.06846978'), remaining: num(BIG), cost_of_remaining: num('9007199254.74099313') };
    // The host gives the results of the methods BitAssets reads exactly as text, as the node wrote them.
    setTransport(async (r) => {
      const result = await fakeNode(r);
      if (!r.exact) {
        expect(JSON.stringify(result ?? null)).not.toContain('\\u0000num:');
        return result;
      }
      return { $rawjson: rawJson(result ?? null) };
    });
  });

  it('a balance is read, shown and filled in by Max with every digit', async () => {
    render(LocalSend);
    render(DialogHost);
    await loaded();
    expect(ba.mine!.assets[0].balance).toBe(BIG);
    sendIntent.asset = GOLD;
    await waitFor(() => expect(document.querySelector('#asset-amount')).not.toBeNull());
    await fireEvent.click(screen.getByRole('button', { name: 'Max' }));
    await waitFor(() => expect((document.querySelector('#asset-amount') as HTMLInputElement).value).toBe(BIG));
    await fireEvent.input(document.querySelector('#asset-to')!, { target: { value: OTHER } });
    const send = screen.getByRole('button', { name: /^Send$/ });
    await waitFor(() => expect(send).toHaveProperty('disabled', false), { timeout: 3000 });
    await fireEvent.click(send);
    // The confirmation of the page, then what the host is asked to confirm and run: the same digits.
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain(`Send 11,529,215,046.06846977 #1739-0029 · "GOLD" to this address?`));
    await answer('yes', 4000);
    await waitFor(() => expect(called('sendasset')[0]?.params).toEqual([OTHER, assetArg(GOLD), BIG]));
  });

  it('My assets shows the balance and the supply exactly', async () => {
    render(MyAssets);
    await loaded();
    await waitFor(() => expect(document.body.textContent).toContain('11,529,215,046.06846977'));
  });

  it('Buy all pays the cost of what is left exactly, and says it so', async () => {
    render(Auctions);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.body.textContent).toContain('11,529,215,046.06846977'));
    await fireEvent.click(screen.getByRole('button', { name: 'Buy all that is left' }));
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('Pay 9,007,199,254.74099313 CHN for the 11,529,215,046.06846977'));
    await answer('yes');
    await waitFor(() => expect(called('bidauction')[0]?.params).toEqual([id('d'), '9007199254.74099313', true]));
  });
});

describe('audit 7: names and amounts', () => {
  it('names that carry a number are refused, as the wallet refuses them (E-BA4)', () => {
    for (const n of ['GOLD (#1739-0029)', 'GOLD #2', 'GOLD 1739-0029', '(x)']) expect(nameMimicsNumber(n)).toBe(true);
    for (const n of ['GOLD', 'GOLD-2026', '123-4567']) expect(nameMimicsNumber(n)).toBe(false);
    expect(nameProblem('GOLD (#1739-0029)')).toMatch(/would read as the number of another asset/);
  });

  it('amounts past 2^53 units never round above a balance, nor below a cap (E-BA6)', () => {
    // 1234567.890123456789 with 12 decimals: the double reads it as …4567 17.
    const v = 1234567.890123456789;
    const exact = 1234567890123456789n;
    expect(unitsAtMost(v, 12) <= exact).toBe(true);
    expect(unitsAtLeast(v, 12) >= exact).toBe(true);
    expect(exact - unitsAtMost(v, 12) < 10_000n).toBe(true);
    // Exact amounts are left as they are.
    expect(unitsAtMost(12.5, 2)).toBe(1250n);
    expect(unitsAtLeast('9223372.036854775807', 12)).toBe(9223372036854775807n);
  });
});

// What BitAssets knows of a transaction of the wallet beyond CHN, for the Transactions page (pages/Transactions.svelte):
// the assets it moved (listassettransactions, read with the store's refresh: one call for the wallet, never one per row),
// what kind of move, how much, and the other side: where a send went, who paid what came in.
import { t } from '../lib/i18n.svelte';
import { wallets } from '../lib/state.svelte';
import { formatAmount, type AssetTx } from './ba/model';
import { ba, holdingOf, refreshBitAssets, startBitAssets } from './ba/store.svelte';
import { ignoreAsset } from './ba/hide.svelte';
import { hiddenAsset } from './ba/visible.svelte';

export interface ChainTx {
  type: string;
  search: string;
  csv: string[];
}

export const CHAIN_TX_FILTER = 'Assets';
export const CHAIN_TX_COLUMN = 'Asset';
export const CHAIN_TX_CSV = ['Asset', 'Asset number', 'Asset amount', 'Asset counterparty'];

/** The words of a kind of move. */
export function categoryText(e: Pick<AssetTx, 'category' | 'sign'>): string {
  switch (e.category) {
    case 'send':
      return t('Sent asset');
    case 'receive':
      return t('Received asset');
    case 'register':
      return t('Registered');
    case 'mint':
      return t('Minted');
    case 'burn':
      return t('Burned');
    case 'swap':
      return t('Swap');
    case 'pool_add':
      return t('Pool add');
    case 'pool_remove':
      return t('Pool remove');
    case 'auction':
      return t('Auction');
    case 'bid':
      return t('Auction bid');
    case 'collect':
      return t('Auction collect');
    case 'logo':
      return t('Logo set');
    case 'update':
      return t('Asset data');
    case 'control':
      return e.sign > 0 ? t('Control received') : t('Control given');
    case 'reserve':
      return t('Name reserved');
    case 'release':
      return t('Asset retired');
    default:
      return t('Asset');
  }
}

/** An amount with its sign, in the asset's decimals, exact. */
export function signedAmount(e: AssetTx): string {
  if (e.amount === undefined) return '';
  const text = formatAmount(e.amount, e.decimals ?? 0, { trim: false });
  const bound = e.at_least ? '≥ ' : e.at_most ? '≤ ' : '';
  return `${e.sign < 0 ? '−' : e.sign > 0 ? '+' : ''}${bound}${text}`;
}

/**
 * The other side of a move: where it went (a send, a mint), who paid it (a receive). Each address once; the wallet's own
 * outputs are not listed one by one (a mint pays the minted coins and the control coin to the wallet, a swap its result
 * and its change): `self` says the wallet is on that side, the parties are the others (user feedback 2026-10-19).
 */
export function counterparty(e: AssetTx): { dir: 'to' | 'from' | ''; parties: AssetTx['to']; self: boolean } {
  const side = (list: AssetTx['to']) => {
    const seen = new Set<string>();
    const others = list.filter((p) => !p.mine && !seen.has(p.address) && (seen.add(p.address), true));
    return { parties: others, self: list.some((p) => p.mine) };
  };
  if (e.category === 'receive' || (e.sign > 0 && e.from.length)) return { dir: 'from', ...side(e.from) };
  if (e.sign < 0 || e.category === 'mint' || e.category === 'register') {
    const to = side(e.to);
    // What went out to others: the wallet's own outputs beside them are its change, not a destination.
    if (to.parties.length) return { dir: 'to', parties: to.parties, self: false };
    return to.self ? { dir: 'to', parties: [], self: true } : { dir: '', parties: [], self: false };
  }
  return { dir: '', parties: [], self: false };
}

/** An address in a few characters: its start and its end ("tba1q…a9x"), whole when short. */
export function shortAddress(address: string): string {
  return address.length > 16 ? `${address.slice(0, 8)}…${address.slice(-4)}` : address;
}

let byTxid = new Map<string, AssetTx[]>();
let byTxidOf: unknown = null;

/** The asset moves of a transaction of the wallet loaded, in the order listed. */
export function assetMovesOf(txid: string): AssetTx[] {
  const list = ba.transfersWallet === wallets.current ? ba.transfers : [];
  if (byTxidOf !== list) {
    byTxid = new Map();
    for (const e of list) byTxid.set(e.txid, [...(byTxid.get(e.txid) ?? []), e]);
    byTxidOf = list;
  }
  return byTxid.get(txid) ?? [];
}

/** What chainTxOf made for each transaction, while the list read stays the same (a page asks it for every row, often). */
let made = new Map<string, ChainTx | undefined>();
let madeOf: unknown = null;

export function chainTxOf(txid: string): ChainTx | undefined {
  const moves = assetMovesOf(txid);
  if (madeOf !== byTxidOf) {
    made = new Map();
    madeOf = byTxidOf;
  }
  if (made.has(txid)) return made.get(txid);
  const out = makeChainTx(moves);
  made.set(txid, out);
  return out;
}

function makeChainTx(moves: AssetTx[]): ChainTx | undefined {
  if (!moves.length) return undefined;
  const parties = (e: AssetTx) => counterparty(e).parties.map((p) => p.address);
  return {
    type: categoryText(moves[0]),
    search: moves.map((e) => [e.label ?? '', e.seq ? `#${e.seq}` : '', e.asset ?? '', categoryText(e), ...parties(e)].join(' ')).join(' '),
    csv: [
      moves.map((e) => e.label ?? '').join('; '),
      moves.map((e) => e.seq ?? '').join('; '),
      moves.map((e) => signedAmount(e).replace('−', '-')).join('; '),
      moves.map((e) => { const c = counterparty(e); return c.dir ? `${c.dir} ${[...c.parties.map((p) => p.address), ...(c.self ? ['this wallet'] : [])].join(' ')}` : ''; }).join('; '),
    ],
  };
}

/**
 * Whether the transaction only moved assets the wallet hides (ignored, or left out by its filters: ba/hide.svelte.ts):
 * the Transactions page folds it into "N hidden transactions", the notices leave it out (user feedback 2026-10-19).
 */
export function chainTxHidden(txid: string): boolean {
  const moves = assetMovesOf(txid);
  return moves.length > 0 && moves.every((e) => !!e.asset && hiddenAsset(e.asset, e.label));
}

/** What the notice of an asset received offers (Ignore this asset) and warns of (a name like one the wallet holds). */
export function chainTxNotice(txid: string): { warning?: string; actions?: { label: string; run: () => void }[] } | undefined {
  const got = assetMovesOf(txid).filter((e) => e.asset && e.sign > 0 && e.category === 'receive');
  if (!got.length) return undefined;
  const e = got[0];
  const asset = e.asset!;
  const label = (e.label ?? '').toLowerCase();
  // Another asset of the wallet of the same name (any case): a look-alike, told apart by its number.
  const twin = label ? (ba.mine?.assets ?? []).find((h) => h.asset !== asset && h.label.toLowerCase() === label && !!holdingOf(h.asset)) : undefined;
  return {
    warning: twin ? t('Same name as an asset you hold: this is #%1, not the one you hold.', e.seq ?? asset.slice(0, 8)) : undefined,
    actions: [{ label: t('Ignore this asset'), run: () => ignoreAsset(asset) }],
  };
}

// The asset moves of the wallet read again on its news, at most once a second while transactions keep coming (a
// script making assets: each read worked out the Transactions page again; user feedback 2026-10-19, item 10).
let lastRead = 0;
let readLater: ReturnType<typeof setTimeout> | null = null;

export function refreshChainTx(wallet: string): void {
  void wallet;
  startBitAssets();
  if (readLater) return;
  const wait = Math.max(0, lastRead + 1000 - Date.now());
  const read = () => {
    readLater = null;
    lastRead = Date.now();
    void refreshBitAssets({ node: false });
  };
  if (wait === 0) read();
  else readLater = setTimeout(read, wait);
}

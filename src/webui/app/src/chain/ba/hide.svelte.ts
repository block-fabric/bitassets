// The assets a wallet hides (user feedback 2026-10-19, item 19): assets it ignores (spam sent to it, many assets of one
// name), names and words it ignores, and filters (dead assets: no activity for a while, assets burned to nothing, assets without a pool). Per
// wallet, kept with the app's preferences (per network, as asset ids are), so that they survive a restart. Hiding is
// display-only: the coins stay in the wallet, and an asset comes back when unignored. CHN is never hidden.
//
// Every list of assets asks `assetHidden` (Browse assets, the Send picker, Trade, Auctions, My assets, the home's
// "Your assets", the Transactions page through chainTxHidden, the notices), each with a "show hidden" toggle.
import { host } from '../../lib/rpc';
import { wallets } from '../../lib/state.svelte';

export interface AssetFilters {
  /** Assets ignored, by id. */
  ignored: string[];
  /** Names ignored (any case, the whole name): every asset of such a name. */
  names: string[];
  /** Words ignored (any case, anywhere in the name). */
  keywords: string[];
  /** Hide dead assets: nothing done with them (no move, trade, mint or burn) in the last `deadAfter` blocks. */
  hideDead: boolean;
  /** Dead after this many blocks without activity; 0: the node's volume window (about a day). */
  deadAfter: number;
  /** Hide assets burned to nothing (no supply left). */
  hideBurned: boolean;
  /** Hide assets without a pool (no market). */
  hideNoPool: boolean;
}

export const NO_FILTERS: AssetFilters = { ignored: [], names: [], keywords: [], hideDead: false, deadAfter: 0, hideBurned: false, hideNoPool: false };

export const hiding = $state({ byWallet: {} as Record<string, AssetFilters>, loaded: false, version: 0 });

/** What a list knows of an asset to tell whether a filter hides it. */
export interface AssetFacts {
  id: string;
  label?: string;
  /** The height of the last block that touched it (searchassets "last_activity"), when known. */
  lastActivity?: number;
  /** The height of the tip, when known. */
  tip?: number;
  supply?: number | string;
  burned?: number | string;
  /** It has a pool. */
  pool?: boolean;
}

const isChn = (id: string) => /^0+$/.test(id);

/** The node's volume window (Consensus asset_volume_window: 1440 blocks, about a day): dead by default after it. */
export const DEFAULT_DEAD_AFTER = 1440;
export const MAX_DEAD_AFTER = 10_000_000;

/**
 * Whether an asset is dead for these filters: no activity in the last `deadAfter` blocks (the node's searchassets
 * exclude_dead: touched at a height below tip - blocks + 1). Unknown (no activity or tip told) is not dead here: the
 * node's search leaves those out on its side.
 */
export function assetDead(a: Pick<AssetFacts, 'lastActivity' | 'tip'>, f: Pick<AssetFilters, 'deadAfter'>): boolean {
  if (a.lastActivity === undefined || a.tip === undefined) return false;
  return a.tip - a.lastActivity >= (f.deadAfter > 0 ? f.deadAfter : DEFAULT_DEAD_AFTER);
}

function clean(raw: unknown): AssetFilters {
  const o = (raw && typeof raw === 'object' ? raw : {}) as Record<string, unknown>;
  const strings = (v: unknown, max: number, re?: RegExp) => (Array.isArray(v) ? [...new Set(v.filter((x): x is string => typeof x === 'string' && x.length > 0 && x.length <= 64 && (!re || re.test(x))))].slice(0, max) : []);
  return {
    ignored: strings(o.ignored, 5000, /^[0-9a-f]{64}$/),
    names: strings(o.names, 500),
    keywords: strings(o.keywords, 500),
    hideDead: o.hideDead === true,
    deadAfter: typeof o.deadAfter === 'number' && Number.isInteger(o.deadAfter) && o.deadAfter >= 1 && o.deadAfter <= MAX_DEAD_AFTER ? o.deadAfter : 0,
    hideBurned: o.hideBurned === true,
    hideNoPool: o.hideNoPool === true,
  };
}

let loading: Promise<void> | null = null;

/** Read them from the preferences, once. */
export function loadHiding(): Promise<void> {
  loading ??= host
    .prefs()
    .then((raw) => {
      const all = raw?.assetFilters;
      const out: Record<string, AssetFilters> = {};
      if (all && typeof all === 'object') for (const [w, f] of Object.entries(all as Record<string, unknown>)) out[w] = clean(f);
      hiding.byWallet = out;
    })
    .catch(() => undefined)
    .finally(() => {
      hiding.loaded = true;
      hiding.version++;
    });
  return loading;
}

/** The filters of a wallet (the one shown by default). */
export function filtersOf(wallet: string | null = wallets.current): AssetFilters {
  return (wallet !== null && hiding.byWallet[wallet]) || NO_FILTERS;
}

/** Change the filters of a wallet, and keep them. */
export function setFilters(changes: Partial<AssetFilters>, wallet: string | null = wallets.current): void {
  if (wallet === null) return;
  hiding.byWallet = { ...hiding.byWallet, [wallet]: clean({ ...filtersOf(wallet), ...changes }) };
  hiding.version++;
  host.setPrefs({ assetFilters: hiding.byWallet }).catch(() => undefined);
}

export function ignoreAsset(id: string, wallet: string | null = wallets.current): void {
  if (isChn(id)) return;
  const f = filtersOf(wallet);
  if (!f.ignored.includes(id)) setFilters({ ignored: [...f.ignored, id] }, wallet);
}

export function unignoreAsset(id: string, wallet: string | null = wallets.current): void {
  const f = filtersOf(wallet);
  setFilters({ ignored: f.ignored.filter((x) => x !== id) }, wallet);
}

/** Ignore every asset of a name (any case). */
export function ignoreName(name: string, wallet: string | null = wallets.current): void {
  const n = name.trim();
  if (!n || n.toUpperCase() === 'CHN') return;
  const f = filtersOf(wallet);
  if (!f.names.some((x) => x.toLowerCase() === n.toLowerCase())) setFilters({ names: [...f.names, n] }, wallet);
}

export function unignoreName(name: string, wallet: string | null = wallets.current): void {
  const f = filtersOf(wallet);
  setFilters({ names: f.names.filter((x) => x.toLowerCase() !== name.toLowerCase()) }, wallet);
}

/** Whether the wallet ignores the asset itself, its name, or a word of it (the filters aside). */
export function assetIgnored(id: string, label = '', wallet: string | null = wallets.current): boolean {
  if (isChn(id)) return false;
  void hiding.version;
  const f = filtersOf(wallet);
  if (f.ignored.includes(id)) return true;
  if (!label) return false;
  const l = label.toLowerCase();
  return f.names.some((n) => n.toLowerCase() === l) || f.keywords.some((k) => l.includes(k.toLowerCase()));
}

/** Whether the asset is hidden in the lists of the wallet: ignored, or left out by a filter. Never CHN. */
export function assetHidden(a: AssetFacts, wallet: string | null = wallets.current): boolean {
  if (isChn(a.id)) return false;
  if (assetIgnored(a.id, a.label ?? '', wallet)) return true;
  const f = filtersOf(wallet);
  if (f.hideDead && assetDead(a, f)) return true;
  if (f.hideBurned && a.supply !== undefined && Number(a.supply) === 0 && Number(a.burned ?? 0) > 0) return true;
  if (f.hideNoPool && a.pool === false) return true;
  return false;
}

/** The filters as the node's searchassets takes them (server side: lists of millions are never read whole). */
export function searchFiltersOf(wallet: string | null = wallets.current): Record<string, unknown> {
  const f = filtersOf(wallet);
  const out: Record<string, unknown> = {};
  // exclude_dead: a number of blocks, or true for the node's volume window (about a day).
  if (f.hideDead) out.exclude_dead = f.deadAfter > 0 ? f.deadAfter : true;
  if (f.hideBurned) out.exclude_burned = true;
  if (f.hideNoPool) out.has_pool = true;
  const words = [...f.keywords, ...f.names];
  if (words.length) out.exclude_keywords = words;
  return out;
}

/** For the tests: forget what was read. */
export function resetHiding(): void {
  loading = null;
  hiding.byWallet = {};
  hiding.loaded = false;
  hiding.version++;
}

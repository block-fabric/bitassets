// Searching the assets, pools and auctions of the chain on the node, a page at a time (user feedback 2026-10-19: there
// will be hundreds of thousands of assets, then millions: no page reads a whole list). The node's searchassets,
// searchpools and searchauctions take what is typed, filters, a sort, a cursor and a limit (at most 200), and answer
// a page and the cursor of the next. A node without them (an older one) is asked the list methods instead, the first
// page only (listassets with its match; the pools and auctions the store read), filtered and sorted here.
import { BridgeError } from '../../lib/bridge';
import { rpc } from '../../lib/rpc';
import { approx, type Asset, type Auction, type Pool } from './model';
import { ba, noteKnownAssets } from './store.svelte';

export type AssetSort = 'relevance' | 'name' | 'newest' | 'oldest' | 'supply' | 'activity' | 'volume' | 'pool_depth';

/** The filters the node applies (its own words). */
export interface SearchFilters {
  /** Leave out those not touched in the last this many blocks; true: the node's volume window (about a day). */
  exclude_dead?: boolean | number;
  exclude_burned?: boolean;
  exclude_retired?: boolean;
  exclude_keywords?: string[];
  has_pool?: boolean;
  has_logo?: boolean;
  active_within_blocks?: number;
  min_supply?: number;
}

export interface SearchQuery {
  query: string;
  filters: SearchFilters;
  sort: AssetSort;
  cursor?: string | null;
  /** At most MAX_LIMIT. */
  limit?: number;
}

export interface SearchPage<T> {
  items: T[];
  /** The cursor of the next page, null at the end. */
  cursor: string | null;
  /** How many match, when the node says. */
  total?: number;
  /** Asked of the list methods (a node without the search): the first page only. */
  fallback: boolean;
}

export const MAX_LIMIT = 200;
export const PAGE = 100;

/** Whether the node has each method: unknown until asked once. */
const support: Record<string, boolean | undefined> = {};

/** For the tests: forget what the node was found to have. */
export function resetSearchSupport(): void {
  for (const k of Object.keys(support)) delete support[k];
}

/** An error that says the node has no such method (an older node), or the app may not call it. */
function missing(e: unknown): boolean {
  const err = BridgeError.from(e);
  return err.code === -32601 || /method not found|not allowed|unknown method|not permitted/i.test(err.message);
}

function pageOf<T>(raw: unknown, keys: string[]): { items: T[]; cursor: string | null; total?: number } {
  if (Array.isArray(raw)) return { items: raw as T[], cursor: null };
  const o = (raw ?? {}) as Record<string, unknown>;
  const key = keys.find((k) => Array.isArray(o[k]));
  const items = (key ? o[key] : []) as T[];
  const cursor = typeof o.cursor === 'string' && o.cursor ? o.cursor : typeof o.next_cursor === 'string' && o.next_cursor ? o.next_cursor : null;
  return { items, cursor, total: typeof o.total === 'number' ? o.total : undefined };
}

type Params = [string, SearchFilters, AssetSort, string | null, number];

async function search<T>(method: string, ask: (params: Params) => Promise<unknown>, keys: string[], q: SearchQuery, fallback: () => Promise<T[]>): Promise<SearchPage<T>> {
  const limit = Math.max(1, Math.min(MAX_LIMIT, q.limit ?? PAGE));
  if (support[method] !== false) {
    try {
      const raw = await ask([q.query, q.filters, q.sort, q.cursor ?? null, limit]);
      support[method] = true;
      return { ...pageOf<T>(raw, keys), fallback: false };
    } catch (e) {
      if (!missing(e)) throw e;
      support[method] = false;
    }
  }
  // The first page only, whatever the cursor asked.
  if (q.cursor) return { items: [], cursor: null, fallback: true };
  return { items: (await fallback()).slice(0, limit), cursor: null, fallback: true };
}

const words = (f: SearchFilters) => (f.exclude_keywords ?? []).map((w) => w.toLowerCase()).filter(Boolean);

/** What the list methods' answer keeps of the filters (the node's search does it on its side). */
export function filterAssets(list: readonly Asset[], f: SearchFilters, pooled: (id: string) => boolean): Asset[] {
  const ws = words(f);
  return list.filter((a) => {
    // The list methods tell no activity: those anyone may retire (no supply, no pool, no auction) stand for the dead.
    if (f.exclude_dead && a.releasable) return false;
    if (f.exclude_burned && approx(a.supply) === 0 && approx(a.burned) > 0) return false;
    if (f.has_pool && !pooled(a.asset)) return false;
    if (f.has_logo && !a.logo) return false;
    if (f.min_supply !== undefined && approx(a.supply) < f.min_supply) return false;
    if (ws.length && ws.some((w) => a.label.toLowerCase().includes(w))) return false;
    return true;
  });
}

export function sortAssets(list: Asset[], sort: AssetSort): Asset[] {
  const by: Record<string, (a: Asset, b: Asset) => number> = {
    name: (a, b) => a.label.localeCompare(b.label),
    oldest: (a, b) => a.registered - b.registered,
    supply: (a, b) => approx(b.supply) - approx(a.supply),
  };
  return list.sort(by[sort] ?? ((a, b) => b.registered - a.registered));
}

/** A page of the assets of the chain matching what is typed, the filters and the sort. */
export async function searchAssets(q: SearchQuery, pooled: (id: string) => boolean = () => true): Promise<SearchPage<Asset>> {
  const page = await search<Asset>('searchassets', (p) => rpc.raw<unknown>('searchassets', p), ['assets', 'results', 'items'], q, async () => {
    const text = q.query.trim();
    const total = ba.info?.assets ?? 0;
    // The newest page (listassets counts from the first registered), or those whose name holds the text.
    const found = (await rpc.raw<Asset[]>('listassets', text ? [MAX_LIMIT, 0, text] : [MAX_LIMIT, Math.max(0, total - MAX_LIMIT)])) ?? [];
    // The newest the store keeps whose description or number holds the text too (listassets matches names only).
    const t = text.toLowerCase();
    const have = new Set(found.map((a) => a.asset));
    const local = t ? ba.assets.filter((a) => !have.has(a.asset) && ((a.data?.info ?? '').toLowerCase().includes(t) || a.seq === text || a.asset.startsWith(t.replace(/^0x/, '')))) : [];
    return sortAssets(filterAssets([...found, ...local], q.filters, pooled), q.sort);
  });
  noteKnownAssets(page.items);
  return page;
}

/**
 * A page of the assets the wallet holds or controls matching what is typed, the filters and the sort: the wallet's
 * searchmyassets (the same arguments as searchassets; `mine` is not a filter of searchassets). A node without it: the
 * wallet's own list (`own`), filtered and sorted here, the first page only.
 */
export async function searchMyAssets(q: SearchQuery, wallet: string, own: () => Asset[], pooled: (id: string) => boolean = () => true): Promise<SearchPage<Asset>> {
  const page = await search<Asset>('searchmyassets', (p) => rpc.raw<unknown>('searchmyassets', p, { wallet }), ['assets', 'results', 'items'], q, async () => {
    const t = q.query.trim().toLowerCase();
    const list = own().filter((a) => !t || a.label.toLowerCase().includes(t) || a.seq === q.query.trim());
    return sortAssets(filterAssets(list, q.filters, pooled), q.sort);
  });
  noteKnownAssets(page.items);
  return page;
}

/** A page of the pools matching what is typed (either asset's name), the filters and the sort. */
export async function searchPools(q: SearchQuery): Promise<SearchPage<Pool>> {
  return search<Pool>('searchpools', (p) => rpc.raw<unknown>('searchpools', p), ['pools', 'results', 'items'], q, async () => {
    const t = q.query.trim().toLowerCase();
    const ws = words(q.filters);
    const list = ba.pools.filter((p) => {
      const names = `${p.asset_a} ${p.asset_b}`.toLowerCase();
      return (!t || names.includes(t)) && !ws.some((w) => names.includes(w));
    });
    return sortPools(list, q.sort);
  });
}

export function sortPools(list: Pool[], sort: AssetSort): Pool[] {
  const by: Record<string, (a: Pool, b: Pool) => number> = {
    name: (a, b) => `${a.asset_a}/${a.asset_b}`.localeCompare(`${b.asset_a}/${b.asset_b}`),
    activity: (a, b) => b.swaps - a.swaps,
    volume: (a, b) => b.swaps - a.swaps,
    pool_depth: (a, b) => approx(b.reserve_a) * approx(b.reserve_b) - approx(a.reserve_a) * approx(a.reserve_b),
  };
  return [...list].sort(by[sort] ?? by.activity);
}

/** A page of the auctions matching what is typed (the asset sold or asked), the filters and the sort. */
export async function searchAuctions(q: SearchQuery): Promise<SearchPage<Auction>> {
  return search<Auction>('searchauctions', (p) => rpc.raw<unknown>('searchauctions', p), ['auctions', 'results', 'items'], q, async () => {
    const t = q.query.trim().toLowerCase();
    const ws = words(q.filters);
    const list = ba.auctions.filter((a) => {
      const names = `${a.base} ${a.quote}`.toLowerCase();
      return (!t || names.includes(t)) && !ws.some((w) => names.includes(w));
    });
    return [...list].sort(q.sort === 'oldest' ? (a, b) => a.start_height - b.start_height : q.sort === 'name' ? (a, b) => `${a.base}`.localeCompare(`${b.base}`) : (a, b) => b.start_height - a.start_height);
  });
}

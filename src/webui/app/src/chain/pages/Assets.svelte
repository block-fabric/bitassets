<script lang="ts">
  // Browse assets: the assets of the chain, as cards to search (a name, a number such as 1739-0029, or a hash
  // "0x…": also an asset whose name is private), filter (held or controlled by the wallet, with a market,
  // mintable, fixed, dead) and sort (newest, by name, by supply, by the value of the supply, most traded).
  // An asset opens whole (ba/AssetView.svelte). The Explore tab of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import '../ba/ba.css';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import AssetView from '../ba/AssetView.svelte';
  import { untrack } from 'svelte';
  import { assetArg, formatAmount, formatPrice, isSeq, type Asset } from '../ba/model';
  import { assetById, ba, chnPrice, hasPool, noteKnownAssets, startBitAssets } from '../ba/store.svelte';
  import { PAGE, searchAssets, sortAssets, type AssetSort } from '../ba/search';
  import { hiding, searchFiltersOf } from '../ba/hide.svelte';
  import { hiddenAsset } from '../ba/visible.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import { wallets } from '../../lib/state.svelte';

  $effect(() => startBitAssets());

  type Filter = 'all' | 'mine' | 'market' | 'mintable' | 'fixed' | 'dead';
  type Sort = 'newest' | 'oldest' | 'name' | 'supply' | 'value' | 'traded';
  // The sorts of the node's search (ba/search.ts): worth and trade by the depth and the volume of the pools.
  const SERVER_SORT: Record<Sort, AssetSort> = { newest: 'newest', oldest: 'oldest', name: 'name', supply: 'supply', value: 'pool_depth', traded: 'volume' };

  let query = $state('');
  let filter = $state<Filter>('all');
  let sort = $state<Sort>('newest');
  let shown = $state<string | null>(null);
  let lookupError = $state('');
  let looking = $state(false);
  /** An asset found by its hash or number that the list does not hold (its name is private). */
  let found = $state<Asset | null>(null);
  /** What the wallet hides (its asset filters, ba/hide.svelte.ts) shown anyway. */
  let showHidden = $state(false);

  // The list is the node's, a page at a time as the user scrolls (user feedback 2026-10-19: hundreds of thousands of
  // assets; the page never reads them all): what is typed, the filter and the sort asked of searchassets (or, on a node
  // without it, of listassets: its first page only).
  let results = $state.raw<Asset[]>([]);
  let cursor: string | null = null;
  let loading = $state(false);
  let more = $state(false);
  let fallback = $state(false);
  let searchError = $state('');
  let total = $state<number | undefined>(undefined);
  let run = 0;

  const mineIds = $derived(new Set((ba.mine?.assets ?? []).map((a) => a.asset)));
  const controlled = $derived(new Set((ba.mine?.assets ?? []).filter((a) => a.control).map((a) => a.asset)));

  /** What the filter keeps that the node's search does not tell (held, mintable, fixed, dead), and what the wallet ignores. */
  function keep(a: Asset): boolean {
    if (filter === 'mine' && !mineIds.has(a.asset)) return false;
    if (filter === 'mintable' && a.fixed) return false;
    if (filter === 'fixed' && !a.fixed) return false;
    if (filter === 'dead' && !a.releasable) return false;
    return showHidden || !hiddenAsset(a.asset, a.label);
  }
  const list = $derived(results.filter(keep));
  const hiddenHere = $derived(showHidden ? 0 : results.filter((a) => hiddenAsset(a.asset, a.label)).length);
  const filtersOn = $derived.by(() => {
    void hiding.version;
    return Object.keys(searchFiltersOf()).length > 0;
  });

  /** Ask the node for a page: the first (anew) or the next. Pages the filter empties ask for the next, a few at most. */
  async function fetchPage(anew: boolean): Promise<void> {
    const mine = anew ? ++run : run;
    if (anew) {
      cursor = null;
      results = [];
      more = false;
    }
    loading = true;
    searchError = '';
    try {
      // The assets of the wallet: its own list, no search.
      if (filter === 'mine') {
        const own = (ba.mine?.assets ?? []).map((h) => assetById(h.asset) ?? ({ asset: h.asset, label: h.label, seq: '', decimals: h.decimals, supply: h.supply, minted: h.supply, burned: 0, fixed: h.fixed, registered: 0, registration: '', releasable: false } as Asset));
        const q = query.trim().toLowerCase();
        results = sortAssets(own.filter((a) => !q || a.label.toLowerCase().includes(q) || a.seq === q), SERVER_SORT[sort]);
        return;
      }
      for (let i = 0; i < 5; i++) {
        const page = await searchAssets(
          { query: query.trim(), filters: { ...(showHidden ? {} : searchFiltersOf()), ...(filter === 'market' ? { has_pool: true } : {}) }, sort: SERVER_SORT[sort], cursor, limit: PAGE },
          hasPool,
        );
        if (mine !== run) return;
        const have = new Set(results.map((a) => a.asset));
        results = [...results, ...page.items.filter((a) => !have.has(a.asset))];
        cursor = page.cursor;
        fallback = page.fallback;
        total = page.total;
        more = cursor !== null;
        if (!more || page.items.filter(keep).length >= 12) break;
      }
    } catch (e) {
      if (mine === run) searchError = errorText(e);
    } finally {
      if (mine === run) loading = false;
    }
  }

  // Asked anew as the user types (a moment after), picks a filter or a sort, or the wallet's filters change; and when
  // the node counts other assets (a block registered some).
  $effect(() => {
    void filter;
    void sort;
    void showHidden;
    void hiding.version;
    void ba.info?.assets;
    void wallets.current;
    const q = query;
    if (!ba.loaded) return;
    const timer = setTimeout(() => void untrack(() => fetchPage(true)), q ? 300 : 0);
    return () => clearTimeout(timer);
  });

  /** The next page when the end of the list comes into view (or "Show more"). */
  function atEnd(node: HTMLElement) {
    if (typeof IntersectionObserver === 'undefined') return;
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting) && more && !loading) void fetchPage(false);
    });
    io.observe(node);
    return { destroy: () => io.disconnect() };
  }

  // Opened for an asset (from another page, the palette, the home).
  $effect(() => {
    void nav.focus;
    const key = takeFocus('assets');
    if (key) shown = key;
  });

  const FILTERS: { id: Filter; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'mine', label: 'Held or controlled' },
    { id: 'market', label: 'With a market' },
    { id: 'mintable', label: 'Mintable' },
    { id: 'fixed', label: 'Fixed supply' },
    { id: 'dead', label: 'Dead' },
  ];
  const counts = $derived<Partial<Record<Filter, number>>>({ all: ba.info?.assets ?? undefined, mine: mineIds.size });

  /** Enter: an asset by its exact name, number or hash, also one the list does not hold (a private name). */
  async function lookup() {
    const q = query.trim();
    if (!q) return;
    const exact = ba.assets.find((a) => a.label === q || a.seq === q || `0x${a.asset}` === q.toLowerCase());
    if (exact) {
      shown = exact.asset;
      return;
    }
    if (!(isSeq(q) || /^0x[0-9a-fA-F]{64}$/.test(q) || q.length <= 64)) return;
    looking = true;
    lookupError = '';
    try {
      const a = await rpc.raw<Asset>('getasset', [q]);
      found = a;
      noteKnownAssets([a]);
      shown = a.asset;
    } catch (e) {
      lookupError = errorText(e);
    } finally {
      looking = false;
    }
  }
</script>

<div class="page ba-page assets-page">
  {#if shown}
    <AssetView id={shown} onback={() => (shown = null)} />
  {:else}
    <header class="page-head">
      <h1>Browse assets</h1>
      {#if ba.info}<span class="muted">{formatCount(ba.info.assets)} assets · {formatCount(ba.info.pools)} pools · {formatCount(ba.info.auctions)} auctions · next block {formatCount(ba.info.height)}</span>{/if}
    </header>

    {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}

    <section class="panel finder">
      <form
        class="search"
        onsubmit={(e) => {
          e.preventDefault();
          void lookup();
        }}
      >
        <Icon name="search" size={18} />
        <input id="asset-search" type="search" bind:value={query} placeholder="Find an asset by its name, number (1739-0029) or hash (0x…)" autocomplete="off" spellcheck="false" aria-label="Find an asset" />
        <button type="submit" class="btn small" disabled={looking || !query.trim()}>{looking ? 'Looking…' : 'Find'}</button>
      </form>
      {#if lookupError}<p class="ba-problems" role="status"><SafeText text={lookupError} /></p>{/if}
      <div class="bar">
        <div class="ba-chips" role="group" aria-label="Filter">
          {#each FILTERS as f (f.id)}
            <button type="button" class="ba-chip" aria-pressed={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)}{#if counts[f.id] !== undefined} <span class="count">{counts[f.id]}</span>{/if}</button>
          {/each}
        </div>
        <label class="sort">
          <span class="muted">Sort</span>
          <select bind:value={sort} aria-label="Sort">
            <option value="newest">Newest</option>
            <option value="oldest">Oldest</option>
            <option value="name">Name</option>
            <option value="supply">Largest supply</option>
            <option value="value">Most valuable</option>
            <option value="traded">Most traded</option>
          </select>
        </label>
      </div>
    </section>

    <HiddenToggle count={hiddenHere} some={filtersOn} bind:show={showHidden} />
    {#if searchError}<p class="ba-problems bad" role="alert"><SafeText text={searchError} /></p>{/if}
    {#if !ba.loaded || (loading && list.length === 0)}
      <p class="ba-empty" role="status">Loading…</p>
    {:else if list.length === 0}
      <div class="panel ba-empty">
        {#if (ba.info?.assets ?? 0) === 0}
          No asset is registered yet. Issue the first one.
        {:else}
          No asset matches. Press Enter to look for one by its exact name, number or hash: a private name is not listed.
        {/if}
      </div>
    {:else}
      <ul class="grid" aria-label="Assets">
        {#each list as a (a.asset)}
          {@const price = chnPrice(a.asset)}
          <li>
            <button type="button" class="panel tile" onclick={() => (shown = a.asset)}>
              <span class="top">
                <AssetBadge id={a.asset} label={a.label} seq={a.seq} />
                {#if controlled.has(a.asset)}<span class="ba-tag accent" title="This wallet holds its control coin"><Icon name="crown" size={12} /></span>{/if}
              </span>
              {#if a.data?.info}<SafeText class="blurb" text={a.data.info} />{:else}<span class="blurb faint">No description.</span>{/if}
              <span class="nums">
                <span><span class="faint">Supply</span> <strong class="num">{formatAmount(a.supply, a.decimals, { max: 4 })}</strong></span>
                <span><span class="faint">Price</span> <strong class="num">{price !== undefined ? `${formatPrice(price)} CHN` : '–'}</strong></span>
              </span>
              <span class="tags">
                {#if a.fixed}<span class="ba-tag">Fixed supply</span>{/if}
                {#if mineIds.has(a.asset)}<span class="ba-tag accent">Yours</span>{/if}
                {#if a.releasable}<span class="ba-tag danger">Dead</span>{/if}
              </span>
            </button>
          </li>
        {/each}
      </ul>
      <div class="end" use:atEnd></div>
      {#if more}
        <p class="more"><button type="button" class="btn" disabled={loading} onclick={() => void fetchPage(false)}>{loading ? 'Loading…' : 'Show more'}</button></p>
      {/if}
    {/if}
    {#if fallback && (ba.info?.assets ?? 0) > PAGE}
      <p class="ba-help">{t('This node lists %1 assets at a time: type a name to find others, or a number or hash and press Enter.', formatCount(200))}</p>
    {/if}
    {#if found && !list.some((a) => a.asset === found?.asset)}
      <p class="ba-help">Found by its hash: <button type="button" class="btn small ghost" onclick={() => (shown = found?.asset ?? null)}><SafeText text={found.label} /></button> <span class="faint" translate="no">{assetArg(found.asset)}</span></p>
    {/if}
  {/if}
</div>

<style>
  .more {
    display: flex;
    justify-content: center;
    margin: 0.8rem 0 0;
  }
  .finder {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 0.9rem 1rem;
  }
  .search {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    color: var(--text-muted);
  }
  .search input {
    flex: 1 1 auto;
    min-width: 0;
    font-size: 1.05rem;
  }
  .bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
  }
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(17rem, 100%), 1fr));
    gap: 0.8rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .tile {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    width: 100%;
    height: 100%;
    padding: 0.95rem 1rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .tile:hover {
    border-color: var(--accent);
  }
  .top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    min-width: 0;
  }
  .tile :global(.blurb) {
    display: -webkit-box;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
    color: var(--text-muted);
    font-size: 0.88rem;
    min-height: 2.5em;
  }
  .nums {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    gap: 0.4rem 1rem;
    font-size: 0.88rem;
  }
  .tags {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem;
    min-height: 1.3rem;
  }
</style>

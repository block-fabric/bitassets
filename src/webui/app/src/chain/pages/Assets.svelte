<script lang="ts">
  // Assets: every asset of the chain, as cards to search (a name, a number such as 1739-0029, or a hash
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
  import { approx, assetArg, formatAmount, formatPrice, isSeq, type Asset } from '../ba/model';
  import { ba, chnPrice, poolOf, startBitAssets } from '../ba/store.svelte';

  $effect(() => startBitAssets());

  type Filter = 'all' | 'mine' | 'market' | 'mintable' | 'fixed' | 'dead';
  type Sort = 'newest' | 'oldest' | 'name' | 'supply' | 'value' | 'traded';

  let query = $state('');
  let filter = $state<Filter>('all');
  let sort = $state<Sort>('newest');
  let shown = $state<string | null>(null);
  let lookupError = $state('');
  let looking = $state(false);
  /** An asset found by its hash or number that the list does not hold (its name is private). */
  let found = $state<Asset | null>(null);
  /** Tiles drawn: more on "Show more" (a chain of thousands of assets stays quick to open). */
  const TILES = 240;
  let drawn = $state(TILES);
  /**
   * Past MAX_ASSETS the list does not hold every asset: what the user types is asked of the node too
   * (listassets with its match), and what it finds is listed with the rest (audit port-2 #4).
   */
  const complete = $derived(!ba.info || ba.assets.length >= ba.info.assets);
  let remote = $state.raw<Asset[]>([]);
  let remoteSeq = 0;
  $effect(() => {
    const q = query.trim();
    const mine = ++remoteSeq;
    if (complete || q.length < 2) {
      remote = [];
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<Asset[]>('listassets', [100, 0, q])
        .then((found) => {
          if (mine === remoteSeq) remote = found ?? [];
        })
        .catch(() => undefined);
    }, 300);
    return () => clearTimeout(timer);
  });
  const pool = $derived.by(() => {
    if (!remote.length) return ba.assets;
    const have = new Set(ba.assets.map((a) => a.asset));
    return [...ba.assets, ...remote.filter((a) => !have.has(a.asset))];
  });
  $effect(() => {
    void query;
    void filter;
    void sort;
    drawn = TILES;
  });

  // Opened for an asset (from another page, the palette, the home).
  $effect(() => {
    void nav.focus;
    const key = takeFocus('assets');
    if (key) shown = key;
  });

  const mineIds = $derived(new Set((ba.mine?.assets ?? []).map((a) => a.asset)));
  const controlled = $derived(new Set((ba.mine?.assets ?? []).filter((a) => a.control).map((a) => a.asset)));
  const withPool = $derived(new Set(ba.pools.filter((p) => !p.abandoned).flatMap((p) => [p.asset_a_id, p.asset_b_id])));

  /** What a pool of the asset traded, in CHN where it trades with CHN, for "most traded". */
  function traded(id: string): number {
    const p = poolOf(id, '0'.repeat(64));
    if (!p) return 0;
    return approx(p.asset_a_id === id ? p.volume_b : p.volume_a);
  }

  const counts = $derived({
    all: ba.assets.length,
    mine: ba.assets.filter((a) => mineIds.has(a.asset)).length,
    market: ba.assets.filter((a) => withPool.has(a.asset)).length,
    mintable: ba.assets.filter((a) => !a.fixed).length,
    fixed: ba.assets.filter((a) => a.fixed).length,
    dead: ba.assets.filter((a) => a.releasable).length,
  });

  const list = $derived.by(() => {
    const q = query.trim().toLowerCase();
    let out = pool.filter((a) => {
      if (filter === 'mine' && !mineIds.has(a.asset)) return false;
      if (filter === 'market' && !withPool.has(a.asset)) return false;
      if (filter === 'mintable' && a.fixed) return false;
      if (filter === 'fixed' && !a.fixed) return false;
      if (filter === 'dead' && !a.releasable) return false;
      if (!q) return true;
      return a.label.toLowerCase().includes(q) || a.seq === q || a.asset.startsWith(q.replace(/^0x/, '')) || (a.data?.info ?? '').toLowerCase().includes(q);
    });
    const value = (a: Asset) => (chnPrice(a.asset) ?? 0) * approx(a.supply);
    out = [...out].sort((a, b) => {
      switch (sort) {
        case 'oldest':
          return a.registered - b.registered;
        case 'name':
          return a.label.localeCompare(b.label);
        case 'supply':
          return approx(b.supply) - approx(a.supply);
        case 'value':
          return value(b) - value(a);
        case 'traded':
          return traded(b.asset) - traded(a.asset);
        default:
          return b.registered - a.registered;
      }
    });
    return out;
  });

  const FILTERS: { id: Filter; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'mine', label: 'Held or controlled' },
    { id: 'market', label: 'With a market' },
    { id: 'mintable', label: 'Mintable' },
    { id: 'fixed', label: 'Fixed supply' },
    { id: 'dead', label: 'Dead' },
  ];

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
      <h1>Explore assets</h1>
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
            <button type="button" class="ba-chip" aria-pressed={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)} <span class="count">{counts[f.id]}</span></button>
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

    {#if !ba.loaded}
      <p class="ba-empty">Loading…</p>
    {:else if list.length === 0}
      <div class="panel ba-empty">
        {#if ba.assets.length === 0}
          No asset is registered yet. Issue the first one.
        {:else}
          No asset matches. Press Enter to look for one by its exact name, number or hash: a private name is not listed.
        {/if}
      </div>
    {:else}
      <ul class="grid" aria-label="Assets">
        {#each list.slice(0, drawn) as a (a.asset)}
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
      {#if list.length > drawn}
        <p class="more"><button type="button" class="btn" onclick={() => (drawn += TILES)}>{t('Show more (%1 more)', formatCount(list.length - drawn))}</button></p>
      {/if}
    {/if}
    {#if !complete}
      <p class="ba-help">{t('The %1 oldest of the %2 assets are not listed here: type a name to find them, or its number or hash and press Enter.', formatCount(ba.info!.assets - ba.assets.length), formatCount(ba.info!.assets))}</p>
    {/if}
    {#if found && !ba.assets.some((a) => a.asset === found?.asset)}
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

<script lang="ts">
  // The pools: each pair, what it holds, its price, what it traded and how often; a pool opened: its curve,
  // its price as this app saw it, the share of this wallet, and the ways in (swap, add). The Pools table of the
  // Trade tab of the Qt window of BitAssets.
  import { selectable, stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import Chart from './Chart.svelte';
  import { approx, curvePoints, formatAmount, formatPercent, formatPrice, isChn, type Pool } from './model';
  import { compactNumber } from './axis';
  import { ba, decimalsOf, morePools, priceSeries, showMorePools, tradeIntent } from './store.svelte';

  let query = $state('');
  let showClosed = $state(false);
  let open = $state<string | null>(null);

  const list = $derived(
    ba.pools
      .filter((p) => showClosed || !p.abandoned)
      .filter((p) => {
        const q = query.trim().toLowerCase();
        return !q || p.asset_a.toLowerCase().includes(q) || p.asset_b.toLowerCase().includes(q);
      })
      .sort((x, y) => volumeChn(y) - volumeChn(x) || y.swaps - x.swaps),
  );
  const closed = $derived(ba.pools.filter((p) => p.abandoned).length);
  const pool = $derived(ba.pools.find((p) => p.pool === open) ?? null);
  const position = $derived(pool ? (ba.mine?.liquidity.find((l) => l.pool === pool.pool) ?? null) : null);
  const series = $derived(pool ? priceSeries(pool, pool.asset_a_id).map((p) => ({ x: p.h, y: p.p })) : []);

  /** What a pool traded, in CHN where one side is CHN (else in trades). */
  function volumeChn(p: Pool): number {
    if (isChn(p.asset_a_id)) return approx(p.volume_a);
    if (isChn(p.asset_b_id)) return approx(p.volume_b);
    return 0;
  }

  function swapIn(p: Pool) {
    tradeIntent.pay = p.asset_b_id;
    tradeIntent.get = p.asset_a_id;
    nav.tab = 'swap';
  }
  function addTo(p: Pool) {
    tradeIntent.a = p.asset_a_id;
    tradeIntent.b = p.asset_b_id;
    nav.tab = 'liquidity';
  }
</script>

{#if pool}
  <div class="pool-view">
    <button type="button" class="btn ghost small ba-back" onclick={() => (open = null)}><Icon name="back" size={15} /> All pools</button>
    <header class="panel head">
      <span class="pair"><AssetBadge id={pool.asset_a_id} label={pool.asset_a} /> <span class="faint">/</span> <AssetBadge id={pool.asset_b_id} label={pool.asset_b} /></span>
      {#if pool.abandoned}<span class="ba-tag danger">Closed: no liquidity</span>{/if}
      <span class="grow"></span>
      <button type="button" class="btn primary" disabled={pool.abandoned} onclick={() => swapIn(pool)}><Icon name="swap" size={15} /> Swap</button>
      <button type="button" class="btn" onclick={() => addTo(pool)}><Icon name="plus" size={15} /> {pool.abandoned ? 'Reopen it' : 'Add liquidity'}</button>
    </header>
    <section class="ba-figures">
      <div class="panel ba-figure"><span class="muted">Price</span><strong>{formatPrice(pool.price)}</strong><span class="faint"><SafeText text={pool.asset_b} /> for 1 <SafeText text={pool.asset_a} /></span></div>
      <div class="panel ba-figure"><span class="muted">Holds</span><strong>{formatAmount(pool.reserve_a, decimalsOf(pool.asset_a_id), { max: 4 })}</strong><span class="faint"><SafeText text={pool.asset_a} /> and {formatAmount(pool.reserve_b, decimalsOf(pool.asset_b_id), { max: 4 })} <SafeText text={pool.asset_b} /></span></div>
      <div class="panel ba-figure"><span class="muted">Traded</span><strong>{formatCount(pool.swaps)}</strong><span class="faint">trades since block {formatCount(pool.created)}</span></div>
      <div class="panel ba-figure"><span class="muted">Your share</span><strong>{position ? formatPercent(position.percent) : '–'}</strong><span class="faint">{position ? t('%1 shares', formatAmount(position.shares, 0)) : t('of %1 shares', formatAmount(pool.shares, 0))}</span></div>
    </section>
    <div class="ba-cols">
      <section class="panel">
        <div class="panel-head"><h3>Price</h3><span class="muted small">as this app saw it</span></div>
        <div class="panel-body">
          <Chart points={series.length ? series : [{ x: ba.height, y: pool.price ?? 0 }]} step label={t('The price of %1 in %2', pool.asset_a, pool.asset_b)} formatX={(x) => t('block %1', String(Math.round(x)))} />
        </div>
      </section>
      <section class="panel">
        <div class="panel-head"><h3>The curve</h3></div>
        <div class="panel-body">
          <Chart points={curvePoints(approx(pool.reserve_a), approx(pool.reserve_b))} marks={[{ x: approx(pool.reserve_a), y: approx(pool.reserve_b) }]} logX yZero area={false} label={t('The curve of the pool: what it holds of each asset')} formatX={(x) => compactNumber(x, 4)} formatY={(y) => formatPrice(y, 4)} />
          <p class="ba-help">Volume: {formatAmount(pool.volume_a, decimalsOf(pool.asset_a_id))} <SafeText text={pool.asset_a} /> and {formatAmount(pool.volume_b, decimalsOf(pool.asset_b_id))} <SafeText text={pool.asset_b} /> traded in.</p>
        </div>
      </section>
    </div>
  </div>
{:else}
  <section class="panel">
    <div class="panel-head">
      <input type="search" class="search" placeholder="Find a pool by an asset" aria-label="Find a pool" bind:value={query} />
      {#if closed}<label class="check"><input type="checkbox" bind:checked={showClosed} /> <span>Closed pools ({closed})</span></label>{/if}
    </div>
    {#if list.length === 0}
      <p class="ba-empty">{ba.pools.length === 0 ? 'No pools yet: add liquidity to make the first.' : 'No pool matches.'}</p>
    {:else}
      <div class="ba-wrap">
        <table class="ba-table" use:stacked aria-label="Pools">
          <thead><tr><th>Pool</th><th class="end">Price</th><th class="end">Holds</th><th class="end">Traded in</th><th class="end">Trades</th></tr></thead>
          <tbody>
            {#each list as p (p.pool)}
              <tr class="pick" onclick={() => (open = p.pool)} use:selectable={() => (open = p.pool)}>
                <td><span class="pair"><AssetBadge id={p.asset_a_id} label={p.asset_a} size="s" /> <span class="faint">/</span> <AssetBadge id={p.asset_b_id} label={p.asset_b} size="s" /></span>{#if p.abandoned} <span class="ba-tag danger">closed</span>{/if}</td>
                <td class="end">{formatPrice(p.price)}</td>
                <td class="end">{formatAmount(p.reserve_a, decimalsOf(p.asset_a_id), { max: 4 })} · {formatAmount(p.reserve_b, decimalsOf(p.asset_b_id), { max: 4 })}</td>
                <td class="end">{formatAmount(p.volume_a, decimalsOf(p.asset_a_id), { max: 4 })} · {formatAmount(p.volume_b, decimalsOf(p.asset_b_id), { max: 4 })}</td>
                <td class="end">{formatCount(p.swaps)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
      {#if morePools()}
      <p class="ba-help more-pools">{t('Showing %1 of %2 pools.', formatCount(ba.pools.length), formatCount(ba.info?.pools ?? 0))} <button type="button" class="btn small" onclick={showMorePools}>Show more</button></p>
    {/if}
  </section>
{/if}

<style>
  .pool-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.7rem;
    padding: 1rem 1.1rem;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .search {
    flex: 1 1 14rem;
    max-width: 24rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>

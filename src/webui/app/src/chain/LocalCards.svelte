<script lang="ts">
  // The cards of BitAssets on the home of the dashboard: My assets (ba/HeldAssets.svelte: the table of what the wallet
  // holds, one card since user feedback 2026-10-19, no totals), the markets (the pools that trade most, with their
  // prices), and the auctions running (the ones that end soonest, at their price now). Each opens its page.
  import SafeText from '../components/SafeText.svelte';
  import Icon from '../components/Icon.svelte';
  import Card from '../views/home/Card.svelte';
  import { formatCount } from '../lib/format';
  import { go, goTo } from '../lib/nav.svelte';
  import { wallets } from '../lib/state.svelte';
  import './ba/ba.css';
  import AssetBadge from './ba/AssetBadge.svelte';
  import HeldAssets from './ba/HeldAssets.svelte';
  import { formatAmount, formatPrice, isChn, unitPrice } from './ba/model';
  import { ba, decimalsOf, startBitAssets } from './ba/store.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  $effect(() => startBitAssets());
  // Simple mode (lib/uimode.svelte.ts): no auctions card (Advanced's), the actions in plain words.
  const simple = $derived(simpleMode());

  const markets = $derived(
    ba.pools
      .filter((p) => !p.abandoned)
      .slice()
      .sort((a, b) => b.swaps - a.swaps)
      .slice(0, 3),
  );
  const ending = $derived(
    ba.auctions
      .filter((a) => a.status === 'open')
      .slice()
      .sort((a, b) => a.end_height - b.end_height)
      .slice(0, 3),
  );
</script>

{#if ba.loaded && !ba.error}
  {#if wallets.current !== null}<div class="ba-held"><HeldAssets home /></div>{/if}
  <div class="ba-cards">
    <Card title="Markets" icon="swap" open={() => go('trade', simple ? 'swap' : 'pools')} openLabel={simple ? 'Open Swap' : 'Open the pools'} stat>
      <span class="value num">{formatCount(ba.pools.filter((p) => !p.abandoned).length)}</span>
      <dl class="pairs">
        <dt>{simple ? 'Tokens to swap' : 'Pools with liquidity'}</dt><dd class="num">{ba.pools.filter((p) => !p.abandoned).length}</dd>
        <dt>Trades, all told</dt><dd class="num">{formatCount(ba.pools.reduce((n, p) => n + p.swaps, 0))}</dd>
      </dl>
      {#if markets.length === 0}
        <p class="muted">No pool trades yet.</p>
      {:else}
        <ul class="mini">
          {#each markets as p (p.pool)}
            {@const asset = isChn(p.asset_a_id) ? p.asset_b_id : p.asset_a_id}
            <li>
              <button type="button" class="link" onclick={() => goTo('assets', '', asset)}><AssetBadge id={asset} label={isChn(p.asset_a_id) ? p.asset_b : p.asset_a} size="s" /></button>
              <span class="num small">{formatPrice(isChn(p.asset_a_id) ? (p.price ? 1 / p.price : undefined) : p.price)} <SafeText text={isChn(p.asset_a_id) ? p.asset_a : p.asset_b} /></span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('trade', 'swap')}><Icon name="swap" size={15} /> Swap</button>
        <button type="button" class="btn ghost" onclick={() => go('assets')}>Browse assets</button>
      {/snippet}
    </Card>

    {#if !simple}
    <Card title="Auctions" icon="gavel" open={() => go('auctions')} openLabel="Open the auctions" stat>
      <span class="value num">{formatCount(ba.auctions.filter((a) => a.status === 'open').length)}</span>
      <dl class="pairs">
        <dt>Taking bids</dt><dd class="num">{ba.auctions.filter((a) => a.status === 'open').length}</dd>
        <dt>Upcoming</dt><dd class="num">{ba.auctions.filter((a) => a.status === 'upcoming').length}</dd>
      </dl>
      {#if ending.length === 0}
        <p class="muted">No auction is taking bids.</p>
      {:else}
        <ul class="mini">
          {#each ending as a (a.auction)}
            {@const up = unitPrice(a.unit_price)}
            <li>
              <button type="button" class="link" onclick={() => goTo('auctions', '', a.auction)}>{formatAmount(a.remaining, decimalsOf(a.base_id), { max: 2 })} <SafeText text={a.base} /></button>
              <span class="num small">{#if up.kind === 'each'}{up.price}{:else if up.kind === 'buys'}1/{up.amount}{:else}–{/if} <SafeText text={a.quote} /></span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('auctions', 'sell')}><Icon name="gavel" size={15} /> Sell</button>
      {/snippet}
    </Card>
    {/if}
  </div>
{/if}

<style>
  .ba-held {
    margin-bottom: 1rem;
  }
  .ba-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(16rem, 100%), 1fr));
    gap: 1rem;
  }
  .value {
    display: block;
    margin: 0.1rem 0 0.55rem;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: -0.02em;
  }
  .pairs {
    display: grid;
    /* The labels keep their longest word; the values wrap when the card is narrow (as the kit's cards). */
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.35rem 1rem;
    margin: 0;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .pairs dt {
    color: var(--text-muted);
    font-size: 0.86rem;
    overflow-wrap: break-word;
  }
  .pairs dd {
    min-width: 0;
    overflow-wrap: anywhere;
    margin: 0;
    text-align: right;
    font-weight: 600;
    font-size: 0.92rem;
  }
  .mini {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    margin: 0.7rem 0 0;
    padding: 0;
    list-style: none;
  }
  .mini li {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    min-width: 0;
  }
  .link {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
    padding: 0;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .link:hover {
    color: var(--accent-fg);
  }
  .small {
    font-size: 0.85rem;
    white-space: nowrap;
  }
</style>

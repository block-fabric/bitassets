<script lang="ts">
  // The asset a transaction of the wallet moved, in its details (components/TxDetailsView.svelte), above the CHN
  // details (user feedback 2026-10-19): per asset, its logo, label and number, the amount exact in its decimals, the
  // direction, the other side (each address once, the wallet's own as "this wallet"), and what the operation did in
  // the node's words (listassetactivity: the bid and the auction's prices, what went into a pool...), with the facts
  // of the asset for a registration (supply, decimals) and its logo for a change of logo.
  import SafeText from '../components/SafeText.svelte';
  import { rpc } from '../lib/rpc';
  import { t } from '../lib/i18n.svelte';
  import AssetBadge from './ba/AssetBadge.svelte';
  import { formatAmount, type Activity } from './ba/model';
  import { ba } from './ba/store.svelte';
  import { assetMovesOf, categoryText, counterparty, signedAmount } from './txinfo.svelte';

  let { wallet, txid }: { wallet: string; txid: string } = $props();

  const moves = $derived(assetMovesOf(txid));
  let fetched: Activity | null = $state(null);
  const activity = $derived(ba.activity.find((a) => a.txid === txid) ?? fetched);

  // Not among the latest the store read: asked once for this transaction.
  $effect(() => {
    const id = txid;
    if (!moves.length || ba.activity.some((a) => a.txid === id)) return;
    fetched = null;
    rpc
      .raw<Activity[]>('listassetactivity', [2000], { wallet })
      .then((list) => {
        if (id === txid) fetched = (list ?? []).find((a) => a.txid === id) ?? null;
      })
      .catch(() => undefined);
  });

  function direction(sign: number): string {
    return sign > 0 ? t('Into this wallet') : sign < 0 ? t('Out of this wallet') : t('Neither in nor out');
  }
</script>

{#if moves.length}
  <section class="asset-move" aria-label="Asset movement" data-testid="asset-movement">
    <h3>Asset movement</h3>
    {#if activity?.summary}<p class="what" translate="no"><SafeText text={activity.summary} /></p>{/if}
    {#each moves as e, i (i)}
      {@const side = counterparty(e)}
      {@const asset = e.asset ? ba.assets.find((a) => a.asset === e.asset) : undefined}
      <dl class="facts">
        <dt>Operation</dt>
        <!-- Already in the language shown (categoryText translates it). -->
        <dd><span translate="no">{categoryText(e)}</span></dd>
        {#if e.asset}
          <dt>Asset</dt>
          <dd class="asset">
            <AssetBadge id={e.asset} label={e.label ?? ''} seq={e.seq ?? ''} size={e.category === 'logo' ? 'l' : 'm'} />
          </dd>
        {/if}
        {#if e.amount !== undefined}
          <dt>Amount</dt>
          <dd><span class="amt" class:neg={e.sign < 0} class:pos={e.sign > 0} translate="no">{signedAmount(e)}</span> {#if e.label}<SafeText text={e.label} />{/if}</dd>
        {/if}
        <dt>Direction</dt>
        <dd>{direction(e.sign)}</dd>
        {#if side.dir === 'to'}
          <dt>To</dt>
        {:else if side.dir === 'from'}
          <dt>From</dt>
        {/if}
        {#if side.dir}
          <dd class="parties">
            {#each side.parties as p (p.address)}<span class="mono break" translate="no">{#if p.address === 'unknown'}unknown{:else}<SafeText text={p.address} />{/if}</span>{/each}
            {#if side.self}<span class="ba-tag">this wallet</span>{/if}
          </dd>
        {/if}
        {#if e.category === 'register' && asset}
          <dt>Supply</dt>
          <dd translate="no">{formatAmount(asset.supply, asset.decimals, { trim: false })}</dd>
          <dt>Decimals</dt>
          <dd translate="no">{asset.decimals}</dd>
        {/if}
        {#if e.fee !== undefined}
          <dt>Network fee</dt>
          <dd translate="no">{e.fee} CHN</dd>
        {/if}
      </dl>
    {/each}
  </section>
{/if}

<style>
  .asset-move {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    padding-bottom: 0.7rem;
    border-bottom: 1px solid var(--line);
  }
  h3 {
    margin: 0;
    font-size: 0.95rem;
  }
  .what {
    margin: 0;
  }
  dd {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .amt {
    font-variant-numeric: tabular-nums;
    font-weight: 600;
  }
  .amt.neg {
    color: var(--negative);
  }
  .amt.pos {
    color: var(--positive);
  }
  .parties {
    flex-direction: column;
    align-items: flex-start;
  }
</style>

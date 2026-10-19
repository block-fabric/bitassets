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
  import { go } from '../lib/nav.svelte';
  import { simpleMode } from '../lib/uimode.svelte';
  import { assetTitle, formatAmount, type Activity, type AssetTx } from './ba/model';
  import { ba, displayNameOf } from './ba/store.svelte';
  import { assetMovesOf, categoryText, counterparty, middleAddress, ownAddresses, signedAmount } from './txinfo.svelte';

  const simple = $derived(simpleMode());

  // The labels of the wallet's own addresses listed (getaddressinfo, once each).
  let labels = $state<Record<string, string>>({});
  $effect(() => {
    const w = wallet;
    for (const e of moves) {
      for (const { address } of ownAddresses(e)) {
        if (address in labels) continue;
        labels[address] = '';
        rpc
          .raw<{ labels?: string[]; label?: string }>('getaddressinfo', [address], { wallet: w })
          .then((info) => {
            const label = info?.labels?.[0] ?? info?.label ?? '';
            if (label) labels[address] = label;
          })
          .catch(() => undefined);
      }
    }
  });

  /** The line that says what happened, in the app's words where they read better than the node's. */
  function headline(e: AssetTx): string {
    const name = assetTitle(e.label ?? '', e.display_name ?? (e.asset ? displayNameOf(e.asset) : undefined));
    const amount = e.amount !== undefined ? formatAmount(e.amount, e.decimals ?? 0, { trim: false }) : '';
    switch (e.category) {
      case 'register':
        return amount ? t('Registered %1 · %2 created', name, amount) : t('Registered %1', name);
      case 'mint':
        return t('Minted %1 %2', amount, name);
      case 'burn':
        return t('Burned %1 %2', amount, name);
      case 'display_name':
        return t('Display name of %1 set', e.label ?? '');
      default:
        return '';
    }
  }

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
    {#if moves[0] && headline(moves[0])}<p class="what" data-testid="asset-headline"><SafeText text={headline(moves[0])} /></p>
    {:else if activity?.summary}<p class="what" translate="no"><SafeText text={activity.summary} /></p>{/if}
    {#each moves as e, i (i)}
      {@const side = counterparty(e)}
      {@const asset = e.asset ? ba.assets.find((a) => a.asset === e.asset) : undefined}
      {@const own = ownAddresses(e)}
      <dl class="facts">
        {#if !simple}
        <dt>Operation</dt>
        <!-- Already in the language shown (categoryText translates it). -->
        <dd><span translate="no">{categoryText(e)}</span></dd>
        {/if}
        {#if e.asset}
          <dt>Asset</dt>
          <dd class="asset">
            <AssetBadge id={e.asset} label={e.label ?? ''} display={e.display_name} seq={e.seq ?? ''} size={e.category === 'logo' ? 'l' : 'm'} />
          </dd>
        {/if}
        {#if e.amount !== undefined}
          <dt>Amount</dt>
          <dd><span class="amt" class:neg={e.sign < 0} class:pos={e.sign > 0} translate="no">{signedAmount(e)}</span> {#if e.label}<SafeText text={e.label} />{/if}</dd>
        {/if}
        {#if !simple}
        <dt>Direction</dt>
        <dd>{direction(e.sign)}</dd>
        {/if}
        {#if side.parties.length}
          <dt>{side.dir === 'from' ? 'From' : 'To'}</dt>
          <dd class="parties">
            {#each side.parties as p (p.address)}<span class="mono break" translate="no">{#if p.address === 'unknown'}unknown{:else}<SafeText text={p.address} />{/if}</span>{/each}
          </dd>
        {/if}
        {#if own.length}
          <!-- The wallet's own addresses of the move, each with what it got (the asset, the control coin, the change of CHN). -->
          <dt>This wallet</dt>
          <dd class="parties own" data-testid="own-addresses">
            {#each own as o (o.address)}
              <span class="own-row">
                <button type="button" class="linkish mono" title={o.address} onclick={() => go('explorer', `search:${o.address}`)}><SafeText text={simple ? middleAddress(o.address, 8) : o.address} /></button>
                {#each o.roles as role (role)}<span class="ba-tag">{role}</span>{/each}
                {#if labels[o.address]}<SafeText class="muted" text={labels[o.address]} />{/if}
              </span>
            {/each}
          </dd>
        {:else if side.self}
          <dt>{side.dir === 'from' ? 'From' : 'To'}</dt>
          <dd><span class="ba-tag">this wallet</span></dd>
        {/if}
        {#if e.category === 'register' && asset && !simple}
          <dt>Supply</dt>
          <dd translate="no">{formatAmount(asset.supply, asset.decimals, { trim: false })}</dd>
          <dt>Decimals</dt>
          <dd translate="no">{asset.decimals}</dd>
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
  .own-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.35rem;
  }
  .linkish {
    background: none;
    border: none;
    padding: 0;
    color: var(--accent);
    cursor: pointer;
    font-size: 0.85rem;
    overflow-wrap: anywhere;
    text-align: start;
  }
  .linkish:hover,
  .linkish:focus-visible {
    text-decoration: underline;
  }
</style>

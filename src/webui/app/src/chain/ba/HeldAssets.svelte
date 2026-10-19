<script lang="ts">
  // "Your assets": what the wallet holds beside CHN, on the Home and the Overview. Each asset by its logo (else its
  // initials), its label and its number, its balance in its decimals (exact: the store reads amounts exactly), its worth
  // in CHN where a pool prices it, units on the way marked; the first HELD_SHOWN by worth, then balance, "View all" for
  // My assets; the assets only controlled below. From the store's one listmyassets, asked again with every block and
  // transaction of the wallet. Simple mode: a compact list.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { go, goTo } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import './ba.css';
  import AssetBadge from './AssetBadge.svelte';
  import { HELD_SHOWN, heldRows } from './held';
  import { assetTitle, formatAmount, formatPrice } from './model';
  import { assetById, ba, chnPrice, displayNameOf, startBitAssets, unlockHoldings } from './store.svelte';
  import HiddenToggle from './HiddenToggle.svelte';
  import { withoutHidden } from './visible.svelte';

  let { compact = false }: { compact?: boolean } = $props();
  $effect(() => startBitAssets());

  const simple = $derived(simpleMode() || compact);
  // What the wallet hides (its asset filters: ba/hide.svelte.ts) is left out, unless shown.
  let showHidden = $state(false);
  const visible = $derived(withoutHidden(ba.wallet === wallets.current ? (ba.mine?.assets ?? []) : [], (h) => h.asset, (h) => h.label, showHidden));
  const holdings = $derived(visible.items);
  const held = $derived(heldRows(holdings, chnPrice, (id) => assetById(id)?.seq ?? ''));
  const shown = $derived(held.rows.slice(0, HELD_SHOWN));
</script>

<section class="panel held" aria-labelledby="held-title" data-testid="held-assets">
  <div class="head">
    <h2 id="held-title">Your assets</h2>
    {#if held.rows.length > 0}<button type="button" class="btn ghost small" onclick={() => go('my-assets')}>View all <Icon name="arrow" size={14} /></button>{/if}
  </div>
  <HiddenToggle count={visible.hidden} bind:show={showHidden} />
  {#if wallets.current === null}
    <p class="muted">No wallet is loaded.</p>
  {:else if ba.mineState === 'locked'}
    <p class="muted">The wallet is locked: its assets show once it is unlocked. <button type="button" class="btn small" onclick={() => void unlockHoldings()}>Unlock</button></p>
  {:else if !ba.mine && (ba.mineState === 'loading' || ba.mineState === 'none')}
    <p class="muted" role="status">Loading…</p>
  {:else if ba.mineState === 'error' && !ba.mine}
    <p class="muted">The assets of the wallet could not be read: <SafeText text={ba.mineError} /></p>
  {:else if held.rows.length === 0 && held.controlledOnly.length === 0}
    <p class="muted empty">No assets yet. <button type="button" class="btn small" onclick={() => go('issue')}>Register new asset</button> <button type="button" class="btn ghost small" onclick={() => go('trade', 'swap')}>Get assets in Trade</button></p>
  {:else}
    <!-- One compact row per asset (user feedback 2026-10-19): its logo, the crown if the wallet controls it, its ticker
         with its number, its display name, its balance and its worth; narrow, the name goes first, then the worth. -->
    <table class="rows" aria-labelledby="held-title">
      <thead class="sr-only"><tr><th scope="col">Asset</th><th scope="col" class="c-name">Name</th><th scope="col">Balance</th><th scope="col" class="c-value">Value (CHN)</th></tr></thead>
      <tbody>
        {#each shown as r (r.holding.asset)}
          {@const display = displayNameOf(r.holding.asset) ?? ''}
          <tr class="row" data-testid="held-row" onclick={() => goTo('assets', '', r.holding.asset)}>
            <td class="c-asset">
              <button type="button" class="link asset" title={assetTitle(r.holding.label, display, r.seq)} onclick={(e) => { e.stopPropagation(); goTo('assets', '', r.holding.asset); }}>
                <AssetBadge id={r.holding.asset} label={r.holding.label} size="s" showLabel={false} />
                {#if r.holding.control}<span class="crown" title="You control this asset"><Icon name="crown" size={12} label="You control this asset" /></span>{/if}
                <SafeText class="ticker" text={r.holding.label} />
                {#if r.seq}<span class="seq faint" translate="no">#{r.seq}</span>{/if}
              </button>
            </td>
            <td class="c-name muted" title={display}>{#if display}<SafeText text={display} />{/if}</td>
            <td class="c-balance num" translate="no">{formatAmount(r.holding.balance, r.holding.decimals, { trim: false })}{#if r.pending} <span class="ba-warn on-way" title="Spendable after the next block">+{formatAmount(r.holding.pending, r.holding.decimals, { trim: false })}</span>{/if}</td>
            <td class="c-value num" translate="no">{r.value !== undefined ? formatPrice(r.value) : '–'}</td>
          </tr>
        {/each}
      </tbody>
    </table>
    {#if held.rows.length > HELD_SHOWN}<p class="faint small">And {held.rows.length - HELD_SHOWN} more in My assets.</p>{/if}
    {#if held.controlledOnly.length}
      <p class="small controlled" data-testid="controlled-only"><span class="muted">You control, without holding any:</span>
        {#each held.controlledOnly as r (r.holding.asset)}
          <button type="button" class="chip" title={assetTitle(r.holding.label, displayNameOf(r.holding.asset), r.seq)} onclick={() => goTo('assets', '', r.holding.asset)}>
            <AssetBadge id={r.holding.asset} label={r.holding.label} size="s" showLabel={false} />
            <span class="crown" title="You control this asset"><Icon name="crown" size={11} label="You control this asset" /></span>
            <SafeText class="ticker" text={r.holding.label} />
          </button>
        {/each}
      </p>
    {/if}
  {/if}
</section>

<style>
  .held {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem 1.1rem;
    min-width: 0;
  }
  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .head h2 {
    margin: 0;
    font-size: 1.05rem;
  }
  .held {
    container: held / inline-size;
  }
  .rows {
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
  }
  .row {
    height: 2.25rem;
    cursor: pointer;
    border-top: 1px solid var(--line);
  }
  .row:first-child {
    border-top: none;
  }
  .row:hover {
    background: var(--surface-2);
  }
  .row td {
    padding: 0 0.35rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    vertical-align: middle;
  }
  .c-asset {
    width: 42%;
  }
  .c-name {
    width: 26%;
  }
  .c-balance {
    width: 20%;
  }
  .c-value {
    width: 12%;
  }
  .asset {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    max-width: 100%;
    min-width: 0;
    color: inherit;
    text-decoration: none;
  }
  .asset :global(.ticker),
  .chip :global(.ticker) {
    font-weight: 700;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }
  .seq {
    font-size: 0.75rem;
    flex: none;
  }
  .crown {
    display: inline-flex;
    color: var(--warning, var(--accent));
    flex: none;
  }
  .on-way {
    font-size: 0.75rem;
  }
  .chip {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    margin: 0.15rem 0.3rem 0 0;
    padding: 0.1rem 0.5rem 0.1rem 0.15rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
    color: inherit;
    font: inherit;
    cursor: pointer;
    max-width: 14rem;
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
    white-space: nowrap;
  }
  /* Narrow: the display name goes first, then the worth. */
  @container held (max-width: 30rem) {
    .c-name {
      display: none;
    }
    .c-asset {
      width: 58%;
    }
  }
  @container held (max-width: 21rem) {
    .c-value {
      display: none;
    }
    .c-asset {
      width: 62%;
    }
    .c-balance {
      width: 38%;
    }
  }
  .num {
    text-align: end;
    font-variant-numeric: tabular-nums;
  }
  .small {
    font-size: 0.85rem;
  }
  .controlled {
    margin: 0;
  }
  .empty {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
</style>

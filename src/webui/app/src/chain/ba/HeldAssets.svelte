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
  import { formatAmount, formatPrice } from './model';
  import { assetById, ba, chnPrice, startBitAssets, unlockHoldings } from './store.svelte';
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
    {#if simple}
      <ul class="compact">
        {#each shown as r (r.holding.asset)}
          <li>
            <button type="button" class="link" onclick={() => goTo('assets', '', r.holding.asset)}><AssetBadge id={r.holding.asset} label={r.holding.label} size="s" /></button>
            <span class="num">{formatAmount(r.holding.balance, r.holding.decimals)}{#if r.pending} <span class="ba-warn small">+{formatAmount(r.holding.pending, r.holding.decimals)}</span>{/if}</span>
          </li>
        {/each}
      </ul>
    {:else}
      <table class="ba-table">
        <thead><tr><th scope="col">Asset</th><th scope="col" class="num">Balance</th><th scope="col" class="num">Value (CHN)</th></tr></thead>
        <tbody>
          {#each shown as r (r.holding.asset)}
            <tr class="clickable" onclick={() => goTo('assets', '', r.holding.asset)}>
              <td><button type="button" class="link" onclick={(e) => { e.stopPropagation(); goTo('assets', '', r.holding.asset); }}><AssetBadge id={r.holding.asset} label={r.holding.label} seq={r.seq} size="s" /></button>{#if r.holding.control} <Icon name="crown" size={12} label="You control it" />{/if}</td>
              <td class="num">{formatAmount(r.holding.balance, r.holding.decimals)}{#if r.pending}<br /><span class="ba-warn small" title="Spendable after the next block">+{formatAmount(r.holding.pending, r.holding.decimals)} on the way</span>{/if}</td>
              <td class="num">{r.value !== undefined ? formatPrice(r.value) : '–'}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
    {#if held.rows.length > HELD_SHOWN}<p class="faint small">And {held.rows.length - HELD_SHOWN} more in My assets.</p>{/if}
    {#if held.controlledOnly.length}
      <p class="small controlled"><span class="muted">You control, without holding any:</span>
        {#each held.controlledOnly as r, i (r.holding.asset)}{#if i > 0}, {/if}<button type="button" class="link" onclick={() => goTo('assets', '', r.holding.asset)}><SafeText text={r.holding.label} />{#if r.seq} <span class="faint" translate="no">#{r.seq}</span>{/if}</button>{/each}
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
  .compact {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
  }
  .compact li {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.6rem;
  }
  .clickable {
    cursor: pointer;
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

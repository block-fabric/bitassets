<script lang="ts">
  // The Asset column of the Transactions page: each asset the transaction moved (its logo, label and number), the amount
  // with its sign, and the other side: "to" the addresses of a send, "from" those that paid what came in ("yours" marks
  // the wallet's own), with the transaction in the explorer. The asset keeps its name (up to a width); the addresses
  // give way first, shortened in the middle in the table (user feedback 2026-10-19); the wallet's own outputs are
  // "this wallet", once, with the first of its addresses (the others in the tooltip and the details): a click opens it in
  // the explorer (the wallet's view of it: its label, balance and transactions).
  import SafeText from '../components/SafeText.svelte';
  import { go } from '../lib/nav.svelte';
  import AssetBadge from './ba/AssetBadge.svelte';
  import { assetMovesOf, counterparty, middleAddress, shortAddress, signedAmount } from './txinfo.svelte';

  function openAddress(ev: Event, address: string) {
    ev.stopPropagation();
    go('explorer', `search:${address}`);
  }

  // In the table: one line, cut to its column (the row's menu has the explorer); in the activity: whole, with the link.
  let { txid, full = false }: { txid: string; full?: boolean } = $props();
  const moves = $derived(assetMovesOf(txid));
</script>

{#if moves.length}
  <span class="moves" class:full data-testid="asset-moves">
    {#each moves as e, i (i)}
      {@const side = counterparty(e)}
      <span class="move">
        {#if e.asset}<span class="badge" title={e.seq ? `${e.label ?? ''} #${e.seq}` : (e.label ?? '')}><AssetBadge id={e.asset} label={e.label ?? ''} seq={full ? (e.seq ?? '') : ''} size="s" /></span>{/if}
        {#if e.amount !== undefined}<span class="amt" class:neg={e.sign < 0} class:pos={e.sign > 0} translate="no">{signedAmount(e)}</span>{/if}
        {#if side.dir && (side.parties.length || side.self)}
          <span class="party faint" title={side.parties.map((p) => p.address).join(' ')}>{side.dir === 'to' ? 'to' : 'from'}
            {#each side.parties.slice(0, 3) as p (p.address)}<span class="addr" translate="no">{#if p.address === 'unknown'}unknown{:else}<SafeText text={full ? p.address : shortAddress(p.address)} />{/if}</span> {/each}{#if side.parties.length > 3}+{side.parties.length - 3}{/if}
            {#if side.self && side.parties.length === 0}
              {@const first = side.own[0]?.address ?? ''}
              {#if first && first !== 'unknown'}
                <button type="button" class="ba-tag self" data-testid="this-wallet" title={side.own.map((p) => p.address).join('\n')} onclick={(ev) => openAddress(ev, first)} onkeydown={(ev) => ev.stopPropagation()}>this wallet · <span class="addr" translate="no"><SafeText text={middleAddress(first)} /></span>{#if side.own.length > 1} +{side.own.length - 1}{/if}</button>
              {:else}<span class="ba-tag">this wallet</span>{/if}
            {/if}
          </span>
        {/if}
      </span>
    {/each}
    {#if full}<button type="button" class="link small" onclick={(ev) => { ev.stopPropagation(); go('explorer', `search:${txid}`); }}>Explorer</button>{/if}
  </span>
{/if}

<style>
  .moves {
    display: flex;
    gap: 0.6rem;
    min-width: 0;
    max-width: 100%;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .moves.full {
    flex-direction: column;
    gap: 0.15rem;
    white-space: normal;
  }
  .move {
    display: inline-flex;
    flex: 1 1 auto;
    align-items: center;
    gap: 0.35rem;
    min-width: 0;
  }
  .full .move {
    flex-wrap: wrap;
  }
  /* The asset before the addresses: its name shown up to a width, the addresses shrink first. */
  .badge {
    flex: 0 1 auto;
    min-width: 4.5rem;
    max-width: 11rem;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .full .badge {
    max-width: none;
  }
  .amt {
    flex: none;
  }
  .amt {
    font-variant-numeric: tabular-nums;
  }
  .party {
    flex: 1 1 0;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .full .party {
    overflow: visible;
  }
  .amt.neg {
    color: var(--negative);
  }
  .amt.pos {
    color: var(--positive);
  }
  .addr {
    font-family: var(--font-mono);
    font-size: 0.8rem;
  }
  .full .addr {
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.8rem;
    align-self: flex-start;
  }
  .self {
    cursor: pointer;
    font: inherit;
    font-size: 0.8rem;
  }
  .self:hover,
  .self:focus-visible {
    border-color: var(--accent);
  }
</style>

<script lang="ts">
  // Settings → Asset filters (user feedback 2026-10-19): what the wallet shown hides of the assets, everywhere in its
  // lists (ba/hide.svelte.ts): filters, words and names to ignore, and the assets it ignores, to review and unignore.
  // Display only: the coins stay in the wallet. CHN is never hidden.
  import SafeText from '../components/SafeText.svelte';
  import { wallets } from '../lib/state.svelte';
  import { walletDisplayName } from '../lib/wallet';
  import './ba/ba.css';
  import AssetBadge from './ba/AssetBadge.svelte';
  import { DEFAULT_DEAD_AFTER, MAX_DEAD_AFTER, filtersOf, ignoreName, loadHiding, setFilters, unignoreAsset, unignoreName } from './ba/hide.svelte';
  import { labelOf, nameOf, startBitAssets } from './ba/store.svelte';

  let { tab }: { tab: string } = $props();

  $effect(() => {
    void loadHiding();
    startBitAssets();
  });

  const f = $derived(filtersOf());
  let word = $state('');
  let name = $state('');

  function addWord() {
    const w = word.trim();
    if (!w) return;
    if (!f.keywords.some((k) => k.toLowerCase() === w.toLowerCase())) setFilters({ keywords: [...f.keywords, w] });
    word = '';
  }
  /** The blocks after which an asset is dead: a whole number, or empty for the node's volume window. */
  function setDeadAfter(raw: string) {
    const n = Number(raw.trim());
    setFilters({ deadAfter: raw.trim() && Number.isInteger(n) && n >= 1 && n <= MAX_DEAD_AFTER ? n : 0 });
  }
  function addName() {
    if (name.trim()) ignoreName(name);
    name = '';
  }
</script>

<section class="panel asset-filters" data-tab={tab} aria-label="Asset filters">
  {#if wallets.current === null}
    <p class="muted">Open a wallet: the asset filters are its own.</p>
  {:else}
    <p class="ba-help">What wallet <SafeText text={walletDisplayName(wallets.current)} /> hides of the assets: in Browse assets, Send, Trade, Auctions, My assets, the home, the transactions and the notices. Each list can show what it hides. The coins stay in the wallet; CHN is never hidden.</p>

    <h3>Filters</h3>
    <label class="check"><input type="checkbox" checked={f.hideDead} onchange={(e) => setFilters({ hideDead: e.currentTarget.checked })} /> Hide dead assets (nothing done with them for a while: no move, trade, mint or burn)</label>
    <label class="row dead-after">
      <span>Dead after</span>
      <input type="number" aria-label="Dead after this many blocks" min="1" max={MAX_DEAD_AFTER} step="1" placeholder={String(DEFAULT_DEAD_AFTER)} value={f.deadAfter > 0 ? f.deadAfter : ''} disabled={!f.hideDead} onchange={(e) => setDeadAfter(e.currentTarget.value)} />
      <span>blocks without activity{f.deadAfter > 0 ? '' : ' (empty: about a day, the node\'s volume window)'}</span>
    </label>
    <label class="check"><input type="checkbox" checked={f.hideBurned} onchange={(e) => setFilters({ hideBurned: e.currentTarget.checked })} /> Hide assets burned to nothing</label>
    <label class="check"><input type="checkbox" checked={f.hideNoPool} onchange={(e) => setFilters({ hideNoPool: e.currentTarget.checked })} /> Hide assets without a pool</label>

    <h3>Words to ignore</h3>
    <p class="ba-help">An asset whose name holds one of these words (any case) is hidden.</p>
    <form class="row" onsubmit={(e) => { e.preventDefault(); addWord(); }}>
      <input type="text" aria-label="A word to ignore" placeholder="airdrop" bind:value={word} maxlength="64" />
      <button type="submit" class="btn small" disabled={!word.trim()}>Add</button>
    </form>
    <ul class="chips" aria-label="Words ignored">
      {#each f.keywords as k (k)}<li><span translate="no"><SafeText text={k} /></span> <button type="button" class="link small" aria-label="Remove {k}" onclick={() => setFilters({ keywords: f.keywords.filter((x) => x !== k) })}>Remove</button></li>{/each}
    </ul>

    <h3>Names to ignore</h3>
    <p class="ba-help">Every asset of such a name (the whole name, any case): many assets made with one name.</p>
    <form class="row" onsubmit={(e) => { e.preventDefault(); addName(); }}>
      <input type="text" aria-label="A name to ignore" placeholder="FREE-COIN" bind:value={name} maxlength="64" />
      <button type="submit" class="btn small" disabled={!name.trim()}>Add</button>
    </form>
    <ul class="chips" aria-label="Names ignored">
      {#each f.names as n (n)}<li><span translate="no"><SafeText text={n} /></span> <button type="button" class="link small" onclick={() => unignoreName(n)}>Remove</button></li>{/each}
    </ul>

    <h3>Ignored assets</h3>
    {#if f.ignored.length === 0}
      <p class="muted">None. "Ignore this asset" on an asset, or on the notice of one received, puts it here.</p>
    {:else}
      <ul class="ignored" aria-label="Ignored assets">
        {#each f.ignored as id (id)}
          <li>
            <AssetBadge {id} label={labelOf(id)} size="s" />
            <span class="faint" translate="no">{nameOf(id)}</span>
            <button type="button" class="btn ghost small" onclick={() => unignoreAsset(id)}>Unignore</button>
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</section>

<style>
  .asset-filters {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    padding: 1rem 1.1rem;
    max-width: 48rem;
  }
  h3 {
    margin: 0.6rem 0 0;
    font-size: 0.98rem;
  }
  .check {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .row {
    display: flex;
    gap: 0.5rem;
  }
  .dead-after {
    align-items: center;
    flex-wrap: wrap;
    margin-left: 1.6rem;
  }
  .dead-after input {
    width: 7rem;
  }
  .chips,
  .ignored {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .chips li {
    padding: 0.15rem 0.5rem;
    border: 1px solid var(--line);
    border-radius: 999px;
  }
  .ignored {
    flex-direction: column;
  }
  .ignored li {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
</style>

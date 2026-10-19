<script lang="ts">
  // "N hidden: show them" under a list of assets the wallet hides some of (hide.svelte.ts): the list's own toggle.
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';

  // `some`: the node leaves out what the filters hide (a search), uncounted here.
  let { count, some = false, show = $bindable(false) }: { count: number; some?: boolean; show?: boolean } = $props();
</script>

{#if count > 0 || some}
  <p class="ba-hidden" data-testid="hidden-assets">
    <span class="muted">{#if count > 0}{tn('%n hidden by your asset filters', count)}{:else}{t('Your asset filters hide some assets')}{/if}</span>
    <button type="button" class="btn ghost small" aria-pressed={show} onclick={() => (show = !show)}>{show ? 'Hide them' : 'Show them'}</button>
    <button type="button" class="link small" onclick={() => go('settings', 'asset-filters')}>Asset filters</button>
  </p>
{/if}

<style>
  .ba-hidden {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    margin: 0.3rem 0;
    font-size: 0.88rem;
  }
</style>

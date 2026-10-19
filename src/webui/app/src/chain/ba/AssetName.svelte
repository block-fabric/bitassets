<script lang="ts">
  // An asset's ticker and, after it, its display name (muted: not unique, never instead of the ticker), always with a
  // separator between them: "GOLD · Gold Bar Token" (user feedback 2026-10-19: they ran together, "GOLDGold Bar
  // Token", where a space at the start of an {#if} block was dropped by the compiler). The separator is text (an
  // expression, which the compiler keeps whole), so that it is there when copied and read out too.
  import SafeText from '../../components/SafeText.svelte';

  let {
    label,
    display = '',
    tickerClass = 'ticker',
    displayClass = 'display muted',
  }: {
    /** The ticker (or name): text of the chain. */
    label: string;
    /** The display name: text of the chain; '' or undefined: none. */
    display?: string;
    tickerClass?: string;
    displayClass?: string;
  } = $props();
</script>

<span class="asset-name"><SafeText class={tickerClass} text={label} />{#if display}<span class="sep" aria-hidden="true">{' · '}</span><SafeText class={displayClass} text={display} title={display} />{/if}</span>

<style>
  .asset-name {
    display: inline;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .sep {
    white-space: pre;
    opacity: 0.7;
    font-weight: 400;
  }
  .asset-name :global(.display) {
    font-weight: 400;
  }
</style>

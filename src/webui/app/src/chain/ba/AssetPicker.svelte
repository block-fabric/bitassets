<script lang="ts">
  // Picking an asset among hundreds (user feedback 2026-10-19): a button that shows the asset chosen (its
  // logo, its number first, its label, what the wallet holds), and opens a list to search: type a name or a
  // number (#1739-0029) to filter, the arrow keys, Page Up/Down, Home/End to move, Enter to pick, Escape to
  // close. Favourites (the star) and the last picked are on top (picks.svelte.ts). Only the rows in view are
  // drawn. Each asset says its number: labels may look alike, numbers never do (audit 7 E-BA4).
  import { t } from '../../lib/i18n.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import AssetName from './AssetName.svelte';
  import { pickerRows, type PickChip, type PickChoice, type PickRow } from './picker';
  import { isFavourite, loadPicks, notePicked, picks, toggleFavourite } from './picks.svelte';
  import { displayNameOf } from './store.svelte';

  let {
    id,
    label,
    value = $bindable(''),
    choices,
    disabled = false,
    placeholder = 'Choose an asset',
    onpick,
    chips = [],
  }: {
    id: string;
    label: string;
    value?: string;
    choices: readonly PickChoice[];
    disabled?: boolean;
    placeholder?: string;
    onpick?: (id: string) => void;
    /** Filters at the top of the list (as those of Browse assets): one at a time, "All" first. */
    chips?: readonly PickChip[];
  } = $props();
  let chip = $state('');
  // Each with its display name (the store knows it), matched after the name.
  const named = $derived(choices.map((c) => (c.display !== undefined ? c : { ...c, display: displayNameOf(c.id) })));
  const filtered = $derived(chip ? named.filter((c) => chips.find((x) => x.id === chip)?.test(c) ?? true) : named);

  /** The height of a row, in pixels (the list draws only the rows in view). */
  const ROW = 44;
  const VIEW = 7;

  let open = $state(false);
  let query = $state('');
  let active = $state(0);
  let scrollTop = $state(0);
  let root: HTMLElement | undefined = $state();
  let input: HTMLInputElement | undefined = $state();
  let list: HTMLElement | undefined = $state();

  $effect(() => {
    void loadPicks();
  });

  const current = $derived(choices.find((c) => c.id === value));
  const rows: PickRow[] = $derived(open ? pickerRows(filtered, query, picks.favourites, picks.recent) : []);
  const first = $derived(Math.max(0, Math.floor(scrollTop / ROW) - 3));
  const last = $derived(Math.min(rows.length, first + VIEW + 7));
  const shown = $derived(rows.slice(first, last).map((row, i) => ({ row, index: first + i })));
  const optionId = (i: number) => `${id}-opt-${i}`;
  const isItem = (i: number) => rows[i]?.kind === 'item';

  function show() {
    if (disabled) return;
    open = true;
    query = '';
    scrollTop = 0;
    // The asset chosen, else the first one.
    queueMicrotask(() => {
      const at = rows.findIndex((r) => r.kind === 'item' && r.choice.id === value);
      moveTo(at >= 0 ? at : step(-1, 1));
      input?.focus();
    });
  }
  function hide(focusButton = true) {
    open = false;
    if (focusButton) queueMicrotask(() => root?.querySelector<HTMLElement>('.trigger')?.focus());
  }
  function pick(c: PickChoice) {
    value = c.id;
    notePicked(c.id);
    onpick?.(c.id);
    hide();
  }
  /** The next row that is an asset from `from`, `dir` at a time (the edge if none). */
  function step(from: number, dir: number): number {
    let i = from + dir;
    while (i >= 0 && i < rows.length && !isItem(i)) i += dir;
    if (i < 0 || i >= rows.length) return isItem(from) ? from : rows.findIndex((r) => r.kind === 'item');
    return i;
  }
  function moveTo(i: number) {
    if (i < 0) return;
    active = i;
    // Not laid out (yet): nothing to scroll.
    if (!list || list.clientHeight === 0) return;
    const top = i * ROW;
    if (top < list.scrollTop) list.scrollTop = top;
    else if (top + ROW > list.scrollTop + list.clientHeight) list.scrollTop = top + ROW - list.clientHeight;
    scrollTop = list.scrollTop;
  }
  function onkey(e: KeyboardEvent) {
    const page = VIEW - 1;
    if (e.key === 'ArrowDown') moveTo(step(active, 1));
    else if (e.key === 'ArrowUp') moveTo(step(active, -1));
    else if (e.key === 'PageDown') moveTo(step(Math.min(rows.length - 1, active + page) - 1, 1));
    else if (e.key === 'PageUp') moveTo(step(Math.max(0, active - page) + 1, -1));
    else if (e.key === 'Home' && e.ctrlKey) moveTo(step(-1, 1));
    else if (e.key === 'End' && e.ctrlKey) moveTo(step(rows.length, -1));
    else if (e.key === 'Enter') {
      const r = rows[active];
      if (r?.kind === 'item') pick(r.choice);
    } else if (e.key === 'Escape') hide();
    else if (e.key === 'Tab') hide(false);
    else return;
    e.preventDefault();
    e.stopPropagation();
  }
  function typed() {
    scrollTop = 0;
    if (list) list.scrollTop = 0;
    active = rows.findIndex((r) => r.kind === 'item');
  }
</script>

<svelte:window
  onmousedown={(e) => {
    if (open && root && !root.contains(e.target as Node)) hide(false);
  }}
/>

<div class="field asset-picker" bind:this={root}>
  <label for={id}>{label}</label>
  <button
    type="button"
    {id}
    class="trigger"
    data-value={value}
    {disabled}
    aria-haspopup="listbox"
    aria-expanded={open}
    onclick={() => (open ? hide() : show())}
    onkeydown={(e) => {
      if (!open && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
        e.preventDefault();
        show();
      }
    }}
  >
    {#if current}
      <AssetBadge id={current.id} label={current.label} size="s" showLabel={false} />
      <span class="name">
        {#if current.seq}<span class="seq" translate="no">#{current.seq}</span>{/if}
        <SafeText text={current.label} />
      </span>
      {#if current.warn}<span class="warn" title={current.warn}><Icon name="alert" size={14} label={current.warn} /></span>{/if}
      {#if current.held}<span class="held" translate="no">{current.held}</span>{/if}
    {:else}
      <span class="name muted">{t(placeholder)}</span>
    {/if}
    <Icon name="chevron" size={16} />
  </button>
  {#if open}
    <div class="pop">
      <input
        bind:this={input}
        bind:value={query}
        type="search"
        class="search"
        role="combobox"
        aria-label="Search assets"
        aria-expanded="true"
        aria-controls="{id}-list"
        aria-autocomplete="list"
        aria-activedescendant={isItem(active) ? optionId(active) : undefined}
        placeholder="Name or #number"
        autocomplete="off"
        spellcheck="false"
        oninput={typed}
        onkeydown={onkey}
      />
      {#if chips.length}
        <div class="chips" role="group" aria-label="Filter">
          <button type="button" class="ba-chip small" aria-pressed={chip === ''} onclick={() => { chip = ''; typed(); input?.focus(); }}>All</button>
          {#each chips as c (c.id)}<button type="button" class="ba-chip small" aria-pressed={chip === c.id} onclick={() => { chip = c.id; typed(); input?.focus(); }}>{t(c.label)}</button>{/each}
        </div>
      {/if}
      {#if rows.length === 0}
        <p class="none">No asset matches.</p>
      {:else}
        <div class="list" id="{id}-list" role="listbox" aria-label={label} tabindex="-1" bind:this={list} style:max-height="{VIEW * ROW}px" onscroll={() => (scrollTop = list?.scrollTop ?? 0)}>
          <div class="space" style:height="{rows.length * ROW}px">
            {#each shown as { row, index } (index)}
              {#if row.kind === 'head'}
                <div class="head caps" role="presentation" style:top="{index * ROW}px">{t(row.text)}</div>
              {:else}
                {@const c = row.choice}
                <!-- svelte-ignore a11y_click_events_have_key_events (the keys are the search field's: aria-activedescendant) -->
                <div
                  class="opt"
                  id={optionId(index)}
                  data-id={c.id}
                  role="option"
                  tabindex="-1"
                  aria-selected={c.id === value}
                  class:active={index === active}
                  style:top="{index * ROW}px"
                  onmousemove={() => (active = index)}
                  onclick={() => pick(c)}
                >
                  <AssetBadge id={c.id} label={c.label} size="s" showLabel={false} />
                  <span class="name">
                    {#if c.seq}<span class="seq" translate="no">#{c.seq}</span>{/if}
                    <AssetName label={c.label} display={c.display ?? ''} displayClass="display" />
                  </span>
                  {#if c.warn}<span class="warn" title={c.warn}><Icon name="alert" size={14} label={c.warn} /></span>{/if}
                  {#if c.held}<span class="held" translate="no">{c.held}</span>{/if}
                  <button
                    type="button"
                    class="star"
                    class:on={isFavourite(c.id)}
                    tabindex="-1"
                    aria-label={isFavourite(c.id) ? t('Remove from favourites') : t('Add to favourites')}
                    title={isFavourite(c.id) ? t('Remove from favourites') : t('Add to favourites')}
                    onclick={(e) => {
                      e.stopPropagation();
                      toggleFavourite(c.id);
                      input?.focus();
                    }}>{isFavourite(c.id) ? '★' : '☆'}</button>
                </div>
              {/if}
            {/each}
          </div>
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .asset-picker {
    position: relative;
  }
  .trigger {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    width: 100%;
    min-height: 2.4rem;
    padding: 0.3rem 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .trigger:disabled {
    opacity: 0.55;
    cursor: default;
  }
  .name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .name :global(.ticker) {
    font-weight: 600;
  }
  .name :global(.display) {
    color: var(--text-muted);
    font-weight: 400;
  }
  .seq {
    margin-right: 0.4rem;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }
  .held {
    flex: none;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }
  .pop {
    position: absolute;
    z-index: 30;
    left: 0;
    right: 0;
    top: calc(100% + 0.25rem);
    min-width: min(22rem, 90vw);
    padding: 0.4rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .search {
    width: 100%;
    margin-bottom: 0.35rem;
  }
  .list {
    position: relative;
    overflow-y: auto;
  }
  .space {
    position: relative;
  }
  .head,
  .opt {
    position: absolute;
    left: 0;
    right: 0;
    height: 44px;
    box-sizing: border-box;
  }
  .head {
    display: flex;
    align-items: flex-end;
    padding: 0 0.5rem 0.35rem;
    font-size: 0.72rem;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.5rem;
    border-radius: var(--radius-s);
    cursor: pointer;
  }
  .opt.active {
    background: var(--raised);
  }
  .opt[aria-selected='true'] .name {
    font-weight: 650;
  }
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem;
    margin-bottom: 0.35rem;
  }
  .warn {
    flex: none;
    color: var(--warning);
  }
  .star {
    flex: none;
    width: 1.8rem;
    height: 1.8rem;
    border: 0;
    border-radius: 50%;
    background: none;
    color: var(--text-faint);
    font-size: 1rem;
    cursor: pointer;
  }
  .star.on {
    color: var(--warning);
  }
  .star:hover {
    background: var(--surface-2);
  }
  .none {
    margin: 0.4rem 0.5rem;
    color: var(--text-muted);
  }
</style>

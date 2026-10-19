<script lang="ts">
  // Auctions: the Dutch auctions of the chain (open, coming, sold out or ended, and those collected), each with
  // its price falling block by block and the bid panel (ba/AuctionView.svelte); and selling by auction: what,
  // for what, the price of all of it at the start and at the end, how long, from when; drawn before it is
  // made. The Auctions tab of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount, niceTimeOffset } from '../../lib/format';
  import { nav, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app, toast, wallets } from '../../lib/state.svelte';
  import '../ba/ba.css';
  import { barWidth } from '../ba/actions';
  import AssetSelect from '../ba/AssetSelect.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import { hiddenAsset } from '../ba/visible.svelte';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import AuctionView from '../ba/AuctionView.svelte';
  import Chart from '../ba/Chart.svelte';
  import UnitsInput from '../ba/UnitsInput.svelte';
  import { amountParam, approx, assetArg, AUCTION_STATUS, auctionProblems, auctionProgress, CHN_ID, formatAmount, formatPrice, formatUnits, isChn, parseUnits, unitPrice, unitsAtMost, unitsOf, type Auction } from '../ba/model';
  import { ba, decimalsOf, labelOf, moreAuctions, nameOf, refreshBitAssets, sellIntent, showMoreAuctions, startBitAssets } from '../ba/store.svelte';

  $effect(() => startBitAssets());

  type Tab = 'browse' | 'sell';
  const TABS: { id: Tab; label: string }[] = [
    { id: 'browse', label: 'Browse' },
    { id: 'sell', label: 'Sell by auction' },
  ];
  const tab = $derived<Tab>(nav.tab === 'sell' ? 'sell' : 'browse');

  // --- Browse ---
  type Filter = 'live' | 'open' | 'upcoming' | 'done' | 'closed';
  let filter = $state<Filter>('live');
  let closed = $state<Auction[]>([]);
  let picked = $state<string | null>(null);
  $effect(() => {
    void nav.focus;
    const key = takeFocus('auctions');
    if (key) {
      picked = key;
      filter = 'live';
    }
  });
  // Those collected already: asked for when shown.
  $effect(() => {
    if (filter !== 'closed') return;
    void ba.height;
    rpc
      .raw<Auction[]>('listauctions', [true, 1000, 0])
      .then((all) => (closed = (all ?? []).filter((a) => a.status === 'closed')))
      .catch(() => (closed = []));
  });
  const FILTERS: { id: Filter; label: string }[] = [
    { id: 'live', label: 'All running' },
    { id: 'open', label: 'Open' },
    { id: 'upcoming', label: 'Upcoming' },
    { id: 'done', label: 'Sold out or ended' },
    { id: 'closed', label: 'Collected' },
  ];
  // Searched, sorted (user feedback 2026-10-19), without the auctions of assets the wallet hides unless shown.
  type AuctionSort = 'newest' | 'ending' | 'cheapest' | 'bids' | 'name';
  let query = $state('');
  let sort = $state<AuctionSort>('newest');
  let showHidden = $state(false);
  const isHidden = (a: Auction) => hiddenAsset(a.base_id, a.base) || hiddenAsset(a.quote_id, a.quote);
  const SORTS: Record<AuctionSort, (x: Auction, y: Auction) => number> = {
    newest: (x, y) => y.start_height - x.start_height,
    ending: (x, y) => x.end_height - y.end_height,
    cheapest: (x, y) => (x.unit_price || Infinity) - (y.unit_price || Infinity),
    bids: (x, y) => y.bids - x.bids,
    name: (x, y) => x.base.localeCompare(y.base),
  };
  const matching = $derived(
    (filter === 'closed'
      ? closed
      : ba.auctions.filter((a) => filter === 'live' || (filter === 'open' && a.status === 'open') || (filter === 'upcoming' && a.status === 'upcoming') || (filter === 'done' && (a.status === 'sold out' || a.status === 'ended')))
    ).filter((a) => {
      const q = query.trim().toLowerCase();
      return !q || a.base.toLowerCase().includes(q) || a.quote.toLowerCase().includes(q) || a.auction.startsWith(q);
    }),
  );
  const hiddenCount = $derived(showHidden ? 0 : matching.filter(isHidden).length);
  const list = $derived([...(showHidden ? matching : matching.filter((a) => !isHidden(a)))].sort(SORTS[sort]));
  const counts = $derived({
    live: Math.max(ba.auctions.length, ba.info?.auctions ?? 0),
    open: ba.auctions.filter((a) => a.status === 'open').length,
    upcoming: ba.auctions.filter((a) => a.status === 'upcoming').length,
    done: ba.auctions.filter((a) => a.status === 'sold out' || a.status === 'ended').length,
    closed: closed.length,
  });
  $effect(() => {
    if (tab === 'browse' && list.length && (picked === null || !list.some((a) => a.auction === picked))) picked = list[0].auction;
  });
  const shown = $derived(list.find((a) => a.auction === picked) ?? ba.auctions.find((a) => a.auction === picked) ?? null);
  const mineIds = $derived(new Set((ba.mine?.receipts ?? []).map((r) => r.auction)));

  // --- Sell ---
  let base = $state('');
  let quote = $state(CHN_ID);
  let amountText = $state('');
  let amount = $state<bigint | null>(null);
  let startText = $state('');
  let endText = $state('');
  let duration = $state(60);
  let startIn = $state(1);
  let busy = $state(false);
  const holdings = $derived((ba.mine?.assets ?? []).filter((h) => approx(h.balance) > 0));
  const sellChoices = $derived(holdings.map((h) => ({ id: h.asset, label: h.label, held: formatAmount(h.balance, h.decimals, { max: 4 }) })));
  const quoteChoices = $derived(ba.assets.map((a) => ({ id: a.asset, label: a.label, seq: a.seq })));
  $effect(() => {
    if (sellIntent.asset) {
      base = sellIntent.asset;
      sellIntent.asset = '';
      nav.tab = 'sell';
    }
  });
  $effect(() => {
    // The first asset the wallet holds, once known.
    if (!base && ba.loaded) base = sellChoices[0]?.id ?? CHN_ID;
  });
  const dbase = $derived(decimalsOf(base));
  const dquote = $derived(decimalsOf(quote));
  const heldBase = $derived.by(() => {
    if (isChn(base)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((a) => a.asset === base);
    return h ? unitsAtMost(h.balance, h.decimals) : 0n;
  });
  const startParsed = $derived(parseUnits(startText, dquote));
  const endParsed = $derived(parseUnits(endText, dquote));
  const problems = $derived(
    amountText || startText || endText
      ? auctionProblems({
          sameAsset: base === quote,
          amount: parseUnits(amountText, dbase),
          start: startParsed,
          end: endParsed,
          duration,
          startIn,
          sellsChn: isChn(base),
          held: heldBase,
        })
      : [],
  );
  const ready = $derived(problems.length === 0 && amount !== null && startParsed.ok && endParsed.ok);
  const spacing = $derived(app.info?.blockSpacing ?? 600);
  const preview = $derived.by(() => {
    if (!ready || amount === null || !startParsed.ok || !endParsed.ok) return [];
    const whole = Number(amount) / 10 ** dbase;
    const s = Number(startParsed.units) / 10 ** dquote / whole;
    const e = Number(endParsed.units) / 10 ** dquote / whole;
    const from = Math.max(0, ba.height) + startIn;
    return [
      { x: from, y: s },
      { x: from + duration - 1, y: e },
    ];
  });

  async function sell() {
    const wallet = wallets.current;
    if (wallet === null || !ready || amount === null || !startParsed.ok || !endParsed.ok || busy) return;
    // Named with their numbers in the confirmation: look-alike labels told apart (audit port-2 #5).
    const lb = nameOf(base);
    const lq = nameOf(quote);
    const r = await ask({
      title: t('Start an auction'),
      message: t('Sell %1 %2 for %3, from %4 down to %5 %3 for all of it, over %6 blocks?', formatUnits(amount, dbase), lb, lq, formatUnits(startParsed.units, dquote), formatUnits(endParsed.units, dquote), String(duration)),
      informative: t('It starts %1 block(s) after the next block and takes bids for about %2. Its receipt, in My assets, collects what it brings in and what is left; before any bid, it cancels it.', String(startIn), niceTimeOffset(duration * spacing)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Start the auction', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'createauction', [assetArg(base), amountParam(amount, dbase), assetArg(quote), amountParam(startParsed.units, dquote), amountParam(endParsed.units, dquote), duration, startIn]);
      if (!done) return;
      toast('success', t('Auction started'), t('It takes bids from the block after the next. Its receipt is in My assets.'));
      amountText = '';
      picked = done.txid;
      nav.tab = 'browse';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Start an auction'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="page ba-page auctions">
  <header class="page-head">
    <h1>Auctions</h1>
    <span class="muted">The price of all of it falls block by block: the longer you wait, the less you pay, if anything is left.</span>
  </header>
  <TabRow label="Auctions" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} />

  {#if tab === 'browse'}
    <div class="ba-chips" role="group" aria-label="Filter">
      {#each FILTERS as f (f.id)}
        <button type="button" class="ba-chip" aria-pressed={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)} {#if f.id !== 'closed'}<span class="count">{counts[f.id]}</span>{/if}</button>
      {/each}
    </div>
    <div class="finder-row">
      <input type="search" class="search" placeholder="Find an auction by an asset" aria-label="Find an auction" bind:value={query} />
      <label class="sort"><span class="muted">Sort</span>
        <select bind:value={sort} aria-label="Sort auctions">
          <option value="newest">Newest</option>
          <option value="ending">Ending soonest</option>
          <option value="cheapest">Cheapest a unit</option>
          <option value="bids">Most bids</option>
          <option value="name">Name</option>
        </select>
      </label>
    </div>
    <HiddenToggle count={hiddenCount} bind:show={showHidden} />
    {#if list.length === 0}
      <div class="panel ba-empty">{filter === 'closed' ? 'No auction was collected yet.' : 'No auction here.'} <button type="button" class="btn small" onclick={() => (nav.tab = 'sell')}>Sell by auction</button></div>
    {:else}
      <div class="ba-split">
        <section class="panel">
          <ul class="ba-list" aria-label="Auctions">
            {#each list as a (a.auction)}
              {@const prog = auctionProgress(a, ba.height)}
              {@const up = unitPrice(a.unit_price)}
              <li>
                <button type="button" class="ba-row" aria-current={picked === a.auction} onclick={() => (picked = a.auction)}>
                  <AssetBadge id={a.base_id} label={a.base} size="s" showLabel={false} />
                  <span class="main">
                    <span>{formatAmount(a.remaining, decimalsOf(a.base_id), { max: 4 })} <SafeText text={a.base} /> <span class="faint">for</span> <SafeText text={a.quote} /></span>
                    <span class="ba-bar"><span use:barWidth={prog.sold}></span></span>
                  </span>
                  <span class="side">
                    <span class="small">{#if up.kind === 'each'}{up.price}{:else if up.kind === 'buys'}1/{up.amount}{:else}–{/if}</span>
                    <span class="faint small">{t(AUCTION_STATUS[a.status] ?? 'Ended')}{mineIds.has(a.auction) ? ` · ${t('yours')}` : ''}</span>
                  </span>
                </button>
              </li>
            {/each}
          </ul>
          {#if filter !== 'closed' && moreAuctions()}
            <p class="ba-help more-auctions">{t('Showing the newest %1 of %2 auctions.', formatCount(ba.auctions.length), formatCount(ba.info?.auctions ?? 0))} <button type="button" class="btn small" onclick={showMoreAuctions}>Show more</button></p>
          {/if}
        </section>
        {#if shown}
          {#key shown.auction}<AuctionView auction={shown} />{/key}
        {/if}
      </div>
    {/if}
  {:else}
    <div class="ba-split">
      <form class="panel ba-form sell" onsubmit={(e) => { e.preventDefault(); void sell(); }}>
        <div class="leg">
          <AssetSelect id="sell-asset" label="Sell" bind:value={base} choices={sellChoices} />
          <UnitsInput id="sell-amount" label="How much" decimals={dbase} bind:units={amount} bind:text={amountText} max={heldBase} unit={labelOf(base)} />
        </div>
        <AssetSelect id="sell-for" label="For" bind:value={quote} choices={quoteChoices} />
        <div class="leg">
          <UnitsInput id="sell-start" label="Start price, for all of it" decimals={dquote} bind:text={startText} unit={labelOf(quote)} />
          <UnitsInput id="sell-end" label="End price: the least you take" decimals={dquote} bind:text={endText} unit={labelOf(quote)} />
        </div>
        <div class="leg">
          <div class="field">
            <label for="sell-duration">Runs for (blocks)</label>
            <input id="sell-duration" type="number" min="1" max="1000000" bind:value={duration} />
            <span class="hint">About {niceTimeOffset(duration * spacing)}</span>
          </div>
          <div class="field">
            <label for="sell-start-in">Starts in (blocks)</label>
            <input id="sell-start-in" type="number" min="1" max="52560" bind:value={startIn} />
            <span class="hint">{tn('%n block(s) after the next', startIn)}</span>
          </div>
        </div>
        {#if problems.length}<ul class="ba-problems list" role="status">{#each problems as p (p)}<li>{t(p)}</li>{/each}</ul>{/if}
        {#if ready && amount !== null && startParsed.ok && endParsed.ok}
          <dl class="ba-lines">
            <dt>One, at the start</dt><dd>{formatPrice(Number(startParsed.units) / 10 ** dquote / (Number(amount) / 10 ** dbase))} <SafeText text={labelOf(quote)} /></dd>
            <dt>One, at the end</dt><dd>{formatPrice(Number(endParsed.units) / 10 ** dquote / (Number(amount) / 10 ** dbase))} <SafeText text={labelOf(quote)} /></dd>
            <dt>Takes bids</dt><dd>blocks {formatCount(ba.height + startIn)} to {formatCount(ba.height + startIn + duration - 1)}</dd>
          </dl>
        {/if}
        <div class="row"><button type="submit" class="btn primary" disabled={busy || wallets.current === null || !ready}><Icon name="gavel" size={15} /> {busy ? 'Starting…' : 'Start the auction'}</button></div>
      </form>
      <section class="panel">
        <div class="panel-head"><h3>The price of one, block by block</h3></div>
        <div class="panel-body">
          {#if preview.length}
            <Chart points={preview} label={t('The price of one unit over the auction')} formatX={(x) => t('block %1', String(Math.round(x)))} yZero />
          {:else}
            <p class="ba-help">Give what you sell, for what, and the prices: the line they make shows here. A bid buys at the price of its block, rounded in your favour; what is not sold comes back to you.</p>
          {/if}
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .finder-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    margin: 0.5rem 0;
  }
  .finder-row .search {
    flex: 1 1 14rem;
  }
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .sell {
    padding: 1.2rem;
  }
  .leg {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(12rem, 100%), 1fr));
    gap: 0.75rem;
    align-items: start;
  }
  .list {
    margin: 0;
    padding-left: 1.8rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>

<script lang="ts">
  // Putting two assets in their pool, for shares of it and 0.3% of every trade. Into a pool someone provides
  // liquidity to, the second amount follows from the first at the pool's price (only what its price takes goes
  // in); a new pool, or one everyone left (its dust counts toward the amounts), takes both, which set its price
  // (asked about first). The shares and the share of the pool it gives, worked out as the chain works them out
  // (amm::Provide). The Add liquidity card of the Trade tab of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetSelect from './AssetSelect.svelte';
  import { hiddenAsset } from './visible.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { amountParam, assetArg, CHN_ID, formatAmount, formatPercent, formatPrice, formatUnits, isChn, MIN_LIQUIDITY, MIN_OPEN_CHN, MIN_OPEN_SHARES, pairedAmount, provide, unitsAtMost, unitsOf, type Pool, type Amount } from './model';
  import { assetById, ba, decimalsOf, labelOf, nameOf, poolOf, refreshBitAssets, tradeIntent } from './store.svelte';

  let a = $state('');
  let b = $state(CHN_ID);
  let aText = $state('');
  let aUnits = $state<bigint | null>(null);
  let bText = $state('');
  let bUnits = $state<bigint | null>(null);
  let slippage = $state(1);
  let busy = $state(false);

  // The newest assets the store keeps, and those the wallet holds, without what it hides (unless chosen already).
  const choices = $derived.by(() => {
    const seen = new Set<string>();
    const all = [...ba.assets.map((x) => ({ id: x.asset, label: x.label, seq: x.seq })), ...(ba.mine?.assets ?? []).map((h) => ({ id: h.asset, label: h.label, seq: assetById(h.asset)?.seq ?? '' }))];
    return all.filter((c) => !seen.has(c.id) && (seen.add(c.id), true) && (c.id === a || c.id === b || !hiddenAsset(c.id, c.label)));
  });
  $effect(() => {
    if (!tradeIntent.a && !tradeIntent.b) return;
    if (tradeIntent.a) a = tradeIntent.a;
    if (tradeIntent.b) b = tradeIntent.b;
    tradeIntent.a = '';
    tradeIntent.b = '';
  });
  $effect(() => {
    if (!a && choices.length) a = choices[0].id;
  });

  const da = $derived(decimalsOf(a));
  const db = $derived(decimalsOf(b));
  const pool = $derived(a && b && a !== b ? poolOf(a, b) : undefined);
  const setsPrice = $derived(!pool || pool.abandoned);
  /** The pool as the chain keeps it, in units: the first asset of the form first. */
  const units = $derived.by(() => {
    if (!pool) return { reserve0: 0n, reserve1: 0n, shares: 0n, chn0: isChn(a), chn1: isChn(b) };
    const flip = pool.asset_a_id !== a;
    return {
      reserve0: unitsOf(flip ? pool.reserve_b : pool.reserve_a, da),
      reserve1: unitsOf(flip ? pool.reserve_a : pool.reserve_b, db),
      shares: unitsOf(pool.shares, 0),
      chn0: isChn(a),
      chn1: isChn(b),
    };
  });
  // The second amount at the pool's price, into a pool someone provides liquidity to.
  $effect(() => {
    if (setsPrice) return;
    bText = aUnits !== null && units.reserve0 > 0n ? formatUnits(pairedAmount(aUnits, units.reserve0, units.reserve1), db, { group: false }) : '';
  });
  /** What the dust of an abandoned pool counts for: the amounts given are what it reopens with. */
  const offer = $derived.by(() => {
    if (aUnits === null || bUnits === null) return null;
    if (pool && pool.abandoned) return { a: aUnits - units.reserve0, b: bUnits - units.reserve1 };
    return { a: aUnits, b: bUnits };
  });
  const deposit = $derived(offer && offer.a > 0n && offer.b > 0n ? provide(units, offer.a, offer.b) : null);
  const shareAfter = $derived(deposit ? Number(deposit.shares) / Number((setsPrice ? deposit.shares + MIN_LIQUIDITY : units.shares + deposit.shares) || 1n) : 0);
  const heldA = $derived(isChn(a) ? (wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null) : (ba.mine?.assets.find((x) => x.asset === a) ? unitsAtMost(ba.mine.assets.find((x) => x.asset === a)!.balance, da) : 0n));
  const heldB = $derived(isChn(b) ? (wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null) : (ba.mine?.assets.find((x) => x.asset === b) ? unitsAtMost(ba.mine.assets.find((x) => x.asset === b)!.balance, db) : 0n));

  const problem = $derived.by(() => {
    if (!a || !b || a === b) return 'Pick two different assets.';
    if (aUnits === null || bUnits === null) return '';
    if (pool?.abandoned && offer && (offer.a <= 0n || offer.b <= 0n)) return 'This pool holds dust left by its last providers, which counts toward what it reopens with: give more than that of each.';
    if (!deposit) return 'Too little: it makes no shares (a new pool needs the product of the amounts, in units, above a million).';
    if (setsPrice) {
      const chn = isChn(a) ? aUnits : isChn(b) ? bUnits : MIN_OPEN_CHN;
      if (deposit.shares < MIN_OPEN_SHARES || chn < MIN_OPEN_CHN) return 'Too little to open a pool: it takes at least 0.01 CHN on the CHN side, and amounts whose product, in units, is above 10,201,000,000.';
    }
    return '';
  });

  async function add() {
    const wallet = wallets.current;
    if (wallet === null || aUnits === null || bUnits === null || !deposit || problem || busy) return;
    // What the confirmation shows is what is signed: the store refreshes while the dialog is open, so the
    // amounts, the pool's state and the deposit are taken now and the parameters built from them (audit 7
    // E-BA1). Both amounts always go: into a pool someone provides to, they are the most that goes in.
    const sA = a;
    const sB = b;
    const sAUnits = aUnits;
    const sBUnits = bUnits;
    const sDa = da;
    const sDb = db;
    const sSetsPrice = setsPrice;
    const sAbandoned = !!pool?.abandoned;
    const sDeposit = deposit;
    const sSlippage = slippage;
    const sShare = shareAfter;
    // Named with their numbers in the confirmation: look-alike labels told apart (audit port-2 #5).
    const la = nameOf(sA);
    const lb = nameOf(sB);
    // The pool may have changed since the amounts were typed (made, reopened or left): asked again.
    let now: Pool | null = null;
    try {
      now = await rpc.raw<Pool>('getpool', [assetArg(sA), assetArg(sB)]);
    } catch {
      now = null;
    }
    const nowSets = !now || now.abandoned;
    if (nowSets !== sSetsPrice) {
      await inform(t('Add liquidity'), nowSets ? t('Nobody provides liquidity to this pool any more since you typed the amounts: give both amounts, which set its price.') : t('A pool of these assets was made meanwhile: its price sets the second amount now. Check the amounts.'), 'warning');
      void refreshBitAssets();
      return;
    }
    const message = sSetsPrice
      ? sAbandoned
        ? t('Reopen the %1 / %2 pool with %3 %1 and %4 %2?', la, lb, formatUnits(sAUnits, sDa), formatUnits(sBUnits, sDb))
        : t('Make the %1 / %2 pool with %3 %1 and %4 %2?', la, lb, formatUnits(sAUnits, sDa), formatUnits(sBUnits, sDb))
      : t('Add about %1 %2 and %3 %4 to their pool?', formatUnits(sDeposit.take0, sDa), la, formatUnits(sDeposit.take1, sDb), lb);
    const informative = sSetsPrice
      ? t('Its price starts at 1 %1 = %2 %3: check it is the price you want. Should someone open it first, the deposit is refused, not put in at their price.', la, formatPrice(Number(sBUnits) / 10 ** sDb / (Number(sAUnits) / 10 ** sDa)), lb)
      : t('About %1 shares, %2 of the pool. Should the pool move first, what its price does not take comes back.', String(sDeposit.shares), formatPercent(sShare * 100));
    const r = await ask({
      title: sSetsPrice ? (sAbandoned ? t('Reopen a pool') : t('Make a pool')) : t('Add liquidity'),
      message,
      informative,
      level: sAbandoned ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: sSetsPrice ? 'Make it' : 'Add', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      // The amounts confirmed (the second, into a pool someone provides to, the most that goes in), and what
      // was expected: a deposit meant to set the price is refused if someone opened the pool meanwhile.
      const params: unknown[] = [assetArg(sA), amountParam(sAUnits, sDa), assetArg(sB), amountParam(sSetsPrice ? sBUnits : sDeposit.take1, sDb), sSlippage, sSetsPrice];
      const done = await walletRpc<{ txid: string; amount_a: Amount; amount_b: Amount; shares: Amount }>(wallet, 'addliquidity', params);
      if (!done) return;
      toast('success', t('Adding liquidity'), t('%1 %2 and %3 %4 into their pool, for about %5 shares, with the next block.', formatAmount(done.amount_a, sDa), la, formatAmount(done.amount_b, sDb), lb, String(done.shares)));
      aText = '';
      bText = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Add liquidity'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="ba-split">
  <form class="panel ba-form card" onsubmit={(e) => { e.preventDefault(); void add(); }}>
    <div class="leg">
      <AssetSelect id="lq-a" label="First asset" bind:value={a} {choices} />
      <UnitsInput id="lq-amount-a" label="Amount" decimals={da} bind:units={aUnits} bind:text={aText} max={heldA} unit={labelOf(a)} />
    </div>
    <div class="plus" aria-hidden="true"><Icon name="plus" size={18} /></div>
    <div class="leg">
      <AssetSelect id="lq-b" label="Second asset" bind:value={b} {choices} />
      <UnitsInput id="lq-amount-b" label="Amount" decimals={db} bind:units={bUnits} bind:text={bText} max={heldB} unit={labelOf(b)} readonly={!setsPrice} hint={setsPrice ? '' : 'Worked out from the pool\'s price'} />
    </div>

    <div class="field">
      <label for="lq-slippage">Slippage</label>
      <select id="lq-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
      <span class="hint">How many fewer shares than quoted it may give, if the pool moves first.</span>
    </div>

    {#if a && b && a !== b}
      {#if !pool}
        <p class="ba-help"><strong>A new pool:</strong> your amounts set its price. It opens with at least 0.01 CHN on a CHN side, and keeps 1,000 shares for good, so that it is never emptied.</p>
      {:else if pool.abandoned}
        <p class="ba-problems"><strong>Nobody provides liquidity to this pool any more.</strong> It holds only the dust every pool keeps ({formatAmount(pool.asset_a_id === a ? pool.reserve_a : pool.reserve_b, da)} <SafeText text={labelOf(a)} /> and {formatAmount(pool.asset_a_id === a ? pool.reserve_b : pool.reserve_a, db)} <SafeText text={labelOf(b)} />), at whatever price it was left at: your amounts set its price, as for a new pool, and the dust counts toward them.</p>
      {:else}
        <p class="ba-help">1 <SafeText text={labelOf(a)} /> = {formatPrice(units.reserve0 > 0n ? Number(units.reserve1) / 10 ** db / (Number(units.reserve0) / 10 ** da) : undefined)} <SafeText text={labelOf(b)} /> · the pool holds {formatUnits(units.reserve0, da)} <SafeText text={labelOf(a)} /> and {formatUnits(units.reserve1, db)} <SafeText text={labelOf(b)} />.</p>
      {/if}
    {/if}

    {#if problem}<p class="ba-problems" role="status">{t(problem)}</p>{/if}

    {#if deposit && !problem}
      <dl class="ba-lines">
        <dt>Goes in</dt><dd>{formatUnits(deposit.take0, da)} <SafeText text={labelOf(a)} /></dd>
        <dt></dt><dd>{formatUnits(deposit.take1, db)} <SafeText text={labelOf(b)} /></dd>
        <dt>Shares</dt><dd class="strong">≈ {deposit.shares.toString()}</dd>
        <dt>Share of the pool</dt><dd>{formatPercent(shareAfter * 100)}</dd>
        {#if setsPrice && aUnits && bUnits}<dt>Its price</dt><dd>1 <SafeText text={labelOf(a)} /> = {formatPrice(Number(bUnits) / 10 ** db / (Number(aUnits) / 10 ** da))} <SafeText text={labelOf(b)} /></dd>{/if}
      </dl>
    {/if}

    <div class="row"><button type="submit" class="btn primary wide" disabled={busy || wallets.current === null || !deposit || !!problem}><Icon name="pool" size={16} /> {busy ? 'Adding…' : setsPrice ? 'Make the pool' : 'Add liquidity'}</button></div>
  </form>

  <section class="panel">
    <div class="panel-head"><h3>How a pool works</h3></div>
    <div class="panel-body">
      <ul class="how">
        <li>A pool trades two assets at the price their amounts in it set: what one of them holds times what the other holds never falls (a constant product).</li>
        <li>Every trade leaves 0.3% in the pool, for those who provide its liquidity, in proportion to their shares.</li>
        <li>Your shares are tokens like any other: take them out in My assets, for your part of what the pool holds then.</li>
        <li>The price moves as people trade: what you take out may be more of one asset and less of the other than what you put in.</li>
      </ul>
    </div>
  </section>
</div>

<style>
  .card {
    padding: 1.2rem;
  }
  .leg {
    display: grid;
    grid-template-columns: minmax(8rem, 0.8fr) minmax(0, 1.2fr);
    gap: 0.75rem;
    align-items: start;
  }
  .plus {
    display: grid;
    place-items: center;
    color: var(--text-muted);
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
  .how {
    margin: 0;
    padding-left: 1.1rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    color: var(--text-muted);
    font-size: 0.92rem;
  }
</style>

<script lang="ts">
  // My assets: what this wallet holds and controls, and what it is doing. Holdings (each asset, its value in
  // CHN where it has a market, and the panel to send, trade, auction, mint, change the data, give control, fix
  // the supply or burn: ba/ManagePanel.svelte); the assets being created (the two steps, followed until the
  // asset is listed; a reservation released); pool shares (their worth, taken out in part or whole); auctions
  // to collect or cancel; and the asset activity of the wallet. The My assets and Activity tabs of the Qt
  // window of BitAssets.
  import { stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount, formatTime, shortHash } from '../../lib/format';
  import { go, nav, PAGES, takeFocus } from '../../lib/nav.svelte';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../ba/ba.css';
  import { barWidth } from '../ba/actions';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import LocalTxCell from '../LocalTxCell.svelte';
  import { assetMovesOf } from '../txinfo.svelte';
  import ManagePanel from '../ba/ManagePanel.svelte';
  import Issue from './Issue.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import { withoutHidden } from '../ba/visible.svelte';
  import { chainTxHidden } from '../txinfo.svelte';
  import UnitsInput from '../ba/UnitsInput.svelte';
  import {
    amountParam,
    approx,
    assetArg,
    formatAmount,
    formatPercent,
    formatPrice,
    formatUnits,
    issueStage,
    MAX_DECIMALS,
    AUCTION_STATUS,
    OPERATIONS,
    receiptAction,
    unitsOf,
    withdraw,
    type Amount,
    type Auction,
    type Position,
    type Reservation,
  } from '../ba/model';
  import { simpleMode } from '../../lib/uimode.svelte';
  import { ACTIVITY_PAGE, auctionById, ba, baView, chnPrice, decimalsOf, dropPending, MAX_ACTIVITY, nameOf, pendingOf, pendingOpOf, poolOf, refreshBitAssets, registerNow, startBitAssets, tradeIntent, type PendingIssue, unlockHoldings } from '../ba/store.svelte';
  import PendingCard from '../../components/PendingCard.svelte';

  $effect(() => startBitAssets());

  type Tab = 'holdings' | 'register' | 'issuing' | 'liquidity' | 'auctions' | 'activity';
  // Register new asset is a tab here (user feedback 2026-10-19; go('issue') leads to it, chain/local.ts tabOf).
  const TABS: { id: Tab; label: string }[] = [
    { id: 'holdings', label: 'Holdings' },
    { id: 'register', label: 'Register new asset' },
    { id: 'issuing', label: 'Being issued' },
    { id: 'liquidity', label: 'Pool shares' },
    { id: 'auctions', label: 'My auctions' },
    { id: 'activity', label: 'Activity' },
  ];
  const outer = $derived(nav.tab.split('/')[0]);
  const tab = $derived<Tab>(TABS.some((x) => x.id === outer) ? (outer as Tab) : 'holdings');
  // Simple mode (lib/uimode.svelte.ts): what the wallet holds, what it is creating and its activity; pool shares
  // and auctions are Advanced's, shown when asked for.
  const simple = $derived(simpleMode());
  const tabs = $derived(
    simple
      ? TABS.filter((x) => x.id === 'holdings' || x.id === 'register' || x.id === 'issuing' || x.id === 'activity' || x.id === tab).map((x) => (x.id === 'issuing' ? { ...x, label: 'Being created' } : x.id === 'register' ? { ...x, label: 'Create a token' } : x))
      : TABS,
  );

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const mine = $derived(ba.wallet === wallets.current ? ba.mine : null);
  // What the wallet hides (its asset filters: ba/hide.svelte.ts) is left out of its holdings and activity, unless shown.
  let showHidden = $state(false);
  const visible = $derived(withoutHidden(mine?.assets ?? [], (h) => h.asset, (h) => h.label, showHidden));
  const activity = $derived(showHidden ? ba.activity : ba.activity.filter((a) => !chainTxHidden(a.txid)));
  const holdings = $derived(visible.items.slice().sort((a, b) => (chnPrice(b.asset) ?? 0) * approx(b.balance) - (chnPrice(a.asset) ?? 0) * approx(a.balance) || a.label.localeCompare(b.label)));
  let picked = $state<string | null>(null);
  $effect(() => {
    void nav.focus;
    const key = takeFocus('my-assets');
    if (key) {
      picked = key;
      nav.tab = 'holdings';
    }
  });
  $effect(() => {
    if (holdings.length && (picked === null || !holdings.some((h) => h.asset === picked))) picked = holdings[0].asset;
  });
  const holding = $derived(holdings.find((h) => h.asset === picked) ?? null);
  const worth = $derived(holdings.reduce((n, h) => n + (chnPrice(h.asset) ?? 0) * approx(h.balance), 0));
  const priced = $derived(holdings.filter((h) => chnPrice(h.asset) !== undefined).length);

  // --- Being issued: what this app is creating, and the reservations of the wallet ---
  const pending = $derived(pendingOf(wallets.current));
  /** The reservations the app does not know the supply of (made with the console, or the journal lost). */
  const strays = $derived((mine?.reservations ?? []).filter((r) => !pending.some((p) => p.name === r.name)));
  const issuingCount = $derived(pending.length + strays.length);

  function stageOf(p: PendingIssue): ReturnType<typeof issueStage> {
    const r = mine?.reservations.find((x) => x.name === p.name);
    const registered = ba.assets.some((a) => a.asset === p.asset);
    if (!r && p.registration && !registered) return 'registering';
    return issueStage(r, registered);
  }
  function waitOf(name: string): number {
    return mine?.reservations.find((x) => x.name === name)?.wait ?? 0;
  }

  async function release(name: string) {
    const wallet = wallets.current;
    if (wallet === null) return;
    const r = await ask({
      title: t('Release'),
      message: t('Give up the reservation of %1?', name),
      informative: t('The name is free again once the release is in a block; anyone can then reserve it.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Release', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'releaseassetreservation', [name]);
      if (!done) return;
      dropPending(wallet, name);
      toast('success', t('Reservation released'), t('The reservation of %1 is released with the next block.', name));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Release'), errorText(e), 'error');
    }
  }

  async function register(p: PendingIssue) {
    try {
      const txid = await registerNow(p);
      if (txid) toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', p.name));
    } catch (e) {
      await inform(t('Register'), errorText(e), 'error');
    }
  }

  // A reservation the app has no record of: registered with what is typed here.
  let strayName = $state<string | null>(null);
  let straySupplyText = $state('0');
  let straySupply = $state<bigint | null>(0n);
  let strayDecimals = $state(2);
  let strayInfo = $state('');
  let strayPublic = $state(true);
  async function registerStray(r: Reservation) {
    const wallet = wallets.current;
    if (wallet === null || !r.name || straySupply === null) return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'registerasset', [r.name, amountParam(straySupply, strayDecimals), strayDecimals, strayInfo.trim() ? { info: strayInfo } : {}, strayPublic]);
      if (!done) return;
      strayName = null;
      toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', r.name));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Register'), errorText(e), 'error');
    }
  }

  // --- Pool shares ---
  let removing = $state<string | null>(null);
  let percent = $state(100);
  let slippage = $state(1);
  function preview(pos: Position): { a: bigint; b: bigint } | null {
    const pool = poolOf(pos.asset_a_id, pos.asset_b_id);
    if (!pool) return null;
    const da = decimalsOf(pos.asset_a_id);
    const db = decimalsOf(pos.asset_b_id);
    const flip = pool.asset_a_id !== pos.asset_a_id;
    const ra = unitsOf(flip ? pool.reserve_b : pool.reserve_a, da);
    const rb = unitsOf(flip ? pool.reserve_a : pool.reserve_b, db);
    const held = unitsOf(pos.shares, 0);
    const shares = percent >= 100 ? held : (held * BigInt(Math.round(percent * 100))) / 10000n;
    const [a, b] = withdraw({ reserve0: ra, reserve1: rb, shares: unitsOf(pool.shares, 0), chn0: /^0+$/.test(pos.asset_a_id), chn1: /^0+$/.test(pos.asset_b_id) }, shares);
    return { a, b };
  }
  async function removeLiquidity(pos: Position) {
    const wallet = wallets.current;
    if (wallet === null) return;
    const out = preview(pos);
    const da = decimalsOf(pos.asset_a_id);
    const db = decimalsOf(pos.asset_b_id);
    const r = await ask({
      title: t('Take liquidity out'),
      message: t('Take %1% of your shares of the %2 / %3 pool out?', String(percent), nameOf(pos.asset_a_id, pos.asset_a), nameOf(pos.asset_b_id, pos.asset_b)),
      informative: out ? t('About %1 %2 and %3 %4, at least %5% less should the pool move first. CHN comes by the coinbase of the next block; assets as coins of the transaction.', formatUnits(out.a, da), nameOf(pos.asset_a_id, pos.asset_a), formatUnits(out.b, db), nameOf(pos.asset_b_id, pos.asset_b), String(slippage)) : undefined,
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Take out', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string; amount_a: Amount; amount_b: Amount }>(wallet, 'removeliquidity', [assetArg(pos.asset_a_id), assetArg(pos.asset_b_id), percent, slippage]);
      if (!done) return;
      removing = null;
      toast('success', t('Liquidity taken out'), t('About %1 %2 and %3 %4, with the next block.', formatAmount(done.amount_a, da), pos.asset_a, formatAmount(done.amount_b, db), pos.asset_b));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Take liquidity out'), errorText(e), 'error');
    }
  }
  function addMore(pos: Position) {
    tradeIntent.a = pos.asset_a_id;
    tradeIntent.b = pos.asset_b_id;
    go('trade', 'liquidity');
  }

  /** Read more of the activity (it shows the newest ACTIVITY_PAGE at first: audit port-2 #13). */
  function showMoreActivity() {
    baView.activity = Math.min(MAX_ACTIVITY, baView.activity + ACTIVITY_PAGE);
    void refreshBitAssets();
  }

  // --- Auctions of the wallet (receipts) ---
  const receipts = $derived((mine?.receipts ?? []).map((r) => ({ id: r.auction, a: auctionById(r.auction) ?? null })));
  async function collect(a: Auction) {
    const wallet = wallets.current;
    if (wallet === null) return;
    const action = receiptAction(a);
    const r = await ask({
      title: action === 'cancel' ? t('Cancel the auction') : t('Collect the auction'),
      message:
        action === 'cancel'
          ? t('Cancel the auction of %1 %2? Nobody bid yet: everything comes back.', formatAmount(a.remaining, decimalsOf(a.base_id)), nameOf(a.base_id, a.base))
          : t('Collect %1 %2 and the %3 %4 left?', formatAmount(a.proceeds, decimalsOf(a.quote_id)), nameOf(a.quote_id, a.quote), formatAmount(a.remaining, decimalsOf(a.base_id)), nameOf(a.base_id, a.base)),
      informative: t('CHN comes by the coinbase of the next block; assets as coins of the transaction.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Back' },
        { id: 'yes', label: action === 'cancel' ? 'Cancel it' : 'Collect', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'collectauction', [a.auction]);
      if (!done) return;
      toast('success', t('Collecting an auction'), t('With the next block: CHN by its coinbase, assets as coins.'));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Collect the auction'), errorText(e), 'error');
    }
  }

</script>

{#if noWallet}
  <NoWallet page={PAGES['my-assets']} />
{:else}
  <div class="page ba-page my-assets">
    <header class="page-head">
      <h1>My assets</h1>
      <div class="row">
        <button type="button" class="btn small" onclick={() => go('issue')}><Icon name="issue" size={15} /> {simple ? 'Create a token' : 'Register new asset'}</button>
        {#if simple}<button type="button" class="btn small" onclick={() => go('trade', 'swap')}><Icon name="swap" size={15} /> Swap</button>{/if}
        <button type="button" class="btn small ghost" onclick={() => go('receive')} title="Assets are received at any address of this wallet"><Icon name="receive" size={15} /> Receive</button>
      </div>
    </header>

    {#if ba.mineState === 'locked'}
      <div class="panel locked">
        <Icon name="lock" size={22} />
        <p>This wallet is locked. It needs its passphrase to tell its reservations, so it shows its assets once unlocked for that alone.{#if mine} What is shown is as of the last time.{/if}</p>
        <button type="button" class="btn primary" onclick={() => void unlockHoldings()}><Icon name="unlock" size={15} /> Show my assets</button>
      </div>
    {:else if ba.mineState === 'error'}
      <p class="ba-problems bad" role="alert"><SafeText text={ba.mineError} /></p>
    {/if}

    <section class="ba-figures">
      <div class="panel ba-figure"><span class="muted">Assets held</span><strong>{holdings.filter((h) => approx(h.balance) > 0).length}</strong><span class="faint">{tn('%n controlled', holdings.filter((h) => h.control).length)}</span></div>
      <div class="panel ba-figure"><span class="muted">Worth</span><strong>{formatPrice(worth)} CHN</strong><span class="faint">{priced} of {holdings.length} with a market</span></div>
      {#if simple}
        <div class="panel ba-figure"><span class="muted">Being created</span><strong>{issuingCount}</strong><span class="faint">in your assets once registered</span></div>
      {:else}
        <div class="panel ba-figure"><span class="muted">Pool shares</span><strong>{mine?.liquidity.length ?? 0}</strong><span class="faint">earning 0.3% of each trade</span></div>
        <div class="panel ba-figure"><span class="muted">Being issued</span><strong>{issuingCount}</strong><span class="faint">{tn('%n auction(s) of yours to follow', receipts.length)}</span></div>
      {/if}
    </section>

    <TabRow label="My assets" {tabs} current={tab} select={(id) => (nav.tab = id)} />

    {#if tab === 'register'}
      <Issue embedded />
    {:else if tab === 'holdings'}
      <HiddenToggle count={visible.hidden} bind:show={showHidden} />
      {#if holdings.length === 0}
        <div class="panel ba-empty">
          {#if ba.mineState === 'loading'}Loading…{:else if simple}Nothing yet: swap CHN for a token, create one of your own, or have one sent to an address of this wallet.{:else}Nothing yet: register a new asset, buy one in Trade, or have one sent to an address of this wallet.{/if}
          <div class="row center"><button type="button" class="btn primary" onclick={() => go('issue')}>{simple ? 'Create a token' : 'Register new asset'}</button><button type="button" class="btn" onclick={() => go('trade', 'swap')}>{simple ? 'Swap' : 'Trade'}</button></div>
        </div>
      {:else}
        <div class="ba-split">
          <section class="panel">
            <ul class="ba-list" aria-label="Holdings">
              {#each holdings as h (h.asset)}
                {@const price = chnPrice(h.asset)}
                <li>
                  <button type="button" class="ba-row" aria-current={picked === h.asset} onclick={() => (picked = h.asset)}>
                    <AssetBadge id={h.asset} label={h.label} showLabel={false} />
                    <span class="main">
                      <SafeText text={h.label} />
                      <span class="faint small line">
                        {#if h.control}<span class="ba-tag accent">controlled</span>{/if}
                        <span>{approx(h.supply) > 0 ? formatPercent((approx(h.balance) / approx(h.supply)) * 100) : '–'} of the supply</span>
                      </span>
                    </span>
                    <span class="side">
                      <strong class="num">{formatAmount(h.balance, h.decimals, { max: 6 })}</strong>
                      {#if approx(h.pending) > 0}<span class="ba-warn small">+{formatAmount(h.pending, h.decimals, { max: 6 })} waiting</span>{:else if price !== undefined}<span class="faint small">≈ {formatPrice(price * approx(h.balance))} CHN</span>{/if}
                    </span>
                  </button>
                </li>
              {/each}
            </ul>
          </section>
          {#if holding}
            {#key holding.asset}<ManagePanel {holding} />{/key}
          {/if}
        </div>
      {/if}
    {:else if tab === 'issuing'}
      {#if issuingCount === 0}
        <div class="panel ba-empty">No asset is being registered.<div class="row center"><button type="button" class="btn primary" onclick={() => go('issue')}>Register new asset</button></div></div>
      {:else}
        <div class="ba-cols">
          {#each pending as p (p.name)}
            {@const stage = stageOf(p)}
            <section class="panel issuing">
              <header class="issue-head">
                <AssetBadge id={p.asset} label={p.name} />
                <span class="muted small">{p.supply} · {p.decimals} decimals{p.public ? '' : ` · ${t('private name')}`}</span>
              </header>
              <!-- Its progress (components/PendingCard.svelte): reserved, registering by itself in so long, registered;
                   the user's word when the app needs it (after a restart, or a locked wallet). -->
              <PendingCard op={pendingOpOf(p)} />
              {#if stage === 'waiting' && !simple}<p class="ba-help">{tn('Registered by itself in %n block(s), once the reservation is deep enough that whoever makes a block cannot take the name first.', waitOf(p.name))}</p>{/if}
              <div class="row">
                {#if stage === 'registering' && !p.registration && p.needsConfirm === undefined && !p.error}<button type="button" class="btn small" onclick={() => register(p)}>Register now</button>{/if}
                {#if stage !== 'registered' && !p.registration}<button type="button" class="btn small danger" onclick={() => release(p.name)}>Release</button>{/if}
                {#if stage === 'registered'}<button type="button" class="btn small" onclick={() => wallets.current !== null && dropPending(wallets.current, p.name)}>Dismiss</button>{/if}
              </div>
            </section>
          {/each}
          {#each strays as r (r.txid)}
            <section class="panel issuing">
              <header class="issue-head">
                {#if r.name}<AssetBadge id={r.txid} label={r.name} />{:else}<span class="muted">A reservation <span class="ba-mono" translate="no">{shortHash(r.txid, 8)}</span></span>{/if}
                {#if r.height !== undefined}<span class="faint small">reserved in block {formatCount(r.height)}</span>{/if}
              </header>
              {#if r.taken}
                <p class="ba-problems bad">Someone registered this name first. The reservation can only be released.</p>
              {:else if !r.name}
                <p class="ba-help">The name of this reservation is not known to this wallet: it can only be released by whoever knows it.</p>
              {:else if (r.wait ?? 0) > 0}
                <p class="ba-help">{tn('It can be registered in %n block(s).', r.wait ?? 0)} This app did not make it: register it with its supply once it can be.</p>
              {:else if strayName === r.name}
                <form class="ba-form" onsubmit={(e) => { e.preventDefault(); void registerStray(r); }}>
                  <UnitsInput id="stray-supply" label="Initial supply" decimals={strayDecimals} allowZero bind:units={straySupply} bind:text={straySupplyText} hint="0: none yet; mint later" />
                  <div class="field"><label for="stray-decimals">Decimals</label><input id="stray-decimals" type="number" min="0" max={MAX_DECIMALS} bind:value={strayDecimals} /></div>
                  <div class="field"><label for="stray-info">Description</label><input id="stray-info" type="text" maxlength="512" bind:value={strayInfo} /></div>
                  <label class="check"><input type="checkbox" bind:checked={strayPublic} /> <span>Publish the name, so that it is listed and found by name</span></label>
                  <div class="row"><button type="submit" class="btn primary small" disabled={straySupply === null}>Register</button><button type="button" class="btn small" onclick={() => (strayName = null)}>Cancel</button></div>
                </form>
              {:else}
                <p class="ba-help">Deep enough: it can be registered now. This app did not make it, so it asks for its supply.</p>
              {/if}
              <div class="row">
                {#if r.name && !r.taken && (r.wait ?? 0) === 0 && strayName !== r.name}<button type="button" class="btn primary small" onclick={() => (strayName = r.name ?? null)}>Register…</button>{/if}
                {#if r.name}<button type="button" class="btn small danger" onclick={() => release(r.name ?? '')}>Release</button>{/if}
              </div>
            </section>
          {/each}
        </div>
      {/if}
    {:else if tab === 'liquidity'}
      {#if (mine?.liquidity.length ?? 0) === 0}
        <div class="panel ba-empty">No pool shares. Put two assets in their pool to earn 0.3% of every trade.<div class="row center"><button type="button" class="btn primary" onclick={() => go('trade', 'liquidity')}>Add liquidity</button></div></div>
      {:else}
        <div class="ba-cols">
          {#each mine?.liquidity ?? [] as pos (pos.pool)}
            {@const out = removing === pos.pool ? preview(pos) : null}
            <section class="panel position">
              <header class="issue-head">
                <span class="pair"><AssetBadge id={pos.asset_a_id} label={pos.asset_a} size="s" /> <span class="faint">/</span> <AssetBadge id={pos.asset_b_id} label={pos.asset_b} size="s" /></span>
                <span class="ba-tag accent">{formatPercent(pos.percent)}</span>
              </header>
              <div class="ba-bar"><span use:barWidth={pos.percent / 100}></span></div>
              <dl class="ba-lines">
                <dt>Shares</dt><dd>{formatAmount(pos.shares, 0)}</dd>
                <dt>Worth now</dt><dd>{formatAmount(pos.value_a, decimalsOf(pos.asset_a_id))} <SafeText text={pos.asset_a} /></dd>
                <dt></dt><dd>{formatAmount(pos.value_b, decimalsOf(pos.asset_b_id))} <SafeText text={pos.asset_b} /></dd>
              </dl>
              {#if removing === pos.pool}
                <div class="ba-form">
                  <div class="field">
                    <label for="remove-percent">Take out {percent}%</label>
                    <input id="remove-percent" type="range" min="1" max="100" step="1" bind:value={percent} />
                    <div class="ba-chips">{#each [25, 50, 75, 100] as p (p)}<button type="button" class="ba-chip" aria-pressed={percent === p} onclick={() => (percent = p)}>{p}%</button>{/each}</div>
                  </div>
                  <div class="field"><label for="remove-slippage">Slippage</label><select id="remove-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select></div>
                  {#if out}
                    <dl class="ba-lines">
                      <dt>You get about</dt><dd>{formatUnits(out.a, decimalsOf(pos.asset_a_id))} <SafeText text={pos.asset_a} /></dd>
                      <dt></dt><dd>{formatUnits(out.b, decimalsOf(pos.asset_b_id))} <SafeText text={pos.asset_b} /></dd>
                    </dl>
                    {#if out.a === 0n || out.b === 0n}<p class="ba-help">A side that rounds to nothing (or to less CHN than the least paid out, 0.0005) stays in the pool.</p>{/if}
                  {/if}
                  <div class="row"><button type="button" class="btn primary small" onclick={() => removeLiquidity(pos)}>Take out</button><button type="button" class="btn small" onclick={() => (removing = null)}>Cancel</button></div>
                </div>
              {:else}
                <div class="row">
                  <button type="button" class="btn small" onclick={() => { removing = pos.pool; percent = 100; }}>Take out…</button>
                  <button type="button" class="btn small ghost" onclick={() => addMore(pos)}>Add more</button>
                </div>
              {/if}
            </section>
          {/each}
        </div>
      {/if}
    {:else if tab === 'auctions'}
      {#if receipts.length === 0}
        <div class="panel ba-empty">No auction of this wallet. Sell an amount of an asset at a price that falls block by block.<div class="row center"><button type="button" class="btn primary" onclick={() => go('auctions', 'sell')}>Sell by auction</button></div></div>
      {:else}
        <div class="ba-cols">
          {#each receipts as r (r.id)}
            <section class="panel position">
              {#if !r.a}
                <p class="ba-help">Auction <span class="ba-mono" translate="no">{shortHash(r.id, 8)}</span></p>
              {:else}
                {@const a = r.a}
                {@const action = receiptAction(a)}
                <header class="issue-head">
                  <span><strong>{formatAmount(a.amount, decimalsOf(a.base_id))}</strong> <SafeText text={a.base} /> <span class="faint">for</span> <SafeText text={a.quote} /></span>
                  <span class="ba-tag" class:accent={a.status === 'open'}>{t(AUCTION_STATUS[a.status] ?? 'Ended')}</span>
                </header>
                <div class="ba-bar" title={t('Sold')}><span use:barWidth={approx(a.amount) ? 1 - approx(a.remaining) / approx(a.amount) : 0}></span></div>
                <dl class="ba-lines">
                  <dt>Left</dt><dd>{formatAmount(a.remaining, decimalsOf(a.base_id))} <SafeText text={a.base} /></dd>
                  <dt>Brought in</dt><dd>{formatAmount(a.proceeds, decimalsOf(a.quote_id))} <SafeText text={a.quote} /></dd>
                  <dt>Bids</dt><dd>{a.bids}</dd>
                  <dt>Blocks</dt><dd>{formatCount(a.start_height)} → {formatCount(a.end_height)}</dd>
                </dl>
                <div class="row">
                  {#if action === 'collect'}<button type="button" class="btn primary small" onclick={() => collect(a)}>Collect</button>
                  {:else if action === 'cancel'}<button type="button" class="btn small" onclick={() => collect(a)}>Cancel it: take everything back</button>
                  {:else if action === 'wait'}<span class="faint small">It can be collected once it ends (after block {formatCount(a.end_height)}) or sells out.</span>{/if}
                  <button type="button" class="btn small ghost" onclick={() => go('auctions')}>View</button>
                </div>
              {/if}
            </section>
          {/each}
        </div>
      {/if}
    {:else}
      <section class="panel">
        <HiddenToggle count={ba.activity.length - (showHidden ? ba.activity.filter((a) => !chainTxHidden(a.txid)).length : activity.length)} bind:show={showHidden} />
        {#if activity.length === 0}
          <p class="ba-empty">Nothing yet.</p>
        {:else}
          <div class="ba-wrap">
            <table class="ba-table" use:stacked aria-label="Activity">
              <thead><tr><th>When</th><th>What</th><th>Details</th><th class="end">Confirmations</th><th>Transaction</th></tr></thead>
              <tbody>
                {#each activity as a (a.txid)}
                  <tr>
                    <td class="num">{formatTime(a.time)}</td>
                    <td>{t(OPERATIONS[a.operation] ?? 'Other')}</td>
                    <td><SafeText text={a.summary} />{#if assetMovesOf(a.txid).length}<div class="activity-moves"><LocalTxCell txid={a.txid} full /></div>{/if}</td>
                    <td class="end">{#if a.confirmations === 0}<span class="ba-warn">waiting</span>{:else}{formatCount(a.confirmations)}{/if}</td>
                    <td class="ba-mono"><span translate="no">{shortHash(a.txid, 6)}</span> <CopyButton text={a.txid} label="Copy the transaction" /></td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
          {#if ba.activity.length >= baView.activity && baView.activity < MAX_ACTIVITY}
            <p class="more"><button type="button" class="btn small" onclick={showMoreActivity}>{t('Show older activity')}</button></p>
          {/if}
        {/if}
      </section>
    {/if}
  </div>
{/if}

<style>
  .activity-moves {
    margin-top: 0.25rem;
  }
  .more {
    display: flex;
    justify-content: center;
    margin: 0.6rem 0;
  }
  .locked {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.8rem;
    padding: 0.9rem 1.1rem;
    border-color: var(--warning);
  }
  .locked p {
    flex: 1 1 18rem;
    margin: 0;
  }
  .line {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .center {
    justify-content: center;
    margin-top: 0.8rem;
  }
  .small {
    font-size: 0.85rem;
  }
  .issuing,
  .position {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 1rem 1.1rem;
  }
  .issue-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
</style>

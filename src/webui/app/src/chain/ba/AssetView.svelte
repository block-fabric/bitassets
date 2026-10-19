<script lang="ts">
  // An asset, whole: its supply (minted, burned), what the wallet holds of it, its markets (each pool with its
  // price, depth and the price as this app saw it), its auctions, its data and the history of each field, who
  // controls it, and retiring it once it is dead. The Explore panel of the Qt window of BitAssets, with more.
  import { stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import { errorText, inform, ask, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount, shortHash } from '../../lib/format';
  import { go, goTo } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import Chart from './Chart.svelte';
  import { approx, assetArg, AUCTION_STATUS, auctionProgress, DATA_FIELDS, formatAmount, formatPercent, formatPrice, isChn, unitPrice, type Asset, type AssetHistory, type Auction, type DataField, type Pool } from './model';
  import { ba, chnPrice, nameOf, priceSeries, refreshBitAssets, sellIntent, sendAsset, tradeIntent } from './store.svelte';
  import { ignoreAsset, ignoreName, unignoreAsset, unignoreName } from './hide.svelte';
  import { ignoredAsset } from './visible.svelte';

  let { id, onback }: { id: string; onback?: () => void } = $props();

  let asset = $state<Asset | null>(null);
  let history = $state<AssetHistory>({});
  let pools = $state<Pool[]>([]);
  let error = $state('');
  let loadedAt = '';

  const FIELD_NAMES: Record<DataField, string> = {
    info: 'Description',
    commitment: 'Commitment',
    ipv4: 'IPv4 address',
    ipv6: 'IPv6 address',
    encryptionkey: 'Encryption key',
    signingkey: 'Signing key',
  };

  // Asked again when the asset or the height changes.
  $effect(() => {
    const key = `${id}@${ba.height}`;
    if (key === loadedAt) return;
    loadedAt = key;
    const arg = assetArg(id);
    // Its pools and auctions asked of the node by asset: the lists of the store are pages (audit 7 E-BA3).
    Promise.all([
      rpc.raw<Asset>('getasset', [arg]),
      rpc.raw<AssetHistory>('getassethistory', [arg]).catch(() => ({})),
      rpc.raw<Pool[]>('listpools', [arg, 1000, 0]).catch(() => []),
      rpc.raw<Auction[]>('listauctions', [false, 100, 0, arg]).catch(() => null),
    ])
      .then(([a, h, p, au]) => {
        if (loadedAt !== key) return;
        asset = a;
        history = h ?? {};
        pools = p ?? [];
        ownAuctions = au;
        error = '';
      })
      .catch((e) => {
        if (loadedAt !== key) return;
        asset = null;
        error = errorText(e);
      });
  });

  const holding = $derived(ba.mine?.assets.find((a) => a.asset === id) ?? null);
  const controlled = $derived(!!holding?.control);
  const price = $derived(chnPrice(id));
  let ownAuctions = $state<Auction[] | null>(null);
  const auctions = $derived(ownAuctions ?? ba.auctions.filter((a) => a.base_id === id || a.quote_id === id));
  const chnPool = $derived(pools.find((p) => isChn(p.asset_b_id) || isChn(p.asset_a_id)));
  const series = $derived(chnPool ? priceSeries(chnPool, id).map((p) => ({ x: p.h, y: p.p })) : []);
  const supplyUnits = $derived(asset ? asset.minted : 0);
  const burnedShare = $derived(asset && approx(asset.minted) > 0 ? approx(asset.burned) / approx(asset.minted) : 0);
  const heldShare = $derived(asset && holding && approx(asset.supply) > 0 ? approx(holding.balance) / approx(asset.supply) : 0);
  /** The changes of each field, newest first; the registration's empty stamps left out (as chains-qt left them out). */
  const changes = $derived(
    DATA_FIELDS.flatMap((field) => (history[field] ?? []).filter((e, i) => !(i === 0 && e.value === null)).map((e) => ({ field, ...e }))).sort((a, b) => b.height - a.height),
  );
  const fields = $derived(DATA_FIELDS.filter((f) => asset?.data?.[f] !== undefined));

  function trade(other: string) {
    tradeIntent.pay = other;
    tradeIntent.get = id;
    go('trade', 'swap');
  }

  function makePool() {
    tradeIntent.a = id;
    tradeIntent.b = '0'.repeat(64);
    go('trade', 'liquidity');
  }

  function sell() {
    sellIntent.asset = id;
    go('auctions', 'sell');
  }

  function send() {
    sendAsset(id);
    go('send');
  }

  async function retire() {
    const wallet = wallets.current;
    if (!asset || wallet === null) return;
    const r = await ask({
      title: t('Retire %1', asset.label),
      message: t('Retire %1 for good? It and its pools are deleted and its name is free again; the %2 CHN its pools hold go to mainchain miners, as a fee.', nameOf(asset.asset, asset.label), formatAmount(asset.release_fee ?? 0, 8)),
      informative: t('Anyone may retire a dead asset; this wallet pays the fee of the transaction.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Retire', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'releaseasset', [assetArg(id)]);
      if (!done) return;
      toast('success', t('Retiring an asset'), t('%1 is retired with the next block.', asset.label));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Retire'), errorText(e), 'error');
    }
  }
</script>

<div class="asset-view">
  {#if onback}
    <button type="button" class="btn ghost small ba-back" onclick={onback}><Icon name="back" size={15} /> All assets</button>
  {/if}

  {#if error}
    <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
  {:else if !asset}
    <p class="ba-empty">Loading…</p>
  {:else}
    <header class="panel hero">
      <AssetBadge {id} label={asset.label} size="l" showLabel={false} logo={asset.logo?.hash} />
      <div class="who">
        <h2 class="name"><SafeText text={asset.label} /></h2>
        <div class="tags">
          <span class="ba-tag" translate="no">#{asset.seq}</span>
          {#if asset.fixed}<span class="ba-tag accent">Fixed supply</span>{:else}<span class="ba-tag">Mintable</span>{/if}
          {#if !asset.name}<span class="ba-tag warn" title="Its registration did not publish the name: it is known by its hash and number">Private name</span>{/if}
          {#if controlled}<span class="ba-tag accent"><Icon name="crown" size={12} /> You control it</span>{/if}
          {#if asset.releasable}<span class="ba-tag danger">Dead</span>{/if}
        </div>
      </div>
      <div class="hero-actions">
        <button type="button" class="btn primary" onclick={() => trade('0'.repeat(64))}><Icon name="swap" size={15} /> Trade</button>
        {#if holding && approx(holding.balance) > 0}<button type="button" class="btn" onclick={send}><Icon name="send" size={15} /> Send</button>{/if}
        {#if holding && approx(holding.balance) > 0}<button type="button" class="btn" onclick={sell}><Icon name="gavel" size={15} /> Sell by auction</button>{/if}
        {#if controlled}<button type="button" class="btn" onclick={() => goTo('my-assets', '', id)}><Icon name="crown" size={15} /> Manage</button>{/if}
        {#if controlled && !asset.fixed && (ba.info?.logo_height ?? -1) >= 0}<button type="button" class="btn" onclick={() => goTo('my-assets', '', id)}><Icon name="picture" size={15} /> {asset.logo ? 'Change logo' : 'Add a logo'}</button>{/if}
        <!-- Hide it in this wallet's lists (display only: the coins stay), or every asset of its name (ba/hide.svelte.ts). -->
        {#if wallets.current !== null}
          {#if ignoredAsset(id, asset.label)}
            <button type="button" class="btn ghost" onclick={() => { unignoreAsset(id); unignoreName(asset?.label ?? ''); }}><Icon name="eye" size={15} /> Unignore</button>
          {:else}
            <button type="button" class="btn ghost" onclick={() => ignoreAsset(id)}><Icon name="eyeoff" size={15} /> Ignore this asset</button>
            <button type="button" class="btn ghost" onclick={() => ignoreName(asset?.label ?? '')}>Ignore every asset named <SafeText text={asset.label} /></button>
          {/if}
        {/if}
      </div>
    </header>

    <section class="ba-figures">
      <div class="panel ba-figure"><span class="muted">Supply</span><strong>{formatAmount(asset.supply, asset.decimals)}</strong><span class="faint">{asset.decimals} decimals</span></div>
      <div class="panel ba-figure"><span class="muted">Price</span><strong>{price !== undefined ? formatPrice(price) : '–'}</strong><span class="faint">{price !== undefined ? 'CHN, in its pool' : 'No pool with CHN'}</span></div>
      <div class="panel ba-figure"><span class="muted">Value of the supply</span><strong>{price !== undefined ? formatPrice(price * approx(asset.supply)) : '–'}</strong><span class="faint">CHN at that price</span></div>
      <div class="panel ba-figure"><span class="muted">You hold</span><strong>{holding ? formatAmount(holding.balance, asset.decimals) : '0'}</strong><span class="faint">{heldShare ? formatPercent(heldShare * 100) : '–'} of the supply</span></div>
    </section>

    <div class="ba-split">
      <div class="col">
        <section class="panel">
          <div class="panel-head"><h3>Supply</h3><span class="muted small">registered in block {formatCount(asset.registered)}</span></div>
          <div class="panel-body stack">
            <div class="ba-bar supply" title={t('%1 burned of %2 minted', formatAmount(asset.burned, asset.decimals), formatAmount(supplyUnits, asset.decimals))}>
              <span use:barWidth={1 - burnedShare}></span>
            </div>
            <dl class="ba-lines">
              <dt>Minted, all told</dt><dd>{formatAmount(asset.minted, asset.decimals)}</dd>
              <dt>Burned</dt><dd>{formatAmount(asset.burned, asset.decimals)}</dd>
              <dt>In circulation</dt><dd class="strong">{formatAmount(asset.supply, asset.decimals)}</dd>
              <dt>Smallest amount</dt><dd>{asset.decimals === 0 ? '1' : `0.${'0'.repeat(asset.decimals - 1)}1`}</dd>
            </dl>
            <p class="ba-help">
              {#if asset.fixed}Its control coin was burned: nobody can mint more of it or change its data, ever.{:else}Whoever holds its control coin can mint more and change its data.{/if}
              The chain lists no holders: only the holder of the control coin is known.
            </p>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head"><h3>Markets</h3>{#if pools.length === 0}<button type="button" class="btn small" onclick={makePool}><Icon name="plus" size={14} /> Make a pool</button>{/if}</div>
          {#if pools.length === 0}
            <p class="ba-empty">No pool trades it yet. Make one with CHN, and set its first price.</p>
          {:else}
            <ul class="ba-list">
              {#each pools as p (p.pool)}
                {@const other = p.asset_a_id === id ? p.asset_b_id : p.asset_a_id}
                {@const up = unitPrice(p.asset_a_id === id ? p.price : p.price ? 1 / p.price : undefined)}
                <li>
                  <button type="button" class="ba-row" onclick={() => trade(other)}>
                    <AssetBadge id={other} label={p.asset_a_id === id ? p.asset_b : p.asset_a} size="s" showLabel={false} />
                    <span class="main">
                      <span>1 <SafeText text={asset.label} /> = {#if up.kind === 'each'}{up.price}{:else if up.kind === 'buys'}1/{up.amount}{:else}–{/if} <SafeText text={p.asset_a_id === id ? p.asset_b : p.asset_a} /></span>
                      <span class="faint small">
                        {#if p.abandoned}No liquidity: closed until someone adds some.{:else}{formatAmount(p.asset_a_id === id ? p.reserve_a : p.reserve_b, asset.decimals)} <SafeText text={asset.label} /> · {formatCount(p.swaps)} trades{/if}
                      </span>
                    </span>
                    <span class="side"><span class="btn small ghost">Trade</span></span>
                  </button>
                </li>
              {/each}
            </ul>
          {/if}
        </section>

        {#if chnPool && !chnPool.abandoned}
          <section class="panel">
            <div class="panel-head"><h3>Price in CHN</h3><span class="muted small">as this app saw it, block by block</span></div>
            <div class="panel-body">
              {#if series.length < 2}
                <p class="ba-help">The chain keeps no history of prices: the chart grows from the first trades this app sees.</p>
              {/if}
              <Chart points={series.length ? series : [{ x: ba.height, y: price ?? 0 }]} step label={t('The price of %1 in CHN', asset.label)} formatX={(x) => t('block %1', String(Math.round(x)))} />
            </div>
          </section>
        {/if}
      </div>

      <div class="col">
        <section class="panel">
          <div class="panel-head"><h3>About it</h3></div>
          <div class="panel-body stack">
            {#if fields.length === 0}<p class="ba-help">Its issuer gave no data.</p>{/if}
            {#if asset.data?.info}<p class="info"><SafeText text={asset.data.info} /></p>{/if}
            <dl class="facts">
              {#each fields.filter((f) => f !== 'info') as f (f)}
                <dt>{t(FIELD_NAMES[f])}</dt>
                <dd class="ba-mono"><span translate="no">{asset.data?.[f]}</span> <CopyButton text={asset.data?.[f] ?? ''} label="Copy" /></dd>
              {/each}
              <dt>Hash</dt><dd class="ba-mono"><span translate="no">{shortHash(asset.asset, 12)}</span> <CopyButton text={asset.asset} label="Copy the hash" /></dd>
              <dt>Number</dt><dd><span translate="no">{asset.seq}</span></dd>
              {#if asset.logo}<dt>Logo</dt><dd class="ba-mono"><span translate="no">{asset.logo.mime === 'image/png' ? 'PNG' : 'JPEG'}, {(asset.logo.size / 1024).toFixed(1)} KB, {shortHash(asset.logo.hash, 10)}</span> <CopyButton text={asset.logo.hash} label="Copy the hash of the logo" /> <span class="faint">block {asset.logo.height}</span></dd>{/if}
              <dt>Registration</dt><dd class="ba-mono"><span translate="no">{shortHash(asset.registration, 10)}</span> <CopyButton text={asset.registration} label="Copy the transaction" /></dd>
              {#if asset.control?.address}<dt>Controlled by</dt><dd class="ba-mono"><span translate="no">{asset.control.address}</span> <CopyButton text={asset.control.address} label="Copy the address" /></dd>{/if}
            </dl>
          </div>
        </section>

        {#if auctions.length}
          <section class="panel">
            <div class="panel-head"><h3>Auctions</h3></div>
            <ul class="ba-list">
              {#each auctions as a (a.auction)}
                {@const prog = auctionProgress(a, ba.height)}
                <li>
                  <button type="button" class="ba-row" onclick={() => goTo('auctions', '', a.auction)}>
                    <Icon name="gavel" size={18} />
                    <span class="main">
                      <span>{formatAmount(a.remaining, a.base_id === id ? asset.decimals : 8)} <SafeText text={a.base} /> for <SafeText text={a.quote} /></span>
                      <span class="ba-bar"><span use:barWidth={prog.sold}></span></span>
                    </span>
                    <span class="side"><span class="small">{t(AUCTION_STATUS[a.status] ?? 'Ended')}</span></span>
                  </button>
                </li>
              {/each}
            </ul>
          </section>
        {/if}

        <section class="panel">
          <div class="panel-head"><h3>History of its data</h3></div>
          {#if changes.length === 0}
            <p class="ba-empty">Its data never changed since its registration.</p>
          {:else}
            <div class="ba-wrap">
              <table class="ba-table" use:stacked aria-label="History of its data">
                <thead><tr><th>Block</th><th>Field</th><th>Value</th></tr></thead>
                <tbody>
                  {#each changes as c (c.field + c.txid)}
                    <tr>
                      <td class="num">{formatCount(c.height)}</td>
                      <td>{t(FIELD_NAMES[c.field])}</td>
                      <td>{#if c.value === null}<span class="faint">(deleted)</span>{:else}<SafeText class="ba-mono" text={c.value} />{/if}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          {/if}
        </section>

        {#if asset.releasable}
          <section class="panel dead">
            <div class="panel-head"><h3>This asset is dead</h3></div>
            <div class="panel-body stack">
              <p class="ba-help">Its supply is fixed, nobody holds any of it, and its pools have no liquidity providers. Retiring it deletes it and its pools and frees its name; the {formatAmount(asset.release_fee ?? 0, 8)} CHN its pools hold go to mainchain miners, as a fee. Anyone may do it.</p>
              <div><button type="button" class="btn danger" disabled={wallets.current === null} onclick={retire}><Icon name="flame" size={15} /> Retire it</button></div>
            </div>
          </section>
        {/if}
      </div>
    </div>
  {/if}
</div>

<style>
  .asset-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .hero {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 1rem;
    padding: 1.1rem 1.2rem;
  }
  .who {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    flex: 1 1 14rem;
    min-width: 0;
  }
  .name {
    margin: 0;
    font-size: 1.7rem;
    overflow-wrap: anywhere;
  }
  .tags {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .hero-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .info {
    margin: 0;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .facts dd {
    display: flex;
    align-items: center;
    gap: 0.25rem;
  }
  .supply > span {
    background: var(--accent);
  }
  .supply {
    background: var(--danger-soft);
  }
  .dead {
    border-color: var(--negative);
  }
  .small {
    font-size: 0.85rem;
  }
</style>

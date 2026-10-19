<script lang="ts">
  // An asset of the wallet: what it holds, and what it can do with it. Any holder sends, trades, sells by
  // auction and burns; the holder of the control coin also mints more, changes the data (every field of it),
  // gives the control coin away, or burns it to fix the supply for good. The panel of an asset of My assets in
  // the Qt window of BitAssets, with every field of the data (the Qt window had the description and the
  // commitment).
  import SafeText from '../../components/SafeText.svelte';
  import AddressInput from '../../components/AddressInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import AssetName from './AssetName.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import LogoPicker, { type PickedLogo } from './LogoPicker.svelte';
  import { amountParam, approx, assetArg, DATA_FIELDS, dataProblems, dataUpdates, displayNameProblem, MAX_DISPLAY_NAME_BYTES, normalizeDisplayName, formatAmount, formatUnits, unitsAtMost, unitsOf, type Asset, type DataField, type Holding } from './model';
  import { ba, nameOf, refreshBitAssets, sellIntent, sendAsset, tickersActive, tradeIntent } from './store.svelte';

  let { holding }: { holding: Holding } = $props();

  const id = $derived(holding.asset);
  const arg = $derived(assetArg(holding.asset));
  const decimals = $derived(holding.decimals);
  const balance = $derived(unitsAtMost(holding.balance, holding.decimals));
  // While the control coin moves (a mint, a change, waiting for a block), what it does waits too (as chains-qt).
  const control = $derived(holding.control && !holding.control_pending);

  let asset = $state<Asset | null>(null);
  $effect(() => {
    const a = arg;
    void holding.supply;
    // Asked again with each refresh of the store (a logo or data changed in a block; the holding may be the same).
    void ba.version;
    rpc
      .raw<Asset>('getasset', [a])
      .then((r) => {
        if (a === arg) asset = r;
      })
      .catch(() => undefined);
  });

  type Action = 'mint' | 'data' | 'logo' | 'display' | 'control' | 'burn' | '';
  let open = $state<Action>('');
  let busy = $state(false);

  // Mint.
  let mintUnits = $state<bigint | null>(null);
  let mintText = $state('');
  let mintTo = $state('');
  let mintToValid = $state(false);
  // The data.
  let fields = $state<Record<DataField, string>>({ info: '', commitment: '', ipv4: '', ipv6: '', encryptionkey: '', signingkey: '' });
  // Filled when the form opens (or the asset read changes to another one), not each time the asset is read
  // again: a refresh (a block, the supply after a mint) never wipes what the user is typing.
  let fieldsFor = '';
  $effect(() => {
    if (open !== 'data') {
      fieldsFor = '';
      return;
    }
    if (!asset || fieldsFor === asset.asset) return;
    fieldsFor = asset.asset;
    const data = asset.data ?? {};
    fields = { info: data.info ?? '', commitment: data.commitment ?? '', ipv4: data.ipv4 ?? '', ipv6: data.ipv6 ?? '', encryptionkey: data.encryptionkey ?? '', signingkey: data.signingkey ?? '' };
  });
  const problems = $derived(dataProblems(fields));
  const updates = $derived(dataUpdates(asset?.data, fields));
  // The logo: set (in place of the one before) or removed, by the control coin; shown in the host's confirmation.
  let logo = $state<PickedLogo | null>(null);
  const logosActive = $derived((ba.info?.logo_height ?? -1) >= 0 && (ba.info?.logo_height ?? 0) <= ba.height);
  async function saveLogo(remove: boolean) {
    if (!remove && !logo) return;
    await run('setassetlogo', [arg, remove ? '' : (logo?.base64 ?? '')], () => {
      toast('success', remove ? t('Logo removed') : t('Logo set'), t('The logo of %1 changes with the next block.', holding.label));
      logo = null;
    });
  }
  // The display name (from ticker_height): set, changed or removed by the control coin; not unique.
  const tickers = $derived(tickersActive());
  let displayText = $state('');
  let displayFor = '';
  $effect(() => {
    if (open !== 'display') {
      displayFor = '';
      return;
    }
    if (!asset || displayFor === asset.asset) return;
    displayFor = asset.asset;
    displayText = asset.display_name ?? '';
  });
  const displayValue = $derived(normalizeDisplayName(displayText));
  const displayProblem = $derived(displayNameProblem(displayValue));
  const displayBytes = $derived(new TextEncoder().encode(displayValue).length);
  async function saveDisplay(remove: boolean) {
    if (!remove && (!displayValue || displayProblem || displayValue === asset?.display_name)) return;
    await run('setassetdisplayname', [arg, remove ? '' : displayValue], () => {
      toast('success', remove ? t('Display name removed') : t('Display name set'), t('The display name of %1 changes with the next block.', holding.label));
    });
  }
  // Give control.
  let giveTo = $state('');
  let giveValid = $state(false);
  // Burn.
  let burnUnits = $state<bigint | null>(null);
  let burnText = $state('');

  const FIELD_NAMES: Record<DataField, string> = {
    info: 'Description',
    commitment: 'Commitment',
    ipv4: 'IPv4 address',
    ipv6: 'IPv6 address',
    encryptionkey: 'Encryption key',
    signingkey: 'Signing key',
  };
  const FIELD_HINTS: Record<DataField, string> = {
    info: 'What it is: a description, a link (at most 512 bytes)',
    commitment: 'A hash (64 hex digits) of documents kept elsewhere: terms, a prospectus',
    ipv4: 'An IPv4 address of the issuer, with a port or not',
    ipv6: 'An IPv6 address of the issuer, in brackets with a port or not',
    encryptionkey: 'A public key (66 hex digits) to encrypt messages to the issuer to',
    signingkey: 'An x-only public key (64 hex digits) the issuer signs with',
  };

  async function run<T>(method: string, params: unknown[], done: (r: T) => void): Promise<void> {
    const wallet = wallets.current;
    if (wallet === null) return;
    busy = true;
    try {
      let r: T | null;
      // Each method named as it is (the RPC policy test reads the calls).
      switch (method) {
        case 'mintasset':
          r = await walletRpc<T>(wallet, 'mintasset', params);
          break;
        case 'updateasset':
          r = await walletRpc<T>(wallet, 'updateasset', params);
          break;
        case 'transferassetcontrol':
          r = await walletRpc<T>(wallet, 'transferassetcontrol', params);
          break;
        case 'fixassetsupply':
          r = await walletRpc<T>(wallet, 'fixassetsupply', params);
          break;
        case 'setassetlogo':
          r = await walletRpc<T>(wallet, 'setassetlogo', params);
          break;
        case 'setassetdisplayname':
          r = await walletRpc<T>(wallet, 'setassetdisplayname', params);
          break;
        default:
          r = await walletRpc<T>(wallet, 'burnasset', params);
      }
      if (r === null) return;
      done(r);
      open = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('My assets'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function mint() {
    if (mintUnits === null) return;
    const amount = amountParam(mintUnits, decimals);
    const to = mintTo.trim();
    if (to && !mintToValid) return;
    const r = await ask({
      title: t('Mint %1', holding.label),
      message: t('Mint %1 more %2?', formatUnits(mintUnits, decimals), nameOf(holding.asset, holding.label)),
      informative: to ? t('The new coins go to %1. The supply goes up by as much.', to) : t('The new coins go to this wallet. The supply goes up by as much.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Mint', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    await run('mintasset', to ? [arg, amount, to] : [arg, amount], () => {
      toast('success', t('Minting'), t('%1 more %2 with the next block.', formatUnits(mintUnits ?? 0n, decimals), holding.label));
      mintText = '';
      mintTo = '';
    });
  }

  async function saveData() {
    if (Object.keys(problems).length || Object.keys(updates).length === 0) return;
    const lines = Object.entries(updates).map(([k, v]) => (v === null ? t('%1: deleted', t(FIELD_NAMES[k as DataField])) : t('%1: %2', t(FIELD_NAMES[k as DataField]), v)));
    const r = await ask({
      title: t('Change the data of %1', holding.label),
      message: tn('Change %n field(s) of the data of %1?', lines.length, nameOf(holding.asset, holding.label)),
      details: lines,
      informative: t('The change is on the chain for good: the history of each field keeps every value it had.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Save', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    await run('updateasset', [arg, updates], () => toast('success', t('Data saved'), t('The data of %1 changes with the next block.', holding.label)));
  }

  async function giveControl() {
    const to = giveTo.trim();
    if (!to || !giveValid) return;
    const r = await ask({
      title: t('Give control of %1', holding.label),
      message: t('Give the control coin of %1 to this address?', nameOf(holding.asset, holding.label)),
      code: to,
      informative: t('Its holder mints more and changes its data; only they can give it back.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Give control', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    await run('transferassetcontrol', [arg, to], () => {
      toast('success', t('Control given'), t('The control of %1 moves with the next block.', holding.label));
      giveTo = '';
    });
  }

  async function fixSupply() {
    const r = await ask({
      title: t('Fix the supply of %1', holding.label),
      message: t('Burn the control coin of %1? Nobody will ever mint more of it or change its data.', nameOf(holding.asset, holding.label)),
      informative: t('This cannot be undone. Holders then know the supply will never grow.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Fix the supply', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    await run('fixassetsupply', [arg, true], () => toast('success', t('Supply fixed'), t('The supply of %1 is fixed with the next block.', holding.label)));
  }

  async function burn() {
    if (burnUnits === null) return;
    const r = await ask({
      title: t('Burn %1', holding.label),
      message: t('Destroy %1 %2 for good?', formatUnits(burnUnits, decimals), nameOf(holding.asset, holding.label)),
      informative: t('The supply goes down by as much. This cannot be undone.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Burn', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    await run('burnasset', [arg, amountParam(burnUnits, decimals)], () => {
      toast('success', t('Burning'), t('%1 %2 burned with the next block.', formatUnits(burnUnits ?? 0n, decimals), holding.label));
      burnText = '';
    });
  }

  function toggle(a: Action) {
    open = open === a ? '' : a;
  }
</script>

<section class="panel manage" aria-label={t('Manage %1', holding.label)}>
  <header class="head">
    <AssetBadge {id} label={holding.label} size="l" showLabel={false} />
    <div class="who">
      <h2><AssetName label={holding.label} display={asset?.display_name ?? ''} /></h2>
      <p class="amount"><strong class="num">{formatAmount(holding.balance, decimals)}</strong> <span class="muted">of {formatAmount(holding.supply, decimals)} in all</span></p>
      {#if approx(holding.pending) > 0}<p class="ba-warn small">+{formatAmount(holding.pending, decimals)} on the way: spendable after the next block</p>{/if}
      {#if holding.control_pending}<p class="faint small">The control coin is moving, with a transaction waiting for a block: minting and changes are possible again after it.</p>{/if}
    </div>
  </header>

  <div class="quick">
    <button type="button" class="btn" disabled={balance <= 0n} onclick={() => { sendAsset(id); go('send'); }}><Icon name="send" size={15} /> Send</button>
    <button type="button" class="btn" onclick={() => { tradeIntent.pay = id; tradeIntent.get = '0'.repeat(64); go('trade', 'swap'); }}><Icon name="swap" size={15} /> Trade</button>
    <button type="button" class="btn" disabled={balance <= 0n} onclick={() => { sellIntent.asset = id; go('auctions', 'sell'); }}><Icon name="gavel" size={15} /> Auction</button>
  </div>

  <div class="actions">
    {#if control && !holding.fixed}
      <button type="button" class="act" aria-expanded={open === 'mint'} onclick={() => toggle('mint')}><Icon name="plus" size={16} /> <span>Mint more</span></button>
      {#if open === 'mint'}
        <form class="ba-form body" onsubmit={(e) => { e.preventDefault(); void mint(); }}>
          <UnitsInput id="mint-amount" label="Amount" {decimals} bind:units={mintUnits} bind:text={mintText} unit={holding.label} />
          <AddressInput id="mint-to" label="To" bind:value={mintTo} bind:valid={mintToValid} required={false} hint="Empty: this wallet" />
          <div><button type="submit" class="btn primary" disabled={busy || mintUnits === null || (!!mintTo.trim() && !mintToValid)}>Mint</button></div>
        </form>
      {/if}

      <button type="button" class="act" aria-expanded={open === 'data'} onclick={() => toggle('data')}><Icon name="edit" size={16} /> <span>Change its data</span></button>
      {#if open === 'data'}
        <form class="ba-form body" onsubmit={(e) => { e.preventDefault(); void saveData(); }}>
          {#each DATA_FIELDS as f (f)}
            <div class="field">
              <label for="data-{f}">{t(FIELD_NAMES[f])}</label>
              {#if f === 'info'}
                <textarea id="data-{f}" rows="3" bind:value={fields[f]} aria-invalid={problems[f] ? 'true' : undefined}></textarea>
              {:else}
                <input id="data-{f}" type="text" class="mono" spellcheck="false" autocomplete="off" bind:value={fields[f]} aria-invalid={problems[f] ? 'true' : undefined} />
              {/if}
              {#if problems[f]}<span class="error">{t(problems[f] ?? '')}</span>{:else}<span class="hint">{t(FIELD_HINTS[f])}</span>{/if}
            </div>
          {/each}
          <p class="ba-help">A field emptied is deleted. {tn('%n field(s) changed.', Object.keys(updates).length)}</p>
          <div><button type="submit" class="btn primary" disabled={busy || Object.keys(problems).length > 0 || Object.keys(updates).length === 0}>Save</button></div>
        </form>
      {/if}

      {#if logosActive}
        <button type="button" class="act" aria-expanded={open === 'logo'} onclick={() => toggle('logo')}><Icon name="picture" size={16} /> <span>{asset?.logo ? 'Change its logo' : 'Add a logo'}</span></button>
        {#if open === 'logo'}
          <form class="ba-form body" onsubmit={(e) => { e.preventDefault(); void saveLogo(false); }}>
            <LogoPicker id="manage-logo" bind:logo />
            {#if logo}<div class="logo-before-after"><AssetBadge {id} label={holding.label} size="l" showLabel={false} /> <Icon name="chevron-right" size={16} /> <AssetBadge {id} label={holding.label} size="l" showLabel={false} preview={logo.url} /></div>{/if}
            <p class="ba-help">Everyone sees it wherever the asset is shown. It replaces the logo before; the fee pays for its bytes on the chain.</p>
            <div class="row-buttons">
              <button type="submit" class="btn primary" disabled={busy || !logo}>Save the logo</button>
              {#if asset?.logo}<button type="button" class="btn" disabled={busy} onclick={() => void saveLogo(true)}>Remove the logo</button>{/if}
            </div>
          </form>
        {/if}
      {/if}

      {#if tickers}
        <button type="button" class="act" aria-expanded={open === 'display'} onclick={() => toggle('display')} data-testid="edit-display-name"><Icon name="edit" size={16} /> <span>{asset?.display_name ? 'Change its display name' : 'Add a display name'}</span></button>
        {#if open === 'display'}
          <form class="ba-form body" onsubmit={(e) => { e.preventDefault(); void saveDisplay(false); }}>
            <div class="field">
              <label for="manage-display">Display name</label>
              <input id="manage-display" type="text" autocomplete="off" placeholder="Such as Gold Bar Token, 1 g" bind:value={displayText} aria-invalid={displayProblem ? 'true' : undefined} />
              {#if displayProblem}<span class="error">{t(displayProblem)}</span>{:else}<span class="hint"><span translate="no">{displayBytes} / {MAX_DISPLAY_NAME_BYTES}</span> bytes</span>{/if}
            </div>
            <p class="ba-help">Shown after the ticker <strong><SafeText text={holding.label} /></strong>, which stays as it is. Not unique: other assets may show the same.</p>
            <div class="row-buttons">
              <button type="submit" class="btn primary" disabled={busy || !displayValue || !!displayProblem || displayValue === asset?.display_name}>Save the display name</button>
              {#if asset?.display_name}<button type="button" class="btn" disabled={busy} onclick={() => void saveDisplay(true)}>Remove it</button>{/if}
            </div>
          </form>
        {/if}
      {/if}

      <button type="button" class="act" aria-expanded={open === 'control'} onclick={() => toggle('control')}><Icon name="crown" size={16} /> <span>Control and supply</span></button>
      {#if open === 'control'}
        <div class="ba-form body">
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); void giveControl(); }}>
            <AddressInput id="control-to" label="Give the control coin to" bind:value={giveTo} bind:valid={giveValid} />
            <div><button type="submit" class="btn" disabled={busy || !giveValid}>Give control</button></div>
          </form>
          <p class="ba-help">Fixing the supply burns the control coin: nobody can mint more or change the data, ever. Holders know the supply will never grow.</p>
          <div><button type="button" class="btn danger" disabled={busy} onclick={fixSupply}>Fix the supply for good</button></div>
        </div>
      {/if}
    {:else if holding.fixed}
      <p class="ba-help body">Its supply is fixed: nobody can mint more of it or change its data.</p>
    {/if}

    <button type="button" class="act" aria-expanded={open === 'burn'} disabled={balance <= 0n} onclick={() => toggle('burn')}><Icon name="flame" size={16} /> <span>Burn some</span></button>
    {#if open === 'burn'}
      <form class="ba-form body" onsubmit={(e) => { e.preventDefault(); void burn(); }}>
        <UnitsInput id="burn-amount" label="Amount" {decimals} bind:units={burnUnits} bind:text={burnText} max={balance} unit={holding.label} />
        <div><button type="submit" class="btn danger" disabled={busy || burnUnits === null}>Burn</button></div>
      </form>
    {/if}
  </div>
</section>

<style>
  .logo-before-after {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .row-buttons {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .manage {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    padding: 1.1rem;
  }
  .head {
    display: flex;
    gap: 0.9rem;
    align-items: center;
  }
  .who {
    min-width: 0;
  }
  .who h2 {
    margin: 0;
    font-size: 1.4rem;
    overflow-wrap: anywhere;
  }
  .who p {
    margin: 0.15rem 0 0;
  }
  .amount strong {
    font-size: 1.25rem;
  }
  .quick {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .actions {
    display: flex;
    flex-direction: column;
    border-top: 1px solid var(--line);
  }
  .act {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.7rem 0.2rem;
    border: 0;
    border-bottom: 1px solid var(--line);
    background: none;
    color: var(--text);
    font: inherit;
    font-weight: 600;
    text-align: left;
    cursor: pointer;
  }
  .act:hover:not(:disabled) {
    color: var(--accent-fg);
  }
  .act:disabled {
    color: var(--text-faint);
    cursor: default;
  }
  .act[aria-expanded='true'] {
    color: var(--accent-fg);
  }
  .body {
    padding: 0.8rem 0.2rem 1rem;
    border-bottom: 1px solid var(--line);
  }
  .small {
    font-size: 0.86rem;
  }
</style>

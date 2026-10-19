<script lang="ts">
  // What the Send page sends, beside the coin of the chain: an asset. A choice at the top of Send (CHN, or an
  // asset this wallet holds); with an asset chosen, this form takes the place of the kit's (`active`): the
  // address, the amount (checked with the decimals of the asset, at most what the wallet holds), and a
  // confirmation, then sendasset. The Send panel of My assets of the Qt window of BitAssets, in the Send page.
  import SafeText from '../components/SafeText.svelte';
  import AddressInput from '../components/AddressInput.svelte';
  import Icon from '../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../lib/dialogs.svelte';
  import { t } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import { toast, wallets } from '../lib/state.svelte';
  import './ba/ba.css';
  import AssetBadge from './ba/AssetBadge.svelte';
  import AssetPicker from './ba/AssetPicker.svelte';
  import HiddenToggle from './ba/HiddenToggle.svelte';
  import { lookAlikes, type PickChip, type PickChoice } from './ba/picker';
  import { withoutHidden } from './ba/visible.svelte';
  import { assetById, hasPool } from './ba/store.svelte';
  import UnitsInput from './ba/UnitsInput.svelte';
  import { amountParam, approx, assetArg, CHN_ID, formatAmount, formatUnits, unitsAtMost, unitsOf } from './ba/model';
  import { ba, nameOf, refreshBitAssets, sendIntent, startBitAssets, unlockHoldings } from './ba/store.svelte';

  let { active = $bindable(false) }: { active?: boolean } = $props();

  $effect(() => startBitAssets());

  let what = $state(CHN_ID);
  const holdings = $derived(ba.wallet === wallets.current ? (ba.mine?.assets ?? []).filter((h) => approx(h.balance) > 0 || h.asset === what) : []);
  $effect(() => {
    if (sendIntent.asset) {
      what = sendIntent.asset;
      sendIntent.asset = '';
    }
  });
  $effect(() => {
    active = what !== CHN_ID;
  });

  // What to send, in the searchable picker (hundreds of assets): CHN first, then what the wallet holds, without what it
  // hides (its asset filters) unless shown; two of one name marked, told apart by their numbers.
  let showHidden = $state(false);
  const visible = $derived(withoutHidden(holdings, (h) => h.asset, (h) => h.label, showHidden));
  // The asset chosen stays listed (sent to from another page) even if hidden.
  const listed = $derived(what === CHN_ID || visible.items.some((h) => h.asset === what) ? visible.items : [...visible.items, ...holdings.filter((h) => h.asset === what)]);
  const sendChoices: PickChoice[] = $derived.by(() => {
    const list: PickChoice[] = listed.map((h) => ({ id: h.asset, label: h.label, seq: assetById(h.asset)?.seq, held: formatAmount(h.balance, h.decimals, { max: 4 }) }));
    const twins = lookAlikes(list);
    return [{ id: CHN_ID, label: 'CHN', seq: '' }, ...list.map((c) => (twins.has(c.id) ? { ...c, warn: t('Another asset you hold has this name: check its number') } : c))];
  });
  // The filters of Browse assets.
  const sendChips: PickChip[] = [
    { id: 'market', label: 'With a market', test: (c) => c.id === CHN_ID || hasPool(c.id) },
    { id: 'mintable', label: 'Mintable', test: (c) => c.id === CHN_ID || assetById(c.id)?.fixed === false },
    { id: 'fixed', label: 'Fixed supply', test: (c) => c.id === CHN_ID || assetById(c.id)?.fixed === true },
    { id: 'controlled', label: 'Controlled', test: (c) => c.id === CHN_ID || !!holdings.find((h) => h.asset === c.id)?.control },
  ];
  const holding = $derived(holdings.find((h) => h.asset === what) ?? null);
  const decimals = $derived(holding?.decimals ?? 0);
  const held = $derived(holding ? unitsAtMost(holding.balance, holding.decimals) : 0n);

  let to = $state('');
  let toValid = $state(false);
  let amountText = $state('');
  let amount = $state<bigint | null>(null);
  let busy = $state(false);
  let formKey = $state(0);

  async function send() {
    const wallet = wallets.current;
    if (wallet === null || !holding || !toValid || amount === null || busy) return;
    const address = to.trim();
    const r = await ask({
      title: t('Send %1', holding.label),
      message: t('Send %1 %2 to this address?', formatUnits(amount, decimals), nameOf(holding.asset, holding.label)),
      code: address,
      informative: t('It arrives with the next block. The fee is paid in CHN by this wallet.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Send', kind: 'primary', delay: 2 },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'sendasset', [address, assetArg(holding.asset), amountParam(amount, decimals)]);
      if (!done) return;
      toast('success', t('Sent'), t('%1 %2: it arrives with the next block.', formatUnits(amount, decimals), holding.label));
      amountText = '';
      to = '';
      formKey++;
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Send %1', holding.label), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

{#if wallets.current !== null}
  <section class="panel what" aria-label="What to send">
    <div class="pick-row">
      <AssetPicker id="send-what" label="Asset to send" bind:value={what} choices={sendChoices} chips={sendChips} />
      {#if ba.mineState === 'locked' && holdings.length === 0}
        <button type="button" class="ba-chip" onclick={() => void unlockHoldings()}><Icon name="unlock" size={14} /> Show my assets</button>
      {/if}
    </div>
    <HiddenToggle count={visible.hidden} bind:show={showHidden} />
    {#if !active}<p class="ba-help">Sending CHN. Pick an asset above to send it instead: any address of this chain receives any asset.</p>{/if}
  </section>

  {#if active && holding}
    <form class="panel ba-form asset-send" onsubmit={(e) => { e.preventDefault(); void send(); }}>
      <h2 class="title"><AssetBadge id={holding.asset} label={holding.label} /> </h2>
      {#key formKey}<AddressInput id="asset-to" label="Pay to" bind:value={to} bind:valid={toValid} />{/key}
      <UnitsInput id="asset-amount" label="Amount" {decimals} bind:units={amount} bind:text={amountText} max={held} unit={holding.label} hint={approx(holding.pending) > 0 ? t('%1 more on the way: spendable after the next block', formatAmount(holding.pending, decimals)) : ''} />
      <p class="ba-help">You hold {formatAmount(holding.balance, decimals)} <SafeText text={holding.label} />. The fee of the transaction is paid in CHN.</p>
      <div class="row">
        <button type="submit" class="btn primary" disabled={busy || !toValid || amount === null}><Icon name="send" size={17} /> {busy ? 'Working…' : 'Send'}</button>
        <button type="button" class="btn ghost" onclick={() => go('my-assets')}>My assets</button>
      </div>
    </form>
  {:else if active}
    <p class="panel ba-empty">This wallet holds none of it in blocks.</p>
  {/if}
{/if}

<style>
  .what {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    padding: 0.8rem 1rem;
  }
  .pick-row {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 0.5rem;
    max-width: 44rem;
  }
  .pick-row :global(.asset-picker) {
    flex: 1 1 18rem;
  }
  .asset-send {
    padding: 1.1rem;
    max-width: 44rem;
  }
  .title {
    margin: 0;
    font-size: 1.1rem;
  }
</style>

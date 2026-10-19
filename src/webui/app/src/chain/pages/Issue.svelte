<script lang="ts">
  // Issue: a wizard that creates an asset. The name (checked as the chain checks it, and asked of the node: is
  // it free?), the supply and its decimals (with the smallest amount, and the largest the chain takes), what it
  // is (a description, and the issuer's commitment, addresses and keys), whether its name is published; then a
  // preview, and the two steps on the way: a reservation that hides the name, then the registration once the
  // reservation is deep enough, which this app sends by itself (ba/store.svelte.ts), as chains-qt did. The
  // Create panel of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import PendingCard from '../../components/PendingCard.svelte';
  import { blockSeconds } from '../../lib/pending.svelte';
  import { errorText, inform, prepareConfirm, walletRpcTwoSteps } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { niceTimeOffset } from '../../lib/format';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app, node, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../ba/ba.css';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import UnitsInput from '../ba/UnitsInput.svelte';
  import LogoPicker, { type PickedLogo } from '../ba/LogoPicker.svelte';
  import { amountParam, DATA_FIELDS, displayNameProblem, hashName, dataProblems, dataToSet, formatUnits, MAX_AMOUNT, MAX_DECIMALS, MAX_DISPLAY_NAME_BYTES, MAX_INFO, MAX_TICKER, MIN_TICKER, nameProblem, normalizeDisplayName, smallestAmount, tickerOf, tickerProblem, type DataField } from '../ba/model';
  import { ba, tickersActive, notePending, pendingOf, pendingOpOf, recentlyRegistered, refreshBitAssets, registerParams, startBitAssets, type PendingIssue } from '../ba/store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/terms';

  /** Shown as a tab of My assets, under its heading. */
  let { embedded = false }: { embedded?: boolean } = $props();

  $effect(() => startBitAssets());

  // Simple mode (lib/uimode.svelte.ts): "Create a token", the same wizard and the same calls (reserveasset, then
  // the registration the store sends), with the decimals, the private name and the issuer's data under
  // "More options".
  const simple = $derived(simpleMode());
  let moreSupply = $state(false);

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  type Step = 'name' | 'supply' | 'about' | 'review' | 'done';
  const STEPS: { id: Step; label: string }[] = [
    { id: 'name', label: 'Name' },
    { id: 'supply', label: 'Supply' },
    { id: 'about', label: 'About it' },
    { id: 'review', label: 'Review' },
  ];
  let step = $state<Step>('name');

  // --- The name: from ticker_height a ticker (2 to 12 of A-Z, 0-9, uppercased as typed), and a display name ---
  const tickers = $derived(tickersActive());
  let name = $state('');
  let availability = $state<'' | 'checking' | 'free' | 'taken' | 'mine'>('');
  const problem = $derived(tickers ? tickerProblem(name) || nameProblem(name) : nameProblem(name));
  function typed(input: HTMLInputElement) {
    const text = input.value;
    name = tickers ? tickerOf(text) : text;
    // Uppercased in the field too, where the caret is (the state may not change: "gold" typed over "GOLD").
    if (input.value !== name) {
      const at = input.selectionStart;
      input.value = name;
      if (at !== null) input.setSelectionRange(at, at);
    }
  }
  // Optional, not unique; sent in NFC (the rules take NFC only).
  let displayName = $state('');
  const display = $derived(normalizeDisplayName(displayName));
  const displayProblem = $derived(tickers ? displayNameProblem(display) : '');
  const displayBytes = $derived(new TextEncoder().encode(display).length);
  let check = 0;
  $effect(() => {
    const text = name;
    const mine = ++check;
    availability = '';
    if (!text || nameProblem(text)) return;
    if (pendingOf(wallets.current).some((p) => p.name === text) || ba.mine?.reservations.some((r) => r.name === text)) {
      availability = 'mine';
      return;
    }
    const timer = setTimeout(async () => {
      availability = 'checking';
      try {
        await rpc.raw('getasset', [text]);
        if (mine === check) availability = 'taken';
      } catch {
        // No such asset: the name is free (a reservation of someone else is secret, and may take it first).
        if (mine === check) availability = 'free';
      }
    }, 350);
    return () => clearTimeout(timer);
  });

  // The id the asset will have: the hash of its name (its colour in the pages).
  let previewId = $state('1'.repeat(64));
  $effect(() => {
    const text = name;
    if (!text) return;
    hashName(text)
      .then((h) => {
        if (text === name) previewId = h;
      })
      .catch(() => undefined);
  });

  // --- The supply ---
  let decimals = $state(2);
  let supplyText = $state('');
  let supply = $state<bigint | null>(0n);
  const supplyValue = $derived(supplyText.trim() === '' ? 0n : supply);
  const largest = $derived(formatUnits(MAX_AMOUNT, decimals, { max: decimals }));

  // --- About it ---
  let fields = $state<Record<DataField, string>>({ info: '', commitment: '', ipv4: '', ipv6: '', encryptionkey: '', signingkey: '' });
  let advanced = $state(false);
  let published = $state(true);
  const problems = $derived(dataProblems(fields));
  // Its logo (optional): on the chain with the registration, once the chain has the rules of logos (logo_height).
  let logo = $state<PickedLogo | null>(null);
  const logoHeight = $derived(ba.info?.logo_height ?? -1);
  const logosSoon = $derived(logoHeight >= 0 && logoHeight <= Math.max(ba.height, 0) + (ba.info?.reveal_depth ?? 1));
  const maxInfo = MAX_INFO;
  const infoBytes = $derived(new TextEncoder().encode(fields.info).length);

  const FIELD_NAMES: Record<DataField, string> = {
    info: 'Description',
    commitment: 'Commitment',
    ipv4: 'IPv4 address',
    ipv6: 'IPv6 address',
    encryptionkey: 'Encryption key',
    signingkey: 'Signing key',
  };
  const FIELD_HINTS: Record<DataField, string> = {
    info: 'What it is: a description, a link',
    commitment: 'A hash (64 hex digits) of documents kept elsewhere: terms, a prospectus',
    ipv4: 'An IPv4 address of the issuer, such as 203.0.113.7:8333',
    ipv6: 'An IPv6 address of the issuer, such as [2001:db8::7]:8333',
    encryptionkey: 'A public key (66 hex digits) to encrypt messages to the issuer to',
    signingkey: 'An x-only public key (64 hex digits) the issuer signs with',
  };

  const depth = $derived(ba.info?.reveal_depth ?? 1);
  // The pace of the last blocks (a sidechain follows its mainchain), else the target: as the progress cards say it.
  const spacing = $derived(blockSeconds(node.recent, app.info?.blockSpacing ?? 600));

  const canNext = $derived.by(() => {
    if (step === 'name') return !!name && !problem && availability === 'free' && !displayProblem;
    if (step === 'supply') return supplyValue !== null && Number.isInteger(decimals) && decimals >= 0 && decimals <= MAX_DECIMALS;
    if (step === 'about') return Object.keys(problems).length === 0;
    return true;
  });
  const order: Step[] = ['name', 'supply', 'about', 'review'];
  function next() {
    if (!canNext) return;
    const i = order.indexOf(step);
    if (i >= 0 && i < order.length - 1) step = order[i + 1];
  }
  function back() {
    const i = order.indexOf(step);
    if (i > 0) step = order[i - 1];
  }

  // The review shows: the question of the host is likely, its window is made ahead (it shows at once).
  $effect(() => {
    if (step === 'review') prepareConfirm();
  });

  let busy = $state(false);
  let created = $state('');
  async function create() {
    const wallet = wallets.current;
    if (wallet === null || busy || supplyValue === null) return;
    busy = true;
    try {
      // Both steps in one question of the host: the reservation now, and the registration exactly as shown, which the
      // app then sends by itself without asking again (confirm.h, Reveal).
      const issue: PendingIssue = {
        wallet,
        name,
        asset: '',
        supply: amountParam(supplyValue, decimals),
        decimals,
        data: dataToSet(fields),
        public: tickers ? true : published,
        ...(tickers && display ? { display_name: display } : {}),
        time: Math.floor(Date.now() / 1000),
        reservation: '',
        ...(logo && logosSoon ? { logo: logo.base64 } : {}),
      };
      const done = await walletRpcTwoSteps<{ txid: string; asset: string }>(wallet, 'reserveasset', [name], { method: 'registerasset', params: registerParams(issue) });
      if (!done) return;
      const saved = await notePending({ ...issue, asset: done.asset, reservation: done.txid });
      if (!saved) {
        await inform(t('The journal of BitAssets could not be saved'), t('%1 is reserved. If this app restarts before it is registered, register it in My assets, with its supply and data.', name), 'warning');
      }
      created = name;
      step = 'done';
      toast('success', t('Name reserved'), tn('%1 is registered by itself once the reservation is %n block(s) deep, without asking again.', depth, name));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Register new asset'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // Its progress, live, as on its card in My assets.
  const createdOp = $derived.by(() => {
    if (!created || wallets.current === null) return null;
    const p = pendingOf(wallets.current).find((x) => x.name === created);
    if (p) return pendingOpOf(p);
    const d = recentlyRegistered(wallets.current).find((x) => x.name === created);
    return d ? { key: d.name, what: t('Asset'), name: d.name, wallet: d.wallet, stage: 'done' as const, depth: null, needed: 0, ended: d.ended } : null;
  });

  function again() {
    name = '';
    displayName = '';
    supplyText = '';
    decimals = 2;
    fields = { info: '', commitment: '', ipv4: '', ipv6: '', encryptionkey: '', signingkey: '' };
    published = true;
    advanced = false;
    logo = null;
    created = '';
    step = 'name';
  }
</script>

{#if noWallet}
  <NoWallet page={PAGES.issue} />
{:else}
  <div class="page ba-page issue" class:embedded>
    <!-- A tab of My assets (user feedback 2026-10-19): the tab says its name. -->
    {#if !embedded}<header class="page-head"><h1>{simple ? 'Create a token' : 'Register new asset'}</h1></header>{/if}
    {#if simple}
      <p class="ba-help lead">A <Term id="asset">token</Term> of your own: points, tickets, shares. Give it a name and an amount; you can send it, swap it, and make more later with its <Term id="controlcoin">control coin</Term>.</p>
    {:else}
      <p class="ba-help lead">A token of your own: shares, points, tickets, a stablecoin. You hold its control coin, which mints more and describes it; trade it in a pool, or sell it by auction.</p>
    {/if}

    {#if step !== 'done'}
      <ol class="ba-steps" aria-label="Steps">
        {#each STEPS as s, i (s.id)}
          <li class:done={order.indexOf(step) > i} class:now={step === s.id}>{t(s.label)}</li>
        {/each}
      </ol>
    {/if}

    <div class="ba-split">
      <section class="panel wizard">
        {#if step === 'name'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            <div class="field">
              <label for="issue-name">{tickers ? 'Ticker' : 'Its name'}</label>
              <input id="issue-name" class="big" class:ticker={tickers} type="text" maxlength={tickers ? MAX_TICKER : 64} placeholder="Such as GOLD" autocomplete="off" autocapitalize="characters" spellcheck="false" value={name} oninput={(e) => typed(e.currentTarget)} aria-invalid={problem ? 'true' : undefined} aria-describedby="issue-name-hint" />
              {#if problem}<span class="error">{t(problem)}</span>
              {:else if availability === 'checking'}<span class="hint">Checking…</span>
              {:else if availability === 'free'}<span class="hint ba-good"><Icon name="check" size={14} /> <SafeText text={name} /> is free</span>
              {:else if availability === 'taken'}<span class="error"><SafeText text={name} /> is taken</span>
              {:else if availability === 'mine'}<span class="error">This wallet is creating <SafeText text={name} /> already: see My assets.</span>
              {:else if tickers}<span class="hint" id="issue-name-hint"><span translate="no">{MIN_TICKER}–{MAX_TICKER}</span> letters A–Z and digits 0–9, such as GOLD or BTC2; unique, first come first served.</span>
              {:else}<span class="hint" id="issue-name-hint">1 to 64 plain characters (ASCII), no space at either end; not CHN, not "0x…", not like a number such as 1739-0029.</span>{/if}
            </div>
            {#if tickers}
              <div class="field">
                <label for="issue-display">Display name <span class="faint">(optional)</span></label>
                <input id="issue-display" type="text" placeholder="Such as Gold Bar Token, 1 g" autocomplete="off" bind:value={displayName} aria-invalid={displayProblem ? 'true' : undefined} />
                {#if displayProblem}<span class="error">{t(displayProblem)}</span>
                {:else}<span class="hint"><span translate="no">{displayBytes} / {MAX_DISPLAY_NAME_BYTES}</span> bytes. Shown after the ticker; not unique: other assets may have the same. The controller can change it later.</span>{/if}
              </div>
            {/if}
            <p class="ba-help">{tickers ? 'The asset is known by its ticker (the hash of it).' : 'The asset is known by the hash of its name.'} Nobody can take a name between your two steps: the reservation hides it.</p>
            <div class="row"><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'supply'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            {#if simple}
              <button type="button" class="btn ghost small more" aria-expanded={moreSupply} onclick={() => (moreSupply = !moreSupply)}><Icon name={moreSupply ? 'chevron' : 'chevron-right'} size={14} /> More options: decimals ({decimals})</button>
            {/if}
            {#if !simple || moreSupply}
            <div class="field">
              <label for="issue-decimals">Decimals: {decimals}</label>
              <input id="issue-decimals" type="range" min="0" max={MAX_DECIMALS} step="1" bind:value={decimals} />
              <span class="hint">They only change how amounts are shown: with {decimals}, the smallest amount is {smallestAmount(decimals)}. They are set for good.</span>
            </div>
            {/if}
            <UnitsInput id="issue-supply" label={simple ? 'How many to create' : 'Initial supply'} {decimals} allowZero bind:units={supply} bind:text={supplyText} placeholder="0" hint={simple ? 'You can make more later.' : '0: none yet; the control coin mints more later'} />
            <p class="ba-help">At most {largest} with {decimals} decimals.</p>
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'about'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            <div class="field">
              <label for="issue-info">Description</label>
              <textarea id="issue-info" rows="4" placeholder="What it is: a description, a link (optional)" bind:value={fields.info} aria-invalid={problems.info ? 'true' : undefined}></textarea>
              {#if problems.info}<span class="error">{t(problems.info)}</span>{:else}<span class="hint">{infoBytes} / {maxInfo} bytes</span>{/if}
            </div>
            <div class="field">
              <label for="issue-logo-file">Logo</label>
              {#if logosSoon}
                <LogoPicker id="issue-logo" bind:logo />
              {:else if logoHeight >= 0}
                <span class="hint">Logos are on the chain from block {logoHeight}.</span>
              {:else}
                <span class="hint">This network has no logos of assets yet.</span>
              {/if}
            </div>
            {#if tickers}
              <p class="ba-help">Its ticker is listed, so that anyone can find it.</p>
            {:else if simple && !advanced}
              <p class="ba-help">Its name is listed, so that anyone can find it.</p>
            {:else}
            <label class="check"><input type="checkbox" bind:checked={published} /> <span>Publish the name, so that it is listed and found by name</span></label>
            {/if}
            {#if !published && !tickers}<p class="ba-help">A private name: only its hash and number are on the chain. Whoever knows the name finds it; the list of assets shows its hash.</p>{/if}
            <button type="button" class="btn ghost small more" aria-expanded={advanced} onclick={() => (advanced = !advanced)}><Icon name={advanced ? 'chevron' : 'chevron-right'} size={14} /> {#if simple && !tickers}More options: a private name, the issuer's commitment, addresses and keys{:else if simple}More options: the issuer's commitment, addresses and keys{:else}The issuer's commitment, addresses and keys{/if}</button>
            {#if advanced}
              {#each DATA_FIELDS.filter((f) => f !== 'info') as f (f)}
                <div class="field">
                  <label for="issue-{f}">{t(FIELD_NAMES[f])}</label>
                  <input id="issue-{f}" type="text" class="mono" spellcheck="false" autocomplete="off" bind:value={fields[f]} aria-invalid={problems[f] ? 'true' : undefined} />
                  {#if problems[f]}<span class="error">{t(problems[f] ?? '')}</span>{:else}<span class="hint">{t(FIELD_HINTS[f])}</span>{/if}
                </div>
              {/each}
            {/if}
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'review'}
          <div class="ba-form">
            <h2>Review</h2>
            <dl class="ba-lines">
              <dt>{tickers ? 'Ticker' : 'Name'}</dt><dd><SafeText class="ticker-text" text={name} /></dd>
              {#if tickers}<dt>Display name</dt><dd>{#if display}<SafeText text={display} />{:else}<span class="faint">None</span>{/if}</dd>{/if}
              <dt>Initial supply</dt><dd>{formatUnits(supplyValue ?? 0n, decimals)}</dd>
              <dt>Decimals</dt><dd>{decimals}</dd>
              {#if !tickers}<dt>The name</dt><dd>{published ? 'Published' : 'Private: only its hash'}</dd>{/if}
              {#if logo && logosSoon}<dt>Logo</dt><dd class="logo-line"><img class="logo-review" src={logo.url} alt="" width="64" height="64" /> {logo.kind}, {(logo.size / 1024).toFixed(1)} KB</dd>{/if}
              {#each DATA_FIELDS.filter((f) => fields[f].trim()) as f (f)}
                <dt>{t(FIELD_NAMES[f])}</dt><dd><SafeText class="ba-mono" text={fields[f]} /></dd>
              {/each}
            </dl>
            <h3>On the way</h3>
            <ol class="timeline">
              <li><strong>Now: the reservation.</strong> It hides the name, so that nobody can see it coming and take it first.</li>
              <li><strong>{tn('%n block(s) later (about %1): the registration.', depth, niceTimeOffset(depth * spacing))}</strong> Not sooner, so that whoever makes a block cannot take the name first. This app sends it by itself while it runs.</li>
              <li><strong>The block after: in your assets</strong>, with its supply and its control coin.</li>
            </ol>
            <p class="ba-help">Two small transactions, each with the fee of the wallet, confirmed together once. A reservation can be released while it is not registered.</p>
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="button" class="btn primary" disabled={busy} aria-busy={busy} onclick={create}><Icon name="issue" size={15} /> {busy ? 'Waiting for your confirmation…' : simple ? 'Create the token' : 'Reserve and issue'}</button></div>
            {#if busy}<p class="ba-help" role="status">Confirm both steps in the window of the app; the reservation is sent once you accept.</p>{/if}
          </div>
        {:else}
          <div class="ba-form done">
            <Icon name="check" size={28} />
            <h2><SafeText text={created} /> is reserved</h2>
            <p class="ba-help">{tn('It is registered by itself once the reservation is %n block(s) deep, then it is in your assets after the block after. Keep the app running meanwhile, or come back to it.', depth)}</p>
            {#if createdOp}<PendingCard op={createdOp} />{/if}
            <div class="row"><button type="button" class="btn primary" onclick={() => go('my-assets', 'issuing')}>Follow it</button><button type="button" class="btn" onclick={again}>Issue another</button></div>
          </div>
        {/if}
      </section>

      {#if step !== 'done'}
        <aside class="panel preview" aria-label="Preview">
          <span class="muted small">Preview</span>
          <AssetBadge id={previewId} label={name || '?'} display={tickers ? display : ''} preview={logosSoon ? (logo?.url ?? '') : ''} />
          {#if fields.info.trim()}<SafeText class="blurb" text={fields.info} />{:else}<span class="faint small">No description.</span>{/if}
          <dl class="ba-lines">
            <dt>Supply</dt><dd>{supplyValue !== null ? formatUnits(supplyValue, decimals) : '–'}</dd>
            <dt>Smallest amount</dt><dd>{smallestAmount(decimals)}</dd>
            <dt>Control coin</dt><dd>Yours</dd>
            <dt>Listed</dt><dd>{tickers ? 'By its ticker and display name' : published ? 'By its name' : 'By its hash'}</dd>
          </dl>
        </aside>
      {/if}
    </div>
  </div>
{/if}

<style>
  .embedded {
    padding: 0;
  }
  .lead {
    max-width: 52rem;
  }
  .wizard {
    padding: 1.2rem;
  }
  .wizard h2,
  .wizard h3 {
    margin: 0;
  }
  .big {
    font-size: 1.3rem;
    padding: 0.55rem 0.7rem;
  }
  .big.ticker {
    font-family: var(--font-mono);
    letter-spacing: 0.06em;
    text-transform: uppercase;
    max-width: 16rem;
  }
  .wizard :global(.ticker-text) {
    font-weight: 700;
    letter-spacing: 0.03em;
  }
  .more {
    align-self: flex-start;
  }
  .timeline {
    margin: 0;
    padding-left: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    font-size: 0.92rem;
  }
  .done {
    align-items: flex-start;
  }
  .preview {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 1.1rem;
  }
  .preview :global(.blurb) {
    color: var(--text-muted);
    font-size: 0.9rem;
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.85rem;
  }
  .logo-line {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .logo-review {
    width: 3rem;
    height: 3rem;
    border-radius: 50%;
    border: 1px solid var(--line-strong);
    object-fit: cover;
  }
</style>

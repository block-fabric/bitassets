// What a chain may say of a transaction of the wallet beyond chain/txinfo.svelte.ts's required exports (optional
// exports, so that a chain that does not have them needs no change): whether it hides it (an ignored asset's:
// BitAssets, Layers), and what the notice of it offers (Ignore this asset) or warns of (a look-alike asset).
import * as txinfo from '../chain/txinfo.svelte';

export interface ChainTxNotice {
  /** A warning line put in the notice (a name like an asset the wallet holds). */
  warning?: string;
  /** Buttons of the notice. */
  actions?: { label: string; run: () => void }[];
}

/** How the details of a transaction the chain knows more of show the wallet's own rows (an asset operation's CHN side). */
export interface ChainTxDetailsView {
  /** Leave out the Debit and Credit rows of nothing (0.00000000: noise beside an asset operation). */
  hideZero: boolean;
  /** Rows of the ids put under a disclosure (raw), with the label they are shown with there. */
  raw: Record<string, string>;
  /** Simple mode: only these rows of the CHN side (the asset side says the rest). */
  simpleRows?: string[];
}

const optional = txinfo as unknown as {
  chainTxHidden?: (txid: string) => boolean;
  chainTxNotice?: (txid: string) => ChainTxNotice | undefined;
  chainTxDetails?: (txid: string) => ChainTxDetailsView | undefined;
};

/** Whether the chain hides this transaction of the wallet shown (reactive where the chain's state is). */
export function chainTxHidden(txid: string): boolean {
  return optional.chainTxHidden?.(txid) ?? false;
}

/** How the details of a transaction show its CHN side, if the chain says (else as for any transaction). */
export function chainTxDetails(txid: string): ChainTxDetailsView | undefined {
  return optional.chainTxDetails?.(txid);
}

/** What the notice of a new transaction offers or warns of, if anything. */
export function chainTxNotice(txid: string): ChainTxNotice | undefined {
  return optional.chainTxNotice?.(txid);
}

// Whether an asset shows in the lists of the wallet shown (hide.svelte.ts): its facts from the store, by id.
import { assetHidden, assetIgnored, hiding } from './hide.svelte';
import { assetById, ba, hasPool } from './store.svelte';

/** Whether the wallet shown hides the asset (ignored, or left out by a filter). Never CHN. Reactive. */
export function hiddenAsset(id: string, label?: string): boolean {
  void hiding.version;
  const a = assetById(id);
  return assetHidden({ id, label: label ?? a?.label, lastActivity: a?.last_activity, tip: ba.info ? ba.info.height - 1 : undefined, supply: a?.supply, burned: a?.burned, pool: ba.pools.length || ba.loaded ? hasPool(id) : undefined });
}

/** Whether the wallet shown ignores the asset (the filters aside). */
export function ignoredAsset(id: string, label?: string): boolean {
  void hiding.version;
  return assetIgnored(id, label ?? assetById(id)?.label ?? '');
}

/** A list without what the wallet hides (unless shown), and how many it hides. */
export function withoutHidden<T>(items: readonly T[], idOf: (x: T) => string, labelOf: (x: T) => string | undefined, show = false): { items: T[]; hidden: number } {
  const out: T[] = [];
  let hidden = 0;
  for (const x of items) {
    if (hiddenAsset(idOf(x), labelOf(x))) {
      hidden++;
      if (show) out.push(x);
    } else out.push(x);
  }
  return { items: out, hidden };
}

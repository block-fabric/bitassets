// The assets a user picks most: favourites (starred in the picker) and the last ones picked, first in every
// asset picker (AssetPicker.svelte). Kept with the app's preferences (per network, as asset ids are), under
// keys of their own that the kit leaves alone.
import { host } from '../../lib/rpc';

/** How many of the last picked are kept. */
export const MAX_RECENT = 6;
/** How many favourites are kept. */
export const MAX_FAVOURITES = 50;

export const picks = $state({ favourites: [] as string[], recent: [] as string[] });

let loading: Promise<void> | null = null;

const ids = (v: unknown, max: number): string[] => (Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string' && /^[0-9a-f]{1,64}$/i.test(x)).slice(0, max) : []);

/** Read them from the preferences, once. */
export function loadPicks(): Promise<void> {
  loading ??= host
    .prefs()
    .then((raw) => {
      picks.favourites = ids(raw?.assetFavourites, MAX_FAVOURITES);
      picks.recent = ids(raw?.assetRecent, MAX_RECENT);
    })
    .catch(() => undefined);
  return loading;
}

export function isFavourite(id: string): boolean {
  return picks.favourites.includes(id);
}

export function toggleFavourite(id: string): void {
  picks.favourites = isFavourite(id) ? picks.favourites.filter((x) => x !== id) : [id, ...picks.favourites].slice(0, MAX_FAVOURITES);
  host.setPrefs({ assetFavourites: picks.favourites }).catch(() => undefined);
}

/** An asset was picked: first of the recent ones. */
export function notePicked(id: string): void {
  if (picks.recent[0] === id) return;
  picks.recent = [id, ...picks.recent.filter((x) => x !== id)].slice(0, MAX_RECENT);
  host.setPrefs({ assetRecent: picks.recent }).catch(() => undefined);
}

/** For the tests: forget what was read. */
export function resetPicks(): void {
  loading = null;
  picks.favourites = [];
  picks.recent = [];
}

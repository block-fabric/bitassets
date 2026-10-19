// What an asset picker lists (AssetPicker.svelte), as data: the assets matching what is typed (by name, by
// number "#1739-0029" or its digits, by the start of the id), best first; with nothing typed, the favourites
// and the last picked on top, then every asset. Hundreds of assets: one pass, no sort of the whole list
// unless something is typed.

export interface PickChoice {
  id: string;
  /** Text of the chain. */
  label: string;
  /** Its number (1739-0029): what tells two assets of names alike apart. */
  seq?: string;
  /** What the wallet holds of it, written. */
  held?: string;
  /** A warning shown on its row (a name like another asset the wallet holds). */
  warn?: string;
}

/** A filter of the picker, as the chips at the top of Browse assets (With a market, Mintable...). */
export interface PickChip {
  id: string;
  label: string;
  test: (c: PickChoice) => boolean;
}

/** The choices that hold the same name (any case) as another of them: look-alikes, told apart by their numbers. */
export function lookAlikes(choices: readonly PickChoice[]): Set<string> {
  const byName = new Map<string, number>();
  for (const c of choices) byName.set(c.label.toLowerCase(), (byName.get(c.label.toLowerCase()) ?? 0) + 1);
  return new Set(choices.filter((c) => (byName.get(c.label.toLowerCase()) ?? 0) > 1).map((c) => c.id));
}

export type PickRow = { kind: 'head'; text: string } | { kind: 'item'; choice: PickChoice; fav: boolean };

const digits = (s: string) => s.replace(/\D/g, '');

/** How well a choice matches the words typed (0: not at all). */
export function matchScore(c: PickChoice, query: string): number {
  const q = query.trim().toLowerCase();
  if (!q) return 1;
  const label = c.label.toLowerCase();
  if (q.startsWith('#')) {
    const d = digits(q);
    const s = digits(c.seq ?? '');
    if (!d) return 1;
    return s === d ? 100 : s.startsWith(d) ? 60 : s.includes(d) ? 20 : 0;
  }
  if (label === q) return 100;
  if (label.startsWith(q)) return 80;
  if (/^\d[\d-]*$/.test(q) && c.seq && digits(c.seq).startsWith(digits(q))) return 70;
  if (label.split(/[^a-z0-9]+/).some((w) => w.startsWith(q))) return 50;
  if (label.includes(q)) return 30;
  if (/^[0-9a-f]{4,64}$/.test(q) && c.id.toLowerCase().startsWith(q)) return 25;
  if (c.seq && digits(q).length >= 2 && digits(c.seq).includes(digits(q))) return 10;
  return 0;
}

/** The rows of the list: headings, and the choices under them. */
export function pickerRows(choices: readonly PickChoice[], query: string, favourites: readonly string[], recent: readonly string[]): PickRow[] {
  const fav = new Set(favourites);
  if (query.trim()) {
    const scored: { c: PickChoice; s: number; i: number }[] = [];
    choices.forEach((c, i) => {
      const s = matchScore(c, query);
      if (s > 0) scored.push({ c, s: s + (fav.has(c.id) ? 5 : 0), i });
    });
    scored.sort((a, b) => b.s - a.s || a.i - b.i);
    return scored.map(({ c }) => ({ kind: 'item', choice: c, fav: fav.has(c.id) }));
  }
  const byId = new Map(choices.map((c) => [c.id, c]));
  const top: PickRow[] = [];
  const shown = new Set<string>();
  const group = (title: string, list: readonly string[]) => {
    const items = list.filter((id) => byId.has(id) && !shown.has(id));
    if (!items.length) return;
    top.push({ kind: 'head', text: title });
    for (const id of items) {
      shown.add(id);
      top.push({ kind: 'item', choice: byId.get(id)!, fav: fav.has(id) });
    }
  };
  group('Favourites', favourites);
  group('Recent', recent);
  const rest = choices.filter((c) => !shown.has(c.id)).map((c): PickRow => ({ kind: 'item', choice: c, fav: fav.has(c.id) }));
  return top.length ? [...top, { kind: 'head', text: 'All assets' }, ...rest] : rest;
}

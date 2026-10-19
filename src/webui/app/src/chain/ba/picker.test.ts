// Picking an asset among hundreds (user feedback 2026-10-19): search by name or #number, favourites and the
// last picked on top, the keys, only the rows in view drawn, the number shown against look-alikes.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import AssetPicker from './AssetPicker.svelte';
import { matchScore, pickerRows, type PickChoice } from './picker';
import { picks, resetPicks } from './picks.svelte';
import { setTransport, type BridgeRequest } from '../../lib/bridge';

const hex = (n: number) => n.toString(16).padStart(64, '0');
const MANY: PickChoice[] = Array.from({ length: 600 }, (_, i) => ({ id: hex(i + 1), label: i === 7 ? 'GOLD' : i === 8 ? 'G0LD' : `TOKEN${i}`, seq: `${1700 + Math.floor(i / 100)}-${String(i % 100).padStart(4, '0')}`, held: String(i) }));

let calls: BridgeRequest[] = [];
beforeEach(() => {
  resetPicks();
  calls = [];
  setTransport(async (r) => {
    calls.push(r);
    if (r.method === 'prefs.get') return { assetFavourites: [hex(300)], assetRecent: [hex(42)] };
    return {};
  });
});
afterEach(() => setTransport(null));

describe('what the picker lists', () => {
  it('finds an asset by its name, its #number or the digits of it, best first', () => {
    const names = (q: string) => pickerRows(MANY, q, [], []).map((r) => (r.kind === 'item' ? r.choice.label : r.text));
    expect(names('gold')[0]).toBe('GOLD');
    expect(names('#1700-0008')).toEqual(['G0LD']);
    expect(names('#1700-0007')).toEqual(['GOLD']);
    expect(names('17000007')[0]).toBe('GOLD');
    expect(names('token59')[0]).toBe('TOKEN59');
    expect(names('nothing like it')).toEqual([]);
    expect(matchScore(MANY[7], 'GOLD')).toBeGreaterThan(matchScore(MANY[8], 'GOLD'));
  });

  it('puts the favourites and the last picked on top, then every asset', () => {
    const rows = pickerRows(MANY, '', [hex(300)], [hex(42), hex(300)]);
    expect(rows.slice(0, 5).map((r) => (r.kind === 'item' ? r.choice.id : r.text))).toEqual(['Favourites', hex(300), 'Recent', hex(42), 'All assets']);
    expect(rows.filter((r) => r.kind === 'item').length).toBe(600);
    expect(pickerRows(MANY, '', [], []).every((r) => r.kind === 'item')).toBe(true);
  });
});

describe('the picker', () => {
  it('draws only the rows in view of 600 assets, each with its number, and filters as one types', async () => {
    render(AssetPicker, { props: { id: 'p', label: 'Asset', choices: MANY } });
    await fireEvent.click(document.querySelector('#p')!);
    const options = () => [...document.querySelectorAll<HTMLElement>('[role="option"]')];
    await waitFor(() => expect(options().length).toBeGreaterThan(0));
    expect(options().length).toBeLessThan(30);
    expect(options()[0].textContent).toMatch(/#\d{4}-\d{4}/);
    const search = screen.getByRole('combobox', { name: 'Search assets' });
    await fireEvent.input(search, { target: { value: '#1700-0008' } });
    await waitFor(() => expect(options().map((o) => o.dataset.id)).toEqual([hex(9)]));
  });

  it('moves with the arrow keys and picks with Enter, then remembers it as recent', async () => {
    let picked = '';
    render(AssetPicker, { props: { id: 'p', label: 'Asset', choices: MANY, onpick: (id: string) => (picked = id) } });
    await fireEvent.click(document.querySelector('#p')!);
    const search = screen.getByRole('combobox', { name: 'Search assets' });
    await waitFor(() => expect(picks.favourites).toEqual([hex(300)]));
    await fireEvent.input(search, { target: { value: 'token1' } });
    await fireEvent.keyDown(search, { key: 'ArrowDown' });
    const activeId = search.getAttribute('aria-activedescendant')!;
    const expected = document.getElementById(activeId)!.dataset.id!;
    await fireEvent.keyDown(search, { key: 'Enter' });
    expect(picked).toBe(expected);
    expect(document.querySelector('[role="listbox"]')).toBeNull();
    expect((document.querySelector('#p') as HTMLElement).dataset.value).toBe(expected);
    expect(picks.recent[0]).toBe(expected);
    expect(calls.some((c) => c.method === 'prefs.set' && Array.isArray((c.params as Record<string, unknown>).assetRecent))).toBe(true);
  });

  it('stars a favourite, which then comes first', async () => {
    render(AssetPicker, { props: { id: 'p', label: 'Asset', choices: MANY } });
    await fireEvent.click(document.querySelector('#p')!);
    await waitFor(() => expect(picks.favourites).toEqual([hex(300)]));
    const star = document.querySelector<HTMLElement>(`[role="option"][data-id="${hex(1)}"] .star`)!;
    await fireEvent.click(star);
    expect(picks.favourites[0]).toBe(hex(1));
    expect(calls.some((c) => c.method === 'prefs.set' && (c.params as Record<string, unknown>).assetFavourites)).toBe(true);
    const ids = [...document.querySelectorAll<HTMLElement>('[role="option"]')].map((o) => o.dataset.id);
    expect(ids[0]).toBe(hex(1));
    await fireEvent.keyDown(screen.getByRole('combobox', { name: 'Search assets' }), { key: 'Escape' });
    expect(document.querySelector('[role="listbox"]')).toBeNull();
  });
});

// The pages of this chain of its own, beside those of the kit: the one file of the interface (with
// LocalPages.svelte, which shows them, and chain.json) that a chain made from the template changes, so
// that merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
//
// BitAssets: the assets of the chain (an explorer with each asset's supply, markets, auctions and history),
// what the wallet holds and controls, the wizard that issues an asset, trading in the pools (swaps and
// liquidity) and the Dutch auctions; their components in LocalPages.svelte (chain/pages, chain/ba), their RPC
// methods in src/webui/local.cpp, the cards of the home in LocalCards.svelte, their icons in icons.ts, and
// sending an asset in the Send page (LocalSend.svelte).
//
// The template has none. A sidechain adds an entry per page, for example
//
//   { id: 'names', title: 'Names', icon: 'book', section: 'chain', summary: 'Register and manage names', wallet: true,
//     quick: { label: 'Register a name' }, keywords: 'bitnames register' }
//
// then its component in LocalPages.svelte, the RPC methods it calls in src/webui/local.cpp, and a
// command or a dashboard card if it wants one (LOCAL_COMMANDS, LocalCards in LocalPages.svelte).
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  /** Its name in Simple mode, in plain words, where it differs from the title ("My bets" for Portfolio). */
  simpleTitle?: string;
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
  /**
   * One of the chain's main features, in the navigation of Simple mode (at most three pages; the rule is in
   * doc/webui.md, "Simple and Advanced modes"). It then needs a `simpleSummary`.
   */
  simple?: boolean;
  /** What it is for in Simple mode, in a few plain words (under its name in the sidebar of Simple mode). */
  simpleSummary?: string;
  /**
   * Not a page of its own in the navigation: a tab of another page of the chain (BitAssets: Register new asset
   * under My assets). go('<id>') and its commands lead there (to `tab`, then the tab asked for, "issue/…").
   */
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  // "Explore assets" (user feedback 2026-10-19: told apart from My assets); its id, links and shortcut stay.
  { id: 'assets', title: 'Explore assets', icon: 'gem', section: 'chain', shortcut: 'Alt+6', summary: 'Every asset of the chain: its supply, markets, auctions and history', keywords: 'bitassets explore token find search supply', quick: { label: 'Explore assets', icon: 'gem' } },
  { id: 'my-assets', title: 'My assets', simple: true, simpleSummary: 'The tokens you hold', icon: 'holdings', section: 'chain', summary: 'What this wallet holds and controls: balances, pool shares, auctions to collect, activity', wallet: true, keywords: 'portfolio balances holdings mint burn control liquidity receipts activity' },
  // Shown as "Register new asset" (user decision, 2026-10-15); its id stays issue.
  { id: 'issue', title: 'Register new asset', simpleTitle: 'Create a token', simpleSummary: 'Make a token of your own', simple: true, icon: 'issue', section: 'chain', summary: 'Create an asset of your own, with its supply, decimals and description', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Register new asset', icon: 'issue', simpleLabel: 'Create a token' } },
  { id: 'trade', title: 'Trade', simpleTitle: 'Swap', simple: true, simpleSummary: 'Trade one token for another', icon: 'swap', section: 'chain', summary: 'Swap assets in their pools, and provide liquidity for a share of the fees', keywords: 'swap exchange amm pool liquidity price dex', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'auctions', title: 'Auctions', icon: 'gavel', section: 'chain', summary: 'Buy at a price that falls block by block, or sell by Dutch auction', keywords: 'dutch auction bid sell buy', advanced: true },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'ba.issue', label: 'Register new asset…', page: 'issue', tip: 'Reserve a name and register an asset with its supply', keywords: 'create new token' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another in their pool', keywords: 'exchange trade' },
  { id: 'ba.liquidity', label: 'Add liquidity…', page: 'trade', tab: 'liquidity', tip: 'Put two assets in their pool for a share of its fees', keywords: 'pool provide' },
  { id: 'ba.sell', label: 'Sell by auction…', page: 'auctions', tab: 'sell', tip: 'Start a Dutch auction of an asset', keywords: 'dutch auction' },
  { id: 'ba.activity', label: 'Asset activity', page: 'my-assets', tab: 'activity', tip: 'The asset transactions of this wallet', keywords: 'history transactions' },
] as const satisfies readonly LocalCommand[];

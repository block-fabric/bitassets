// BitAssets as the pages see it: the shapes of what the node and the wallet answer (src/rpc/bitassets.cpp,
// src/wallet/rpc/bitassets.cpp), amounts in units of an asset's decimals (bigint, exact), the rules of names
// and data as the chain checks them (src/bitassets/state.cpp, rpcutil.cpp), and the arithmetic of the pools
// and the auctions as the chain does it (amm::SwapOut, amm::Provide, Auction::PriceAt...), for the previews.
// Pure: no state, no calls (tested in chain/bitassets.test.ts).

/** The id of CHN, the coin of the chain: the null hash. */
export const CHN_ID = '0'.repeat(64);
export const CHN_DECIMALS = 8;
/** Limits of the chain (src/bitassets/state.h). */
export const MAX_NAME = 64;
export const MAX_INFO = 512;
export const MAX_DECIMALS = 12;
export const MAX_AMOUNT = (1n << 63n) - 1n;
export const MIN_LIQUIDITY = 1000n;
export const MIN_OPEN_SHARES = 100_000n;
export const MIN_OPEN_CHN = 1_000_000n;
export const MIN_CHN_PAYOUT = 50_000n;
export const SWAP_FEE_PER_MILLE = 3n;
export const MAX_AUCTION_DURATION = 1_000_000;
export const MAX_AUCTION_DELAY = 52_560;
/** What the wallet refuses by default (swapasset max_impact), and what the page asks about first. */
export const MAX_IMPACT = 10;
export const WARN_IMPACT = 2;

// --- What the node answers ---

export interface BitAssetsInfo {
  assets: number;
  reservations: number;
  pools: number;
  auctions: number;
  /** Auctions, those collected included (older nodes: absent). */
  auctions_all?: number;
  /** The height of the next block, which the prices are for. */
  height: number;
  reveal_depth: number;
  /** The first block of the logos of assets (-1: never on this network; older nodes: absent). */
  logo_height?: number;
  max_logo_bytes?: number;
}

export interface AssetData {
  info?: string;
  commitment?: string;
  ipv4?: string;
  ipv6?: string;
  encryptionkey?: string;
  signingkey?: string;
}

export const DATA_FIELDS = ['info', 'commitment', 'ipv4', 'ipv6', 'encryptionkey', 'signingkey'] as const;
export type DataField = (typeof DATA_FIELDS)[number];

/**
 * An amount of an asset as the node gives it, in whole assets with its decimals: a number, or the text of its digits
 * when a double cannot hold it (past 2^53 units: the results of EXACT_METHODS are read exactly, lib/exactjson.ts).
 * Exact through unitsOf / formatAmount; approx() for a ratio or an order only.
 */
export type Amount = number | string;

/** The RPC methods of BitAssets whose results carry amounts: read exactly (audit 7 E-BA6). */
export const EXACT_METHODS = ['listassets', 'getasset', 'listpools', 'getpool', 'listauctions', 'getauction', 'listmyassets', 'listassetactivity', 'listassettransactions', 'getbitassetsinfo', 'getassethistory', 'quoteswap', 'quotebid', 'swapasset', 'addliquidity', 'removeliquidity', 'bidauction'];

/** An amount as a double: for a ratio, a chart, an order; never for what is shown or sent. */
export function approx(value: Amount | undefined | null): number {
  return value === undefined || value === null ? 0 : typeof value === 'number' ? value : Number(value);
}

export interface Asset {
  /** The hash of its name (hex). */
  asset: string;
  /** Its name, if its registration published it. Text of the chain. */
  name?: string;
  /** Its name, or "0x" and its hash. Text of the chain. */
  label: string;
  /** Its number: 1739-0029. */
  seq: string;
  decimals: number;
  supply: Amount;
  minted: Amount;
  burned: Amount;
  fixed: boolean;
  registered: number;
  registration: string;
  data?: AssetData;
  releasable: boolean;
  /** The height of the last block that touched it (searchassets; absent from the list methods). */
  last_activity?: number;
  release_fee?: Amount;
  control?: { txid: string; vout: number; address?: string };
  /** Its logo now (getassetlogo has its bytes; the pages ask the host for it: logos.svelte.ts). */
  logo?: AssetLogo;
}

export interface AssetLogo {
  /** SHA-256 of its bytes (hex). */
  hash: string;
  mime: 'image/png' | 'image/jpeg';
  size: number;
  height: number;
  txid: string;
}

export interface Pool {
  pool: string;
  asset_a: string;
  asset_b: string;
  asset_a_id: string;
  asset_b_id: string;
  reserve_a: Amount;
  reserve_b: Amount;
  /** What one of the first asset is worth in the second. */
  price?: number;
  shares: Amount;
  volume_a: Amount;
  volume_b: Amount;
  swaps: number;
  created: number;
  abandoned: boolean;
}

export type AuctionStatus = 'upcoming' | 'open' | 'sold out' | 'ended' | 'closed';

export interface Auction {
  auction: string;
  base: string;
  quote: string;
  base_id: string;
  quote_id: string;
  amount: Amount;
  remaining: Amount;
  proceeds: Amount;
  start_price: Amount;
  end_price: Amount;
  unit_price: number;
  price: Amount;
  cost_of_remaining: Amount;
  start_height: number;
  end_height: number;
  bids: number;
  status: AuctionStatus;
  receipt?: { txid: string; vout: number; address?: string };
}

export interface Holding {
  asset: string;
  label: string;
  decimals: number;
  balance: Amount;
  pending: Amount;
  outputs: number;
  control: boolean;
  control_pending: boolean;
  supply: Amount;
  fixed: boolean;
}

export interface Reservation {
  txid: string;
  name?: string;
  taken: boolean;
  height?: number;
  registers_from?: number;
  wait?: number;
}

export interface Position {
  pool: string;
  asset_a: string;
  asset_b: string;
  asset_a_id: string;
  asset_b_id: string;
  shares: Amount;
  percent: number;
  value_a: Amount;
  value_b: Amount;
}

export interface Holdings {
  assets: Holding[];
  reservations: Reservation[];
  liquidity: Position[];
  receipts: { auction: string }[];
}

export interface Activity {
  txid: string;
  time: number;
  confirmations: number;
  operation: string;
  summary: string;
}

/** A side of an asset transfer: an address, and whether it is this wallet's. */
export interface TxParty {
  /** "unknown" where the outputs a transaction spent cannot be read (its block pruned). */
  address: string;
  mine: boolean;
}

/** An asset a transaction of the wallet moved (listassettransactions): one entry per asset. */
export interface AssetTx {
  txid: string;
  time: number;
  confirmations: number;
  blockhash?: string;
  blockheight?: number;
  category: 'send' | 'receive' | 'register' | 'mint' | 'burn' | 'swap' | 'pool_add' | 'pool_remove' | 'auction' | 'bid' | 'collect' | 'logo' | 'update' | 'control' | 'reserve' | 'release' | string;
  asset?: string;
  label?: string;
  seq?: string;
  decimals?: number;
  amount?: Amount;
  sign: -1 | 0 | 1;
  at_least?: boolean;
  at_most?: boolean;
  fee?: Amount;
  to: TxParty[];
  from: TxParty[];
  abandoned: boolean;
}

export interface SwapQuote {
  amount_in: Amount;
  amount_out: Amount;
  price: number;
  spot_price: number;
  price_impact: number;
  fee: Amount;
  abandoned: boolean;
}

export interface BidQuote {
  buys: Amount;
  remaining: Amount;
  cost_of_remaining: Amount;
  open: boolean;
}

export type AssetHistory = Partial<Record<DataField, { value: string | null; txid: string; height: number }[]>>;

// --- Ids ---

export function isChn(id: string): boolean {
  return /^0+$/.test(id);
}

/**
 * How the pages name an asset to the commands: "CHN", or "0x" and its hash; never by its label, which an
 * asset registered under a name like "CHN" or like the number of another shares (as chains-qt did).
 */
export function assetArg(id: string): string {
  return isChn(id) ? 'CHN' : `0x${id}`;
}

/** The id of an asset of this name (bitassets::HashName): its SHA-256, written as the node writes a hash (reversed). */
export async function hashName(name: string): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(name)));
  return Array.from(digest.reverse(), (b) => b.toString(16).padStart(2, '0')).join('');
}

/** A hue for an asset (its avatar), from its hash: the same everywhere. */
export function assetHue(id: string): number {
  if (isChn(id)) return 32;
  return parseInt(id.slice(0, 6), 16) % 360;
}

/** Two letters for an avatar: the start of the name, or of the hash. */
export function initials(label: string): string {
  if (label.startsWith('0x') && label.length > 10) return label.slice(2, 4).toUpperCase();
  const letters = [...label.replace(/[^\p{L}\p{N}]/gu, '')];
  return (letters.slice(0, 2).join('') || '?').toUpperCase();
}

// --- Amounts ---

/** The digits of a number as the node wrote it (JSON numbers of up to 19 digits, exact as text). */
export function numberText(value: number | string, decimals: number): string {
  if (typeof value === 'string') return value;
  if (!Number.isFinite(value)) return '0';
  const text = Math.abs(value) < 1e21 ? value.toFixed(Math.min(decimals, 20)) : BigInt(Math.round(value)).toString();
  return text;
}

/** The units (the smallest amounts) of an amount, with the decimals of its asset. */
export function unitsOf(value: number | string, decimals: number): bigint {
  const text = numberText(value, decimals).trim();
  const m = /^(-?)(\d*)(?:\.(\d*))?$/.exec(text);
  if (!m) return 0n;
  const whole = m[2] || '0';
  const frac = (m[3] ?? '').padEnd(decimals, '0').slice(0, decimals);
  const units = BigInt(whole) * 10n ** BigInt(decimals) + (decimals ? BigInt(frac || '0') : 0n);
  return m[1] ? -units : units;
}

/**
 * How far the units of an amount the node gave as a JSON number may be off: a number past 2^53 units is
 * a double, not the exact digits the node wrote (audit 7 E-BA6). Zero when exact.
 */
export function unitsError(value: number | string, decimals: number): bigint {
  if (typeof value === 'string' || !Number.isFinite(value) || value === 0) return 0n;
  const scaled = Math.abs(value) * 10 ** decimals;
  if (scaled <= Number.MAX_SAFE_INTEGER) return 0n;
  // One unit in the last place of the double, in units, and one more for the rounding to decimals.
  const ulp = 2 ** (Math.floor(Math.log2(Math.abs(value))) - 52);
  return BigInt(Math.ceil(ulp * 10 ** decimals)) + 1n;
}

/** The units of an amount, never more than the node meant (a balance: "Max"). */
export function unitsAtMost(value: number | string, decimals: number): bigint {
  const units = unitsOf(value, decimals) - unitsError(value, decimals);
  return units < 0n ? 0n : units;
}

/** The units of an amount, never less than the node meant (a cap: what buys all that is left). */
export function unitsAtLeast(value: number | string, decimals: number): bigint {
  return unitsOf(value, decimals) + unitsError(value, decimals);
}

/** An amount in units, written with its decimals ("12.50"), the thousands grouped or not, trailing zeros trimmed or not. */
export function formatUnits(units: bigint, decimals: number, options: { group?: boolean; trim?: boolean; max?: number } = {}): string {
  const group = options.group ?? true;
  const trim = options.trim ?? true;
  const negative = units < 0n;
  const abs = negative ? -units : units;
  const scale = 10n ** BigInt(decimals);
  let whole = (abs / scale).toString();
  let frac = decimals ? (abs % scale).toString().padStart(decimals, '0') : '';
  if (options.max !== undefined && frac.length > options.max) {
    frac = frac.slice(0, options.max);
  }
  if (trim) frac = frac.replace(/0+$/, '');
  if (group) whole = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  return `${negative ? '-' : ''}${whole}${frac ? `.${frac}` : ''}`;
}

/** An amount the node gave, formatted with the decimals of its asset. */
export function formatAmount(value: number | string, decimals: number, options: { group?: boolean; trim?: boolean; max?: number } = {}): string {
  return formatUnits(unitsOf(value, decimals), decimals, options);
}

/** The text to give the node for an amount in units: plain, exact. */
export function amountParam(units: bigint, decimals: number): string {
  return formatUnits(units, decimals, { group: false, trim: true });
}

export type UnitsParse = { ok: true; units: bigint } | { ok: false; error: string };

/**
 * An amount typed for an asset with `decimals` decimals (bitassets::ParseUnits): a number with at most that
 * many decimals, above zero (or zero, if allowed), at most the largest amount the chain takes.
 */
export function parseUnits(text: string, decimals: number, options: { allowZero?: boolean } = {}): UnitsParse {
  const s = text.trim().replace(/,/g, '');
  if (s === '') return { ok: false, error: 'Enter an amount.' };
  const m = /^(\d*)(?:\.(\d*))?$/.exec(s);
  if (!m || (m[1] === '' && (m[2] ?? '') === '')) return { ok: false, error: 'Enter a number, such as 12.5.' };
  const frac = m[2] ?? '';
  if (frac.replace(/0+$/, '').length > decimals) {
    return { ok: false, error: decimals === 0 ? 'This asset has no decimals: enter a whole number.' : `At most ${decimals} decimals.` };
  }
  const units = BigInt(m[1] || '0') * 10n ** BigInt(decimals) + (decimals ? BigInt(frac.padEnd(decimals, '0').slice(0, decimals) || '0') : 0n);
  if (units > MAX_AMOUNT) return { ok: false, error: 'Too large.' };
  if (units === 0n && !options.allowZero) return { ok: false, error: 'The amount is zero.' };
  return { ok: true, units };
}

/** A price as the node gives it (up to 12 significant decimals), readably: grouped, at most `digits` significant figures. */
export function formatPrice(price: number | undefined | null, digits = 6): string {
  if (price === undefined || price === null || !Number.isFinite(price) || price <= 0) return '–';
  if (price >= 1) {
    const fixed = price >= 1e6 ? price.toFixed(0) : price.toFixed(Math.max(0, digits - Math.floor(Math.log10(price)) - 1));
    const [w, f] = fixed.split('.');
    const trimmed = f ? f.replace(/0+$/, '') : '';
    return w.replace(/\B(?=(\d{3})+(?!\d))/g, ',') + (trimmed ? `.${trimmed}` : '');
  }
  return Number(price.toPrecision(digits)).toFixed(Math.min(20, digits - Math.floor(Math.log10(price)) - 1)).replace(/\.?0+$/, '');
}

/**
 * The price of one `base` in `quote`, readably both ways: when one unit is worth almost nothing, the other
 * way round (as chains-qt wrote it: "1 CHN buys 400,000 PEBBLES").
 */
export function unitPrice(price: number | undefined | null): { kind: 'each'; price: string } | { kind: 'buys'; amount: string } | { kind: 'none' } {
  if (price === undefined || price === null || !Number.isFinite(price) || price <= 0) return { kind: 'none' };
  if (price >= 0.0001) return { kind: 'each', price: formatPrice(price) };
  return { kind: 'buys', amount: formatPrice(1 / price, 6) };
}

/** A share in percent, with two decimals below 10. */
export function formatPercent(p: number): string {
  if (!Number.isFinite(p)) return '–';
  const a = Math.abs(p);
  return `${a >= 10 ? p.toFixed(1) : a >= 0.01 || a === 0 ? p.toFixed(2) : '<0.01'}%`;
}

// --- Names and data, as the chain checks them ---

/** bitassets::IsAssetName: 1 to 64 printable characters (ASCII), no space at either end. */
export function isAssetName(name: string): boolean {
  if (name.length === 0 || name.length > MAX_NAME || name.startsWith(' ') || name.endsWith(' ')) return false;
  for (const c of name) {
    const code = c.codePointAt(0)!;
    if (code < 0x20 || code > 0x7e) return false;
  }
  return true;
}

/** bitassets::ReadsAsAnotherAsset: CHN in any case, "0x" first, or groups of four digits (two or three). */
export function readsAsAnotherAsset(name: string): boolean {
  if (name.toLowerCase() === 'chn') return true;
  if (/^0[xX]/.test(name)) return true;
  return /^\d{4}-\d{4}(-\d{4})?$/.test(name);
}

/**
 * bitassets::NameMimicsNumber (wallet and relay policy): '#', '(' or ')', or four digits, a dash and four digits
 * anywhere: shown with its number, such a name would read as the number of another asset (audit 7 E-BA4).
 */
export function nameMimicsNumber(name: string): boolean {
  return /[#()]/.test(name) || /\d{4}-\d{4}/.test(name);
}

/** What is wrong with a name for a new asset ('' when nothing). */
export function nameProblem(name: string): string {
  if (name === '') return '';
  if (name.length > MAX_NAME) return `A name is at most ${MAX_NAME} characters.`;
  if (name.startsWith(' ') || name.endsWith(' ')) return 'A name has no space at either end.';
  if (!isAssetName(name)) return 'A name is plain characters: letters, digits, punctuation and spaces (ASCII).';
  if (name.toLowerCase() === 'chn') return 'CHN is the coin of the chain.';
  if (/^0[xX]/.test(name)) return 'A name does not start with "0x": that is how a hash is written.';
  if (readsAsAnotherAsset(name)) return 'A name does not read as the number of an asset (such as 1739-0029).';
  if (nameMimicsNumber(name)) return 'A name holds no "#", "(" or ")", nor four digits, a dash and four digits: shown with its number, it would read as the number of another asset.';
  return '';
}

/** Whether text reads as an asset's number (1739-0029, or 0001-1739-0029). */
export function isSeq(text: string): boolean {
  return /^\d{4}-\d{4}(-\d{4})?$/.test(text.trim());
}

function isHex(text: string, bytes: number): boolean {
  return new RegExp(`^[0-9a-fA-F]{${bytes * 2}}$`).test(text);
}

function ipv4Problem(text: string): boolean {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$/.exec(text);
  if (!m) return true;
  if (m.slice(1, 5).some((x) => Number(x) > 255)) return true;
  return m[5] !== undefined && Number(m[5]) > 65535;
}

function ipv6Problem(text: string): boolean {
  let host = text;
  const bracket = /^\[([^\]]+)\](?::(\d{1,5}))?$/.exec(text);
  if (bracket) {
    host = bracket[1];
    if (bracket[2] !== undefined && Number(bracket[2]) > 65535) return true;
  }
  if (!/^[0-9a-fA-F:.]+$/.test(host) || !host.includes(':')) return true;
  const parts = host.split('::');
  if (parts.length > 2) return true;
  const groups = parts.flatMap((p) => (p === '' ? [] : p.split(':')));
  if (groups.some((g) => !/^[0-9a-fA-F]{1,4}$/.test(g) && !/^\d+\.\d+\.\d+\.\d+$/.test(g))) return true;
  return parts.length === 1 ? groups.length !== 8 : groups.length > 7;
}

/** What is wrong with each field of an asset's data (bitassets::ParseUpdates), by field; empty fields are left out. */
export function dataProblems(data: Partial<Record<DataField, string>>): Partial<Record<DataField, string>> {
  const out: Partial<Record<DataField, string>> = {};
  const v = (k: DataField) => (data[k] ?? '').trim();
  if (data.info !== undefined && new TextEncoder().encode(data.info).length > MAX_INFO) out.info = `At most ${MAX_INFO} bytes.`;
  if (v('commitment') && !isHex(v('commitment'), 32)) out.commitment = 'A hash: 64 hexadecimal digits.';
  if (v('ipv4') && ipv4Problem(v('ipv4'))) out.ipv4 = 'An IPv4 address, such as 203.0.113.7, with a port or not.';
  if (v('ipv6') && ipv6Problem(v('ipv6'))) out.ipv6 = 'An IPv6 address, such as 2001:db8::7, or in brackets with a port.';
  if (v('encryptionkey') && !(isHex(v('encryptionkey'), 33) && /^0[23]/.test(v('encryptionkey')))) out.encryptionkey = 'A compressed public key: 66 hexadecimal digits, starting 02 or 03.';
  if (v('signingkey') && !isHex(v('signingkey'), 32)) out.signingkey = 'An x-only public key: 64 hexadecimal digits.';
  return out;
}

/** The data to register: the fields given, trimmed (the description as typed). */
export function dataToSet(data: Partial<Record<DataField, string>>): AssetData {
  const out: AssetData = {};
  for (const k of DATA_FIELDS) {
    const value = k === 'info' ? (data[k] ?? '') : (data[k] ?? '').trim();
    if (value.trim() !== '') out[k] = value;
  }
  return out;
}

/**
 * The changes of an asset's data (updateasset): a field changed is set, a field emptied is deleted (null),
 * a field as it was is left out.
 */
export function dataUpdates(before: AssetData | undefined, after: Partial<Record<DataField, string>>): Record<string, string | null> {
  const out: Record<string, string | null> = {};
  for (const k of DATA_FIELDS) {
    const was = before?.[k] ?? '';
    const now = k === 'info' ? (after[k] ?? '') : (after[k] ?? '').trim();
    if (now === was || (now.trim() === '' && was === '')) continue;
    out[k] = now.trim() === '' ? null : now;
  }
  return out;
}

// --- Pools: the constant product of the chain (amm::*) ---

/** amm::SwapOut: what `amountIn` buys from a pool, the fee (0.3%) left in it. */
export function swapOut(reserveIn: bigint, reserveOut: bigint, amountIn: bigint): bigint {
  if (reserveIn <= 0n || reserveOut <= 0n || amountIn <= 0n) return 0n;
  const withFee = amountIn * (1000n - SWAP_FEE_PER_MILLE);
  return (withFee * reserveOut) / (reserveIn * 1000n + withFee);
}

/** amm::SwapIn: what has to go in for `amountOut` to come out; null when the pool does not hold that much. */
export function swapIn(reserveIn: bigint, reserveOut: bigint, amountOut: bigint): bigint | null {
  if (reserveIn <= 0n || amountOut <= 0n || amountOut >= reserveOut) return null;
  const amount = (reserveIn * amountOut * 1000n) / ((reserveOut - amountOut) * (1000n - SWAP_FEE_PER_MILLE)) + 1n;
  return amount > MAX_AMOUNT ? null : amount;
}

/** amm::PriceImpact: how much worse than the spot price the trade's price is, in percent, the fee included. */
export function priceImpact(reserveIn: bigint, reserveOut: bigint, amountIn: bigint, amountOut: bigint): number {
  if (reserveIn <= 0n || reserveOut <= 0n || amountIn <= 0n) return 100;
  const impact = 100 * (1 - Number(amountOut) / Number(amountIn) / (Number(reserveOut) / Number(reserveIn)));
  return Math.min(100, Math.max(0, impact));
}

/** The least a trade gives, less the slippage (in percent): at least 1 unit (the wallet's LessSlippage). */
export function lessSlippage(amount: bigint, slippagePercent: number): bigint {
  const bps = BigInt(Math.round(slippagePercent * 100));
  const out = (amount * (10000n - bps)) / 10000n;
  return out < 1n ? 1n : out;
}

/** Whether a pool holds only the shares kept for good: everyone left it (amm::Abandoned). */
export function isAbandoned(shares: bigint): boolean {
  return shares <= MIN_LIQUIDITY;
}

function isqrt(n: bigint): bigint {
  if (n < 2n) return n;
  let x = BigInt(Math.floor(Math.sqrt(Number(n))));
  while (x * x > n) x -= 1n;
  while ((x + 1n) * (x + 1n) <= n) x += 1n;
  return x;
}

export interface PoolUnits {
  reserve0: bigint;
  reserve1: bigint;
  shares: bigint;
  /** Whether the first side is CHN (dust of CHN stays in the pool). */
  chn0?: boolean;
  chn1?: boolean;
}

export interface DepositPreview {
  shares: bigint;
  /** What goes in of each side. */
  take0: bigint;
  take1: bigint;
  /** Whether the deposit makes the pool, or reopens it: its amounts set the price. */
  opens: boolean;
}

/** amm::Provide: the shares a deposit gives, and what of it goes in. A new pool is reserves and shares of 0. */
export function provide(pool: PoolUnits, amount0: bigint, amount1: bigint): DepositPreview | null {
  if (amount0 <= 0n || amount1 <= 0n || amount0 > MAX_AMOUNT || amount1 > MAX_AMOUNT) return null;
  if (isAbandoned(pool.shares)) {
    const total = isqrt((pool.reserve0 + amount0) * (pool.reserve1 + amount1));
    if (total <= MIN_LIQUIDITY) return null;
    return { shares: total - MIN_LIQUIDITY, take0: amount0, take1: amount1, opens: true };
  }
  if (pool.reserve0 === 0n || pool.reserve1 === 0n) return null;
  const s0 = (amount0 * pool.shares) / pool.reserve0;
  const s1 = (amount1 * pool.shares) / pool.reserve1;
  const shares = s0 < s1 ? s0 : s1;
  if (shares === 0n || shares > MAX_AMOUNT) return null;
  const worth = (reserve: bigint) => (shares * reserve + pool.shares - 1n) / pool.shares;
  let take0 = amount0;
  let take1 = amount1;
  if (s0 <= s1) take1 = worth(pool.reserve1);
  else take0 = worth(pool.reserve0);
  if (pool.chn0 && amount0 - take0 < MIN_CHN_PAYOUT) take0 = amount0;
  if (pool.chn1 && amount1 - take1 < MIN_CHN_PAYOUT) take1 = amount1;
  return { shares, take0, take1, opens: false };
}

/** amm::WithdrawPaid: what shares take out of a pool, CHN dust left in it. */
export function withdraw(pool: PoolUnits, shares: bigint): [bigint, bigint] {
  if (pool.shares <= 0n || shares > pool.shares) return [0n, 0n];
  let out0 = (pool.reserve0 * shares) / pool.shares;
  let out1 = (pool.reserve1 * shares) / pool.shares;
  if (pool.chn0 && out0 < MIN_CHN_PAYOUT) out0 = 0n;
  if (pool.chn1 && out1 < MIN_CHN_PAYOUT) out1 = 0n;
  return [out0, out1];
}

/**
 * The second amount of a deposit into a pool someone provides liquidity to, at the pool's price, rounded up
 * so that the first amount is the one that counts (as addliquidity works it out).
 */
export function pairedAmount(amountA: bigint, reserveA: bigint, reserveB: bigint): bigint {
  if (reserveA <= 0n) return 0n;
  return (amountA * reserveB + reserveA - 1n) / reserveA;
}

/**
 * Points of the curve x·y = k of a pool, for its chart: the reserves of the first asset from a fifth to five
 * times what it holds, and what the second then holds.
 */
export function curvePoints(reserveA: number, reserveB: number, n = 48): { x: number; y: number }[] {
  if (!(reserveA > 0 && reserveB > 0)) return [];
  const k = reserveA * reserveB;
  const lo = Math.log(reserveA / 5);
  const hi = Math.log(reserveA * 5);
  return Array.from({ length: n + 1 }, (_, i) => {
    const x = Math.exp(lo + ((hi - lo) * i) / n);
    return { x, y: k / x };
  });
}

// --- Auctions (bitassets::Auction) ---

export interface AuctionUnits {
  baseAmount: bigint;
  remaining: bigint;
  startPrice: bigint;
  endPrice: bigint;
  startHeight: number;
  /** The last block that takes bids. */
  endHeight: number;
}

/** Auction::PriceAt: the price of all of it at a height, in a straight line from the start to the end, rounded up. */
export function auctionPriceAt(a: AuctionUnits, height: number): bigint {
  const duration = a.endHeight - a.startHeight + 1;
  if (duration <= 1 || height <= a.startHeight) return a.startPrice;
  if (height >= a.endHeight) return a.endPrice;
  const drop = ((a.startPrice - a.endPrice) * BigInt(height - a.startHeight)) / BigInt(duration - 1);
  return a.startPrice - drop;
}

/** Auction::BuysAt: what an amount buys at a height, rounded down. */
export function auctionBuysAt(a: AuctionUnits, height: number, amount: bigint): bigint {
  const price = auctionPriceAt(a, height);
  if (price === 0n) return 0n;
  const units = (amount * a.baseAmount) / price;
  return units > MAX_AMOUNT ? MAX_AMOUNT : units;
}

/** Auction::CostOfRemaining: the least that buys all that is left at a height. */
export function auctionCostOfRemaining(a: AuctionUnits, height: number): bigint {
  if (a.baseAmount === 0n) return 0n;
  return (a.remaining * auctionPriceAt(a, height) + a.baseAmount - 1n) / a.baseAmount;
}

/** The share of an auction sold, and of its time gone, from 0 to 1. */
export function auctionProgress(a: Pick<Auction, 'amount' | 'remaining' | 'start_height' | 'end_height'>, height: number): { sold: number; time: number } {
  const sold = approx(a.amount) > 0 ? 1 - approx(a.remaining) / approx(a.amount) : 0;
  const span = a.end_height - a.start_height + 1;
  const time = height < a.start_height ? 0 : span > 0 ? Math.min(1, (height - a.start_height + 1) / span) : 1;
  return { sold: Math.max(0, Math.min(1, sold)), time };
}

/** What a receipt's holder can do with an auction: collect it, cancel it (no bid yet), or wait. */
export function receiptAction(a: Pick<Auction, 'status' | 'bids'>): 'collect' | 'cancel' | 'wait' | 'done' {
  if (a.status === 'closed') return 'done';
  if (a.status === 'sold out' || a.status === 'ended') return 'collect';
  if (a.bids === 0) return 'cancel';
  return 'wait';
}

/** The words of a status of an auction (shown with t()). */
export const AUCTION_STATUS: Record<AuctionStatus, string> = { upcoming: 'Upcoming', open: 'Open', 'sold out': 'Sold out', ended: 'Ended', closed: 'Collected' };

/** Problems of an auction to create, in the order of the form. */
export function auctionProblems(f: {
  sameAsset: boolean;
  amount: UnitsParse;
  start: UnitsParse;
  end: UnitsParse;
  duration: number;
  startIn: number;
  sellsChn: boolean;
  held?: bigint | null;
}): string[] {
  const out: string[] = [];
  if (f.sameAsset) out.push('An auction sells one asset for another.');
  if (!f.amount.ok) out.push(f.amount.error);
  else if (f.held !== undefined && f.held !== null && f.amount.units > f.held) out.push('This wallet holds less than that.');
  else if (f.sellsChn && f.amount.units < MIN_CHN_PAYOUT) out.push('An auction sells 0.0005 CHN at least (the least CHN paid out).');
  if (!f.start.ok) out.push(`Start price: ${f.start.error}`);
  if (!f.end.ok) out.push(`End price: ${f.end.error}`);
  if (f.start.ok && f.end.ok && f.end.units > f.start.units) out.push('The price falls: the end price is at most the start price.');
  if (!Number.isInteger(f.duration) || f.duration < 1 || f.duration > MAX_AUCTION_DURATION) out.push(`It runs 1 to ${MAX_AUCTION_DURATION.toLocaleString('en-US')} blocks.`);
  if (!Number.isInteger(f.startIn) || f.startIn < 1 || f.startIn > MAX_AUCTION_DELAY) out.push(`It starts 1 to ${MAX_AUCTION_DELAY.toLocaleString('en-US')} blocks from now.`);
  return out;
}

// --- Issuing: the two steps ---

export type IssueStage = 'reserving' | 'waiting' | 'registering' | 'registered' | 'taken';

/** Where the creation of an asset is: from the reservation listed by the wallet, and the asset once registered. */
export function issueStage(r: Reservation | undefined, registered: boolean): IssueStage {
  if (registered) return 'registered';
  if (!r) return 'reserving';
  if (r.taken) return 'taken';
  if ((r.wait ?? 0) > 0) return 'waiting';
  return 'registering';
}

/** The smallest amount of an asset with these decimals: 0.01 for 2. */
export function smallestAmount(decimals: number): string {
  return decimals === 0 ? '1' : `0.${'0'.repeat(decimals - 1)}1`;
}

// --- Activity: the operations of listassetactivity ---

/** The words of an operation of listassetactivity (shown with t()). */
export const OPERATIONS: Record<string, string> = {
  reserve: 'Reserve',
  register: 'Register',
  mint: 'Mint',
  update: 'Update',
  burn: 'Burn',
  swap: 'Swap',
  'add liquidity': 'Add liquidity',
  'remove liquidity': 'Remove liquidity',
  auction: 'Auction',
  bid: 'Bid',
  collect: 'Collect',
  release: 'Retire',
  transfer: 'Transfer',
};

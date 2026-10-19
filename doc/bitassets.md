# BitAssets: assets on a sidechain

BitAssets is a sidechain of Chains (slot 4) on which anyone can create assets — shares, points,
tickets, stablecoins — and trade them: in pools (an automated market maker) and by Dutch auction.
It is inspired by **BitAssets by LayerTwo Labs** (plain-bitassets), built anew on this Bitcoin Core
based sidechain, with Bitcoin's keys and transactions, and without the defects of the original.

## Assets

- **Creating one takes two steps**, so that nobody can see a name coming and take it first. A
  *reservation* commits to the name without showing it (HMAC-SHA256, under a nonce only the wallet
  can make, of the name's hash and of the script of the output that carries the reservation); once
  it is in a block, the *registration* reveals the nonce and creates the asset: its initial supply
  (zero is allowed) and its **control coin**. The app (`bitassets-app`, its Issue page) does both steps.
  Bound to its output's script, a reservation cannot be copied to any use: a copy made by someone
  else, with an output of their own, commits to nothing they can reveal.
- **A reservation is revealed only once it is deep enough** (`bitassets_reveal_depth` blocks: 6 on
  the main network, 3 on the test network and signet, 2 on regtest, as for BitNames): a
  registration in block *h* of a reservation made in block *r* needs *h - r* ≥ the depth. Whoever
  makes a block sees every registration waiting for it; without this, it could reserve the same
  name under a nonce of its own and register it in its own block, ahead of the one it saw.
  `registerasset` refuses to register sooner and says from which block it can; `listmyassets`
  gives each reservation's `registers_from` and `wait`, and `getbitassetsinfo` the depth
  (`reveal_depth`). The app waits as long by itself, and registers then.
- **A name may not read as another asset**: not CHN (in any case), not like a number
  (`1739-0029`, `0001-1739-0029`), not starting with `0x`.
- **Nor carry another asset's number** where names are shown with their numbers (`GOLD (#1739-0029)`):
  the wallet does not reserve or publish a name holding `#`, `(`, `)` or four digits, a dash and
  four digits, and the mempool does not relay the registration of one (`ba-name-look-alike`). This
  is policy, not consensus: a block producer may still publish one, so the app shows every asset
  number first (`#5555-1234 · "GOLD (#1739-0029)"`). Text shaped as a number that is none
  (`0000-1739-0029`) is an error in every command, never the asset of that name.
- **The front-running window that remains**: a registration not mined within the reveal depth can
  be seen in the mempool, and a block producer who reserved the same name meanwhile (after seeing
  it) could register it in its own block once its own reservation is deep enough. This is inherent
  to commit-reveal; registering as soon as the depth allows (the app does) keeps the window short.
- **An asset is known by the SHA-256 hash of its name.** The registration publishes the name
  unless asked not to; an asset can be named by its name, `0x` and its hash, or its number
  (`1739-0029` for the first), and CHN by `CHN`, in the node's commands and the wallet's alike.
- **Decimals** (0 to 12) are set at registration and only change how amounts are shown: with 2,
  1.5 is 150 units.
- **The control coin** mints more (`mintasset`) and changes the asset's data (`updateasset`): a
  description, a commitment to documents kept elsewhere, addresses and keys of the issuer. It is
  moved like any coin (`transferassetcontrol`). Burning it (`fixassetsupply`) fixes the supply for
  good.
- **Anyone can burn** coins they hold (`burnasset`). Each asset keeps what was minted and burned;
  the supply is the difference.

## How it is carried

Tokens — asset coins, control coins, reservations, pool shares and auction receipts — are carried
by outputs of no value. A *marker*, an OP_RETURN output of the transaction (`BAST`), lists each such
output with what it carries, and says what the transaction does, if anything. The rules keep the
books: for every token, what the transaction spends plus what its operation creates is exactly
what it outputs plus what its operation takes in. Nothing is burned by accident; a transaction that
spends a token without carrying it on, or outputs one it does not have, is invalid.

## Pools

- A pool trades two assets, CHN included: a constant-product market maker (Uniswap v2). Each trade
  leaves 0.3% in the pool, for the liquidity providers.
- `addliquidity` puts both assets in, for shares of the pool (tokens like any other). The first
  deposit makes the pool and sets its price; it keeps 1000 shares for good, so that a pool is never
  emptied. Later deposits go in at the pool's price.
- **The amounts of a deposit are the most that goes in** (the fourth audit's rules). Into a pool someone provides liquidity to, the side that gives fewer
  shares goes in whole and, of the other, only what those shares are worth at the pool's price
  (rounded up: the pool never loses); the rest comes back. The marker lists two result outputs: the
  shares, then what comes back — asset coins, or CHN paid by the coinbase to that output's address
  (less than the least CHN paid out stays in the pool); it carries nothing if nothing comes back.
  Nothing a deposit offers is given away to the pool's providers. (Before these rules, both amounts
  went in whole, for the shares of the side that gave fewer: the wallet offers only what the pool's
  price takes.)
- A pool everyone has left (*abandoned*: only the 1000 shares nobody holds) takes no trades, and is
  reopened as a new pool is made: shares are the square root of the product of what it holds after
  the deposit, 1000 of them kept for good, and both amounts go in. What it held — dust, at whatever
  price the last trade or provider left it — is merged in: the wallet takes the amounts given as what
  the pool reopens with, and puts in the rest, so that they set its price exactly (it refuses
  amounts not above the dust). Before these rules, a deposit into an abandoned pool went in at the
  dust's price, for the shares of the side that gave fewer: someone who reopened it first at the
  dust's price, just before a reopening, took the reopener's excess.
- **A reopening cannot be front-run.** Whoever's deposit opens the pool first sets its price;
  another deposit made for another price then goes in at that price, its excess back, and is refused
  if it gives fewer shares than the slippage allows (the shares of a deposit fall with the square
  root of how far the price is from its own). Making a reopening fail takes reopening the pool
  oneself, with at least 0.01 CHN on a CHN side and 100000 shares, at a price anyone can trade
  against, or arbitrage by a swap before adding; leaving again leaves the 1000 shares' part behind,
  as dust for the next reopener.
- `swapasset` pays one asset in and takes at least an amount of the other out (the slippage the
  trade accepts); `quoteswap` says what a trade gives now. `removeliquidity` gives shares back for
  both assets; a side that rounds to nothing is left out, so that a small provider can always leave.

## Dutch auctions

- `createauction` sells an amount of one asset for another. The price of all of it starts at the
  start price and falls in a straight line, block by block, to the end price; a bid buys at the
  price of its block (`bidauction`), rounded in the seller's favour.
- The transaction outputs a **receipt**. Its holder collects (`collectauction`) what the auction
  brought in and what is left of what it sold, once it has ended or sold out — or at once, before
  any bid, which cancels it.
- `getauction` and `quotebid` give what all that is left costs in the next block
  (`cost_of_remaining`: the least amount whose bid buys it all). `bidauction` with `buy_all` pays
  exactly that, never more (with an amount, at most that amount), and refuses when no bid buys
  exactly what is left (the least that buys it all buys more, which the rules refuse); a bid of an
  amount that buys all that is left pays only what that costs. The app's "Buy all that is left" pays the
  amount it shows (less, should the price fall first).
- An auction starts within 52560 blocks (about a year) of the one that makes it.
- A closed (collected) auction stays in the state for good, as its history; `listauctions` leaves
  closed ones out unless asked (`include_closed`).

## Retiring an asset

`releaseasset` retires a dead asset: its supply fixed, nobody holding any, every unit in pools
nobody provides liquidity to, and no auction of it to collect. An auction that only sells something
for it, and has taken none of it in, does not count; once the asset is retired (and even if its name
is registered again, as another asset), that auction takes no more bids, and its seller collects
what it sells.

## Logos

An asset can have a logo: a PNG or a JPEG of exactly 256 x 256 pixels, at most 32768 bytes, kept on
the chain. Whoever holds the asset's control coin sets it — at the registration (`registerasset`
with `logo`) or later (`setassetlogo`, which also removes it with an empty logo) — and a new one
replaces the one before. A fixed asset (control coin burned) keeps the logo it has.

**Carrier.** The logo goes whole into the marker of the transaction that sets it (operation
`SetLogo`, kind 32; a registration with a logo is kind 33: the registration, then the logo). One
transaction, signed like any other, spending and outputting the control coin as `updateasset` does.
A 32 KB OP_RETURN is standard here (`-datacarriersize` defaults to 98 000 bytes, a transaction to
392 000 weight units) and a full logo is about 131 000 weight units, 3% of a block's 4 000 000 for
transactions. Witness data (an inscription-style envelope in a tapscript) would cost a quarter of
that, but takes two transactions (a commitment, then a reveal spending it) that a reorganization or
a replacement can separate (the commitment's coins stranded), and a signer the wallet does not have
(the envelope is no miniscript); at 1 sat/vB a full logo costs about 0.00033 CHN in a marker against
0.00009 in a witness, and logos are set rarely. The marker was chosen: atomic, one confirmation of
exactly the bytes shown, nothing new to sign.

**Rules** (`bitassets/logo.h`; consensus checks the structure of the file, it decodes no image):

- At most 32768 bytes, not empty. PNG (signature `89 50 4E 47 0D 0A 1A 0A`) or JPEG (`FF D8`);
  anything else (SVG, GIF, WebP, BMP, HTML...) is refused (`bad-ba-logo-format`).
- PNG: every chunk's length within the file and its CRC-32 right; the chunks IHDR, PLTE, tRNS,
  IDAT, IEND only (no text, time, gamma, colour profile, Exif, animation or private chunk). IHDR
  first, 13 bytes: 256 x 256, a bit depth and colour type the PNG standard pairs (grey 1/2/4/8/16,
  RGB 8/16, palette 1/2/4/8, grey+alpha and RGBA 8/16), compression and filter 0, **not
  interlaced**. PLTE once, before the image data, for a palette image only (and required by one),
  at most 2^depth entries. tRNS once, before the image data, after PLTE, of its colour type's size
  (no more entries than the palette), never with an alpha channel. IDAT chunks one after another,
  more than 0 bytes in all, starting with a zlib header (deflate, window up to 32 KiB, no preset
  dictionary, its check right). IEND empty and last: nothing after it.
- JPEG: SOI, then segments of SOF0 (baseline) or SOF2 (progressive), DQT, DHT, DRI and SOS only,
  each of the length its content gives; no APPn (JFIF, Exif, XMP, ICC, Adobe), no COM, no other
  frame type (extended, lossless, hierarchical, arithmetic), DNL, DHP, EXP, fill bytes or stray
  markers. One SOF before the first scan: 8-bit, 256 x 256, 1 or 3 components (different ids,
  sampling factors 1–4, quantization tables 0–3). DQT: 8-bit tables of 64 values filling the
  segment; DHT: tables filling the segment, of at most 256 values, each a prefix code; DRI 4 bytes.
  SOS: 1 to 4 of the frame's components, each once; baseline 0–63 with no approximation, progressive
  a DC scan (0–0) or an AC scan of one component (s–e, 1 ≤ s ≤ e ≤ 63). Its entropy-coded data
  holds FF only before 00 or a restart marker (FFD0–FFD7). At least one scan; EOI last, nothing after.
- The reasons: `bad-ba-logo-empty`, `-oversize`, `-format`, `-size` (not 256 x 256), `-trailing`,
  `-png-*` and `-jpeg-*`; `bad-ba-logo-none` (removing a logo there is not), `bad-ba-logo-asset`,
  and `bad-ba-no-control` / `bad-ba-asset-unknown` as for `updateasset`.

**Activation.** From `SidechainParams::logo_height`: before it a marker that sets a logo is not
valid (`bad-ba-logo-inactive`), as no such marker was; nodes have to be upgraded before the chain
reaches it (an older node refuses a block with one). Main network and signet: 0 (not launched).
Test network: 0 too (restarted from a new genesis on 19/Oct/2026). Regtest: 0, or
`-testactivationheight=logo@<height>`. `getbitassetsinfo` says `logo_height` (-1: never).

**State and storage.** The state records each asset's logo (table 0x90: SHA-256 of the bytes,
format, size, height, transaction) and keeps its bytes apart (0x91), only the current logo of each
asset; no rule reads them. They are kept with the sidechain state, not read from the blocks, so a
pruned node serves them too; the journal of each block puts the logo before back when a
reorganization disconnects it, and a state derived again from the blocks has them again. Retiring
an asset drops its logo (the name registered again starts without one).

**Commands.** `getasset` and `listassets` say `logo` {`hash`, `mime`, `size`, `height`, `txid`}
(never the bytes); `getassetlogo <asset>` gives them with `base64` (at most 32768 bytes).
`setassetlogo <asset> <base64> ({"fee_rate": sat/vB})` (an empty logo removes it) and
`registerasset ... <logo>` check the logo against the rules before they pay for anything.

**Showing a logo.** The bytes are the chain's: show them only as an image of the MIME type the node
gives, never as a document or markup — the app makes a `data:image/png;base64,...` (or jpeg) URL
for an `<img>`, and its own pages never put a logo in HTML. A file the rules take is one image and
nothing else, but its pixels are anyone's: data hidden in the image data itself (steganography)
cannot be told apart, and a deflate stream may be unusual (a decoder stops at 256 x 256: at most
256 x (1 + 256 x 8) bytes, about 0.5 MB, whatever the stream claims). The app (`bitassets-app`)
crops and scales any picture to 256 x 256, encodes it as PNG (a palette when that is smaller) and
as JPEG (quality about 80), keeps the smaller, and strips every metadata segment.

## Tickers and display names

From `SidechainParams::ticker_height`, an asset has two names:

- Its **ticker** is its name, as before: unique, registered first come first served by a reservation
  then a registration (commit, then reveal), the hash of it the asset's id. From the height, the name
  of every **new** asset is a ticker: **2 to 12 characters, each an uppercase letter A–Z or a digit
  0–9** (`bitassets::IsTicker`), and **published** (a registration without its text is refused:
  `bad-ba-ticker-private`; one whose text is no ticker: `bad-ba-ticker`). The look-alike rules hold
  tickers too (no `CHN`, no `0X...`; `#`, `(`, `)` and `dddd-dddd` cannot be in one). The wallet
  uppercases what it is given (`reserveasset gold` reserves `GOLD`) and refuses anything else before
  paying for a reservation. **Assets registered before keep their names**, whatever they are
  ("gold bar", "my gold coin"), and are shown as they are; everything else about them works as before
  (mint, transfer, data, logo, display name). A reservation made before the height of a name that is
  no ticker can no longer be registered (release it: `releaseassetreservation`).
- Its **display name** is optional and **not unique**: any number of assets may have the same one.
  The app shows the ticker first and the display name after it, muted ("GOLD · Gold Bar Token, 1 g ·
  #1739-0029"), never the display name instead of the ticker.

**What a display name may be** (`bitassets::IsDisplayName`, consensus; `WhyNotDisplayName` says why
not): 1 to 64 bytes of well-formed UTF-8 (shortest forms, no surrogates, nothing past U+10FFFF), no
space at either end, and each code point one of `DISPLAY_NAME_RANGES` (`src/bitassets/displayname_table.h`,
written by `contrib/devtools/gen-display-name-table.py` from Unicode 15.0): assigned and graphic
(letters, digits, punctuation, symbols, emoji of one code point) or the space U+0020. Refused, so that
nothing changes how the rest of a name reads (spoofing): controls (C0, DEL, C1); format characters,
among them the bidirectional overrides, embeddings, isolates and marks (U+202A–202E, U+2066–2069,
U+200E/200F, U+061C), the zero-width space, non-joiner and joiner (U+200B–200D), the word joiner,
the byte order mark, the soft hyphen and the tags; every space but U+0020 (no-break, ideographic,
line and paragraph separators); default-ignorable and blank characters (Hangul fillers, braille
blank); combining marks and variation selectors; conjoining Hangul jamo and every character that
composes with the one before it; every character that NFC changes (the angstrom and ohm signs, CJK
compatibility ideographs...); unassigned code points, private use and noncharacters. A text of these
characters only is in NFC, so no normalization is needed to compare two: the app normalizes what is
typed to NFC before it sends it. The set is frozen at Unicode 15.0 (a later version adds nothing; a
soft fork could). Scripts that need combining marks beyond their precomposed letters cannot be
written in full. Display names are not unique: they are not a defence against an asset that takes
another's display name — the ticker is.

**Operations.** A registration may carry one (marker kind 35: the registration, its logo or an empty
one, then the display name); `SetDisplayName` (kind 34: asset, text; an empty text removes it), by
whoever spends the asset's control coin, as `updateasset` and `setassetlogo` are (a fixed asset cannot
change it). Reasons: `bad-ba-display-name` (not one), `bad-ba-display-name-asset` (CHN),
`bad-ba-display-name-none` (removing one there is not), `bad-ba-no-control`, `bad-ba-asset-unknown`.

**Activation.** Before `SidechainParams::ticker_height`, a marker that sets a display name is not
valid (`bad-ba-display-name-inactive`), and names are as before. Main network and signet: 0 (not
launched). Test network: 0 too (restarted from a new genesis on 19/Oct/2026). Regtest: never, or `-testactivationheight=ticker@<height>` (the
tests of before register names that are not tickers). `getbitassetsinfo` says `ticker_height` (-1:
never), `tickers_active` (the next block), `min_ticker_length`, `max_ticker_length` and
`max_display_name_bytes`. Like `logo_height`, the height counts in the fingerprint of the parameters
of the sidechain database once it is set: the node derives its sidechain state again from the blocks
once when it first starts with a height set (as it did for the logos), not while it is never.

**State and index.** The state keeps each asset's display name in a table of its own (0x92:
`DisplayNameRecord` text, height, transaction), so the layout of the others (`STORE_LAYOUT` 5) is
unchanged; the journal of each block puts the one before back on a reorganization; retiring an asset
drops it. The search index (layout **2**) has it in each row (`display`, written last: a row of layout
1 reads as a row without one), a flag `HAS_DISPLAY`, and two orders: `m` (the normalised display name)
and `u` (each later word of it). A search for text matches the names first (whole, start, a later
word), then the display names (their start, then a later word); `match` says `display_name` or
`display_word`; `exclude_keywords` looks at both. **Upgrading** from index layout 1: nothing is
derived again — a store written by a version without display names holds none, so its index is that
of layout 2 already, and the node writes the new layout at startup (`index::UpgradeLayout`, logged
"brought to layout 2 in place"). An index of any other layout (or a layout-1 store that somehow holds
display names) is derived again from the blocks, as before.

**Commands.** `getasset`, `listassets`, `searchassets` and `searchmyassets` say `display_name` when
there is one. `registerasset name supply (decimals data public logo display_name)` (from the height:
the name uppercased and checked as a ticker, `public` true; the result says the `name` registered);
`setassetdisplayname <asset> <text> ({"fee_rate": sat/vB})` (`""` removes it);
`listassettransactions` category `display_name`.

## CHN in pools and auctions

CHN that goes into a pool or an auction leaves circulation: the marker burns it (its value is the
CHN the operation takes in, exactly). CHN that comes out is paid by the coinbase of the block, as
deposits are, to the address the transaction names (for a deposit into a pool, the address of its
second result output). Asset coins that come out go to the *result*
outputs the marker lists, in the amount the state gives when the block is connected — never less
than the transaction asks for.

The least CHN paid out is 50000 satoshis, 0.0005 CHN (each payout takes a place in the coinbase's
queue, which pays a bounded number per block: 500 of the sidechain's own; filling it with payouts of
1000 satoshis, as under the second audit's rules, took next to nothing): a swap or a bid that would
pay less is refused; less taken out of a pool, or coming back from a deposit, stays in it, for its
providers; less from collecting an auction is not paid (it was burned when it went in).

## Rules from genesis

The rules of the audits apply from block 0 on every network: there are no activation heights for
them, and the rules before them are gone. (The logos of assets, added after, have one: Logos.)

- A reservation registers only bound to its output's script; one of the name alone registers
  nothing (it can only be released, and the name reserved again).
- Whether an asset can be retired is read from counts its record keeps (auctions that hold some of
  it, its pools with providers), not by going through every auction that quotes it.
- Retiring an asset whose pools hold 1 satoshi of CHN gives it to mainchain miners too (a
  withdrawal of 1 satoshi, without fee).

A node stores the assets in a layout of its version (`bitassets::STORE_LAYOUT`, 4 since the audits'
rules apply from genesis, and the search index in its own, `bitassets::index::INDEX_LAYOUT`); one that finds them in an earlier layout derives the sidechain state again
from its blocks at startup (a pruned node cannot, and has to be resynced).

## The search index

Wallet pages search, filter, sort and page through the assets, pools and auctions on the node, a page
at a time: no page reads the assets whole, however many there are (millions, in time).

**What it is.** Derived data, not state: no rule reads it. It is kept in the store of the sidechain
state, under a key byte of its own (`sidechain::DERIVED_TABLE`, 0xA0), and written at the end of each
block (`bitassets::index::ConnectBlock`, after the block's own rules) from what the block changed: the
journal of the block's overlay lists every key of the assets, tokens, pools, auctions and logos the
block touched, with its earlier value. So it is flushed with the state and the coins, undone with the
block by a reorganisation (its keys are in the block's undo data like any other), always at the tip,
and identical on every node: there is nothing to enable, sync or rebuild apart, and no index can lag
or drift from the chain. The hash of the state (`getsidechainstate`) leaves it out. It is always on:
it costs a few store writes per asset a block touches, and the disk below, small next to the state it
indexes; an option would only make some nodes unable to answer the wallets. Upgrading to the version
that has it (index layout 1) derives the sidechain state once from the blocks at startup (a pruned node
that no longer has them all has to be resynced); the state itself, and its hash, are unchanged.

Per asset ever registered, a row (number, id, name, decimals, supply, minted, burned, flags: fixed,
retired, has a logo, has a pool; registration height, last activity height, CHN depth of its pool with
CHN, volume over the window, pools) and an entry in each order, each entry carrying the summary the
filters read (flags, decimals, heights, supply, depth), so that filtering never reads a row:

| Order | Key | Holds |
|---|---|---|
| name | normalised name, then number | every asset (private ones after the named) |
| words | each word of the name that starts after its start | named assets |
| newest | inverted number | every asset |
| supply | inverted supply | every asset |
| activity | inverted last activity height | every asset |
| volume | inverted volume | assets traded within the window |
| pool depth | inverted CHN depth | assets with a pool with CHN someone provides for |

*Activity* is any block that touches the asset: a transfer of its coins or control coin, a mint, a
burn, a change of its data or logo, a trade or deposit in one of its pools, its auctions, its
retirement. *Volume* is the CHN traded over the last `SidechainParams::asset_volume_window` blocks
(1440 on BitAssets, about a day of one-minute blocks; 5760 on Layers, a day of 15-second slots;
10 on regtest): what went into a pool with CHN, the CHN side as is and the asset side at the pool's
price after the block, and bids paid in CHN (or CHN sold) by its auctions. Each block's volumes are
kept apart until they leave the window, when they are taken off. Pools have rows of their own
(reserves, depth, volume, the reserves at the start of the window for the price change) with orders
by depth, volume, price change and age; auctions not closed are ordered by their last block, all of
them by age. Retired assets keep their rows, flagged (their names may belong to another asset now).

**Disk and memory.** 247 bytes per asset on disk in LevelDB (measured over a million synthetic assets
with two- and three-word names: about 250 MB per million assets), plus a few dozen bytes per pool and
auction and per asset and pool traded in each block of the window. Nothing is held in memory but the
page being read: a search reads the database through a snapshot, without the chain lock.

**How a search reads it.** A search walks one order from its cursor, with the filters, until the page
is full: a page costs about the entries it skips, not the assets there are. A text matches the whole
name, its start, or the start of any of its words (any case): by relevance it reads the names that
start with the text, then the words that do; in another order it collects the matches (at most 5000)
and sorts them, or, if there are more, walks that order with the text as a filter. The wallet's
assets (`searchmyassets`), a list of assets, a number or a hash are a set sorted the same way. Each call looks at 50 000 entries
and reads 10 000 rows at most: a page cut short by that says `partial`, with a cursor to go on.
Cursors are the key of the last entry returned: stable while the chain moves (an asset that moves in
the order meanwhile may show twice or not at all, as in any paged list of live data).

Measured with a million assets in LevelDB on the development PC (`BITASSETS_INDEX_BENCH=1 test_bitcoin
--run_test=bitassets_index_tests/million`), pages of 50, averaged over 10 pages:

| Search | ms per page |
|---|---|
| every order (activity, newest, name, supply, volume, pool depth) | 0.1 to 0.3 |
| a text by relevance ("gold", "coin") | 0.1 |
| a text matched by nothing ("moon 12345": every name with "moon" as a later word read) | 19 |
| a text by supply, more matches than 5000 | 2 |
| a text by supply, fewer (sorted in memory) | 22 |
| supply, has_logo, exclude_burned | 0.7 |
| name, has_pool, min_supply (few pass) | 3 |
| a filter nothing passes (the whole budget of 50 000 entries) | 12 |

### Commands

`searchassets (query filters sort cursor limit)`: `query` is text, `"#"` and a number, a number
(`1739-0029`), or a hash (`0x` optional); `filters` is an object of `min_supply` (whole units),
`has_pool`, `has_logo`, `active_within_blocks` / `exclude_dead` (blocks: only those touched within),
`created_after` (height), `exclude_burned` (supply 0), `exclude_retired` (default true),
`exclude_keywords` (whole words of the names, any case), `assets` (only these: names, numbers or hashes, at
most 5000); `exclude_dead: true` means not touched within the volume window (about a day); `sort` is `relevance` (default; no
text: as activity), `name`, `newest`, `oldest`, `supply`, `activity`, `volume`, `pool_depth`; `limit`
1 to 200 (default 50). It returns `assets` (number, id, name, supply, minted, burned, flags, pools,
`pool_depth` and `volume` in CHN, `created`, `last_activity`, and how a text matched: `exact`,
`prefix` or `word`, and, unless retired, the other fields of `getasset`: registration, data, releasable,
control, logo), `next_cursor` (the cursor of the next page, null at the end), `partial`, `scanned`, `height`.
Arguments are positional in this order (`null` for no cursor) or named.

`searchmyassets (query filters sort cursor limit)` (wallet): the same among the assets the wallet holds or
controls, in blocks (its own token outputs, bounded by the wallet's transactions), the wallet chosen as for any
wallet command (`-rpcwallet`, `/wallet/<name>`). The node's `searchassets` never reads a wallet: `mine` there is
refused with a pointer to `searchmyassets`.

```
bitassets-cli searchassets gold
bitassets-cli -named searchassets query=gold sort=volume limit=20 filters='{"has_pool":true}'
bitassets-cli searchassets "" '{"exclude_dead":1440,"exclude_burned":true,"exclude_keywords":["test"]}' newest
bitassets-cli -rpcwallet=w searchmyassets "" '{}' name
bitassets-cli searchassets "" '{}' newest <next_cursor of the page before>
bitassets-cli searchassets "#1739-0029"
```

`searchpools (query filters sort cursor limit)`: pools whose assets match the text (of the first 1000
assets it matches, found within one search budget: a text that few names match among millions stops there and
says `partial`); filters `exclude_keywords` (either asset's name), `exclude_abandoned` (default true), `chn_only`, `min_depth` (CHN); sorts
`depth` (default; pools with CHN), `volume` (traded within the window), `price_change` (of the second
asset in the first, CHN for a pool with CHN, over the window, largest rise first), `newest`. Each pool
as `listpools` shows it, with `depth`, `volume_window`, `price_change` (percent), `last_activity`. The sorts and
filters of `searchassets` are taken too (an app sends the same object): `relevance`, `pool_depth`, `name`,
`supply`, `oldest` sort by depth, `activity` by volume; the filters of assets are left aside.

`searchauctions (query filters sort cursor limit)`: auctions not closed that sell or sell for an asset
the text matches; filter `status` (`open`, `upcoming`, `active` = open or upcoming (default), `ended`
= not collected yet, `any`), `include_closed` (sort newest, no text) and `exclude_keywords`; sorts
`ending_soon` (default; any sort of `searchassets` but `newest`) and `newest`. All three return `next_cursor`
and `partial`. Each auction as `listauctions` shows it.

`getassetindexinfo`: `synced` (always: it is at the tip), `height`, `bestblock`, `count` (assets
indexed, retired ones included), `retired`, `pools`, `volume_window`, `max_page`.

## Commands

Node: `getasset`, `listassets`, `getassethistory`, `getassetlogo`, `getbitassetsinfo`, `listpools`, `getpool`,
`quoteswap`, `listauctions`, `getauction`, `quotebid`, and the search (above): `searchassets`, `searchpools`,
`searchauctions`, `getassetindexinfo`.
`listassets`, `listpools` and `listauctions` return a page at a time (`count`, at most 1000 for
pools and auctions, and `skip`; `listauctions` also by asset). The pools and auctions are ordered,
and `getbitassetsinfo` counted, once per block from a snapshot of the state read without the chain
lock, so polling them costs a page, not the whole table.

`swapasset` takes the least the caller showed (`min_out`): if the pool moved since its quote, so
that what the wallet would sign as the least is lower, it refuses instead of signing for less.
`addliquidity` takes what the caller expects (`sets_price`): a deposit that was to make a pool is
refused if someone made it meanwhile, rather than going in at their price.
Wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `listmyassets`, `sendasset`,
`mintasset`, `burnasset`, `updateasset`, `setassetlogo`, `transferassetcontrol`, `fixassetsupply`, `swapasset`,
`addliquidity`, `removeliquidity`, `createauction`, `bidauction`, `collectauction`,
`listassetactivity`, `listassettransactions`.

`listassettransactions (count skip asset)` lists the asset moves of the wallet, newest first, one entry per asset a
transaction moves: its category (send, receive, register, mint, burn, swap, pool_add, pool_remove, auction, bid,
collect, logo, update, control, reserve, release), the asset with its label and number, the amount (exact) and its sign,
the network fee when the wallet paid it, and the other side: for a send the addresses it went to, for a receive the
addresses that paid the transaction, read from the undo data of its block (or the mempool) — no index; "unknown" when
the block is pruned. A result of a swap or a bid spent since is shown as the least the transaction asked (`at_least`).

## Differences from the original

- Bitcoin's keys, addresses and transactions instead of ed25519 and BLAKE3; SHA-256 for asset ids,
  HMAC-SHA256 for reservations.
- Every output names the token it carries, and the books must balance exactly, instead of the
  original's inferred assets, where an operation could claim more than it spent and leftovers were
  burned without a word.
- Only the control coin of an asset mints it or changes its data (the original let the control
  coin of any asset change any asset's data). Assets without initial supply work, and minting needs
  no coins of the asset.
- CHN in and out of pools and auctions is accounted for (the original counted CHN put in as a fee
  the block's miner could take, and could never pay any out).
- No division by zero in a swap, no stop in an auction's last block, auction quantities rounded
  down (the original rounded up, for the bidder), single 0.3% fee (the original took it twice),
  minimum liquidity, receipts required, checked arithmetic throughout.
- Transactions are checked against the state when they enter the mempool, and stale ones leave it
  after each block; two registrations of a name in one block are refused. After a block, only the
  transactions whose check read what the block changed (the keys of the state noted when each was
  last checked), bids and auctions made to start at a height, and those not checked yet are checked
  again: the work is that of what blocks change, not that of the mempool.
- Added: burning, fixing the supply, giving control, auction cancellation, prices and quotes,
  the wallet's activity.

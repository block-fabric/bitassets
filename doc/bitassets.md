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
  (zero is allowed) and its **control coin**. The window ("Assets", in the sidebar) does both steps.
  Bound to its output's script, a reservation cannot be copied to any use: a copy made by someone
  else, with an output of their own, commits to nothing they can reveal.
- **A name may not read as another asset**: not CHN (in any case), not like a number
  (`1739-0029`, `0001-1739-0029`), not starting with `0x`.
- **An asset is known by the SHA-256 hash of its name.** The registration publishes the name
  unless asked not to; an asset can be named by its name, `0x` and its hash, or its number
  (`1739-0029` for the first), and CHN by `CHN`.
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
- An auction starts within 52560 blocks (about a year) of the one that makes it.

## Retiring an asset

`releaseasset` retires a dead asset: its supply fixed, nobody holding any, every unit in pools
nobody provides liquidity to, and no auction of it to collect. An auction that only sells something
for it, and has taken none of it in, does not count; once the asset is retired (and even if its name
is registered again, as another asset), that auction takes no more bids, and its seller collects
what it sells.

## CHN in pools and auctions

CHN that goes into a pool or an auction leaves circulation: the marker burns it (its value is the
CHN the operation takes in, exactly). CHN that comes out is paid by the coinbase of the block, as
deposits are, to the address the transaction names. Asset coins that come out go to the *result*
outputs the marker lists, in the amount the state gives when the block is connected — never less
than the transaction asks for.

The least CHN paid out is 1000 satoshis (each payout takes a place in the coinbase's queue, which
pays a bounded number per block): a swap or a bid that would pay less is refused; less taken out of a
pool stays in it, for its providers; less from collecting an auction is not paid (it was burned when
it went in).

## Rules by height

The rules of the second audit (October 2026) apply from `bitassets_audit2_height`: from the start
on mainnet, signet and regtest; on the test network from the height set when it is deployed.
Reservations made before it, committing to the name alone, still register after it, under the rule
of the first audit (only the oldest reservation of a commitment reveals it).

A node stores the assets in a layout of its version (`bitassets::STORE_LAYOUT`); one that finds them
in an earlier layout derives the sidechain state again from its blocks at startup (a pruned node
cannot, and has to be resynced).

## Commands

Node: `getasset`, `listassets`, `getassethistory`, `getbitassetsinfo`, `listpools`, `getpool`,
`quoteswap`, `listauctions`, `getauction`, `quotebid`.
Wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `listmyassets`, `sendasset`,
`mintasset`, `burnasset`, `updateasset`, `transferassetcontrol`, `fixassetsupply`, `swapasset`,
`addliquidity`, `removeliquidity`, `createauction`, `bidauction`, `collectauction`,
`listassetactivity`.

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
  after each block; two registrations of a name in one block are refused.
- Added: burning, fixing the supply, giving control, auction cancellation, prices and quotes,
  the wallet's activity.

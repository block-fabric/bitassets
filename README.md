# BitAssets

BitAssets is a sidechain of Chains on which anyone can create assets (shares, points, tickets,
stablecoins) and trade them: in pools, an automated market maker, and by Dutch auction.

| | |
|---|---|
| Slot on the mainchain | 4 |
| Coin | CHN, deposited from [Chains](https://github.com/block-fabric/chains) and withdrawn back to it |
| Addresses | `ba1…` (`tba1…` on the test networks) |
| P2P port | 9355 (testnet 19355, signet 39355, regtest 29355) |
| RPC port | 9354 (testnet 19354, signet 39354, regtest 29354) |

BitAssets is the [sidechain template](doc/sidechain.md) of Chains with assets added. A BitAssets node needs a
Chains node (`chainsd` or `chains-qt`) to follow, reached over its RPC interface.

## Installing

There is no packaged build yet: build it from source, below.

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev
git clone https://github.com/block-fabric/bitassets.git
cd bitassets
cmake -B build -DBUILD_GUI=ON
cmake --build build -j$(nproc)
```

Leave out `-DBUILD_GUI=ON` (and the Qt packages) for a node without a window. Other systems and
options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `bitassets-qt` | the wallet, with its window |
| `bitassetsd` | the node, without a window |
| `bitassets-cli` | commands for a running node or wallet |
| `bitassets-wallet`, `bitassets-tx`, `bitassets-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The
sidechain tests need a built Chains tree next to this one (`../chains`), or `MAINCHAIN_BIN_DIR`
set to the folder of its programs.

## Running it

First a Chains node, with RPC on (`server=1` in `chains.conf`, or `-server`), on the same network.

The simplest is to let the Chains wallet run BitAssets: in its **Sidechain Nodes** window, select
BitAssets, press **Locate programs…** and choose the folder with `bitassetsd` and `bitassets-qt`, then **Start node**
or **Open wallet**.

By hand, on the same computer as the Chains node:

```sh
bitassets-qt                 # the main network
bitassets-qt -testnet        # the test network
```

It finds the Chains node by itself: RPC on 127.0.0.1 at the port of the network, and the cookie
file in the Chains data folder (`~/.chains/.cookie`, `~/.chains/testnet/.cookie`). Options for
anything else:

| Option | What it is |
|---|---|
| `-mainchainrpcconnect=<ip>` | address of the Chains node (default 127.0.0.1) |
| `-mainchainrpcport=<port>` | its RPC port (default 9554, testnet 19554, signet 39554, regtest 29554) |
| `-mainchainrpccookiefile=<file>` | its cookie file |
| `-mainchaindatadir=<dir>` | its data folder, where the cookie file is looked for (default `~/.chains`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Chains wallet that pays for merged mining (default: its only loaded wallet) |

The slot is fixed on the main and test networks; `-sidechainslot=<n>` is for regtest only, where
it makes the chain a sidechain in slot `n` (see [doc/sidechain.md](doc/sidechain.md)).

Data and the configuration file `bitassets.conf` are in `~/.bitassets`; the test network has a subfolder
`testnet`. The networks have no DNS seeds: give the node a peer with `addnode`. A sample
`~/.bitassets/bitassets.conf`:

```ini
server=1
fallbackfee=0.0002

# For the test network instead, uncomment:
# testnet=1

[main]
addnode=<peer>:9355
mainchainrpcwallet=<wallet of the Chains node>

[test]
addnode=<peer>:19355
mainchainrpcwallet=<wallet of the Chains node>
```

On the test network, the `-mainchainrpcport`, `-mainchainrpccookiefile` and `-mainchainrpcwallet`
options are read only from the `[test]` section.

## Coins in and out

- **Deposit.** In BitAssets, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s4_<address>_<checksum>`. In Chains, send to it from the **Sidechains** page, or
  `chains-cli createsidechaindeposit 4 <deposit address> <amount>`. The coins appear with the
  next BitAssets block.
- **Withdraw.** `createwithdrawal <Chains address> <amount> ( <fee for Chains miners> )`, or the
  **Mainchain** page. Withdrawals are paid in bundles that Chains miners vote on (about three
  months on the main network, 600 blocks on the test network). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** BitAssets blocks are mined by Chains miners, for a fee in CHN paid by the Chains
  wallet. `setbmm true <BitAssets address>` has the node ask for a block whenever the fees waiting pay
  for one; it offers the miners 99% and keeps 1% at the address. `requestbmmblock <address>
  <amount>` asks for one block now, for a fee of your choice (for a deposit on a quiet chain).
  `getbmminfo` shows how it goes.

How deposits, withdrawals, bundles and merged mining work: [doc/sidechain.md](doc/sidechain.md).

## Assets

- **Creating an asset** takes two steps, a hidden reservation then the registration, so that nobody
  can see the name coming and take it first. The asset comes with a **control coin**: its holder can
  mint more, until the supply is fixed for good.
- Assets are sent like coins, and can be burned.
- **Pools** pair an asset with CHN or another asset (constant product, 0.3% fee). Anyone can add
  liquidity and swap.
- **Dutch auctions** sell an amount of an asset at a price that falls from a start to an end price.
- An asset nobody holds any more can be **retired**: what is left in its pools goes to the miners.

How assets work: [doc/bitassets.md](doc/bitassets.md).

## Commands

BitAssets, on the node: `getbitassetsinfo`, `listassets`, `getasset`, `getassethistory`, `listpools`,
`getpool`, `quoteswap`, `listauctions`, `getauction`, `quotebid`.

BitAssets, in the wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `mintasset`,
`fixassetsupply`, `updateasset`, `transferassetcontrol`, `sendasset`, `burnasset`, `releaseasset`,
`listmyassets`, `listassetactivity`, `addliquidity`, `removeliquidity`, `swapasset`, `createauction`,
`bidauction`, `collectauction`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`.

Sidechain, in the wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

## Wallet

The pages: Overview, Send, Receive, Transactions, **Mainchain** (deposits, withdrawals, merged
mining), and **Assets** (your assets, send and receive, create, trade in pools, auctions, explore).

## Credits

BitAssets is inspired by **BitAssets by LayerTwo Labs** (plain-bitassets). It is built on the sidechain template of Chains, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).

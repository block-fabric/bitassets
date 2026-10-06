# Chains

Chains (CHN) is a proof-of-work coin with drivechains: sidechains that hold CHN, created by
deposits from the mainchain and paid back to it by withdrawals that the miners vote on (BIP300),
and whose blocks the miners of Chains mine blindly, for a fee (BIP301). The node enforces these
rules itself; no separate enforcer is needed. It is Bitcoin Core v32 with drivechains, and its
wallet runs sidechain nodes.

- SHA-256d, one-minute blocks, aserti3 difficulty (two-hour half-life).
- 50 CHN per block, halving every 210 000 blocks, no premine. Coinbase maturity 360 blocks.
- Blocks of 6 MB, of which 4 MB for transactions; the rest is kept for the coinbase and the
  drivechain messages.
- Addresses `chn1…` (`tchn1…` on the test networks).

The sidechains, each in a repository of its own:

| Sidechain | Slot | What it is |
|---|---|---|
| [Thunder](https://github.com/<org>/thunder) | 2 | Large blocks, for volume |
| [BitNames](https://github.com/<org>/bitnames) | 3 | Names, with `.x` domains |
| [BitAssets](https://github.com/<org>/bitassets) | 4 | Assets, pools and auctions |
| [zSide](https://github.com/<org>/zside) | 6 | Private payments (Zcash Orchard) |
| [Hivemind](https://github.com/<org>/hivemind) | 9 | Prediction markets |

## Installing

Builds for Linux (x86-64) are at https://blockfab.org/drivechains/. Unpack, check
`sha256sum -c SHA256SUMS`, and run what is in `bin/`. The wallet needs the Qt libraries of the
system (on Ubuntu 24.04: `sudo apt install libqt6widgets6 libqt6network6 libqt6dbus6 libqrencode4
libsqlite3-0`); the node and the tools need none of them.

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev
git clone https://github.com/<org>/chains.git
cd chains
cmake -B build -DBUILD_GUI=ON
cmake --build build -j$(nproc)
```

Leave out `-DBUILD_GUI=ON` (and the Qt packages) for a node without a window. Other systems and
options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `chains-qt` | the wallet, with its window |
| `chainsd` | the node, without a window |
| `chains-cli` | commands for a running node or wallet |
| `chains-wallet`, `chains-tx`, `chains-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`.

## Running it

```sh
chains-qt                  # the main network
chains-qt -testnet         # the test network
chains-qt -regtest         # a private chain of your own, for trying things out
```

Data and the configuration file `chains.conf` are in `~/.chains` (on Windows
`%LOCALAPPDATA%\Chains`, on macOS `~/Library/Application Support/Chains`); each network other
than the main one has a subfolder (`testnet`, `signet`, `regtest`).

| Network | P2P port | RPC port |
|---|---|---|
| main | 9555 | 9554 |
| testnet | 19555 | 19554 |
| signet | 39555 | 39554 |
| regtest | 29555 | 29554 |

The networks have no DNS seeds: give the node a peer with `addnode`.

A `~/.chains/chains.conf` for a node that runs sidechain nodes next to it:

```ini
# Commands over RPC: sidechain nodes and chains-cli use them (with the cookie file).
server=1
fallbackfee=0.0002

# For the test network instead, uncomment:
# testnet=1
# [test]
# addnode=<peer>:19555

[main]
addnode=<peer>:9555
```

A sidechain node reaches the Chains node over RPC, on the same computer by default, with the
cookie file in the data folder. Run Chains with `server=1` (or `-server`) and then the sidechain;
see the sidechain's README.

## Sidechains in the wallet

- **Sidechains** page: the active sidechains and the proposals; deposits, to a deposit address
  of the sidechain (`s<slot>_<address>_<checksum>`, checked before anything is sent); the
  withdrawal bundles and how this node votes on them.
- **Sidechain Nodes** window: installs, starts and stops sidechain nodes. **Locate programs…**
  takes the `bin` folder of a sidechain build; then **Start node** or **Open wallet**.

## Commands

Drivechains, on the node: `listactivesidechains`, `getsidechain`, `listsidechainproposals`,
`createsidechainproposal`, `acksidechain`, `removesidechainproposal`, `listsidechaindeposits`,
`getsidechainevents`, `listwithdrawalbundles`, `getwithdrawalbundle`, `receivewithdrawalbundle`,
`sendwithdrawalbundle`, `setwithdrawalvote`, `setdefaultwithdrawalvote`, `verifybmm`,
`getdrivechaininfo`, `getaveragefee`.

In the wallet: `createsidechaindeposit` (deposit to a sidechain), `createbmmrequest` (pay miners
to commit to a sidechain block; sidechain nodes do this themselves).

Mining: `getblocktemplate` carries the drivechain messages in the coinbase it asks for, and
`setgenerate` mines on the CPU.

## Drivechains

- **Activation.** A sidechain is proposed in a slot (`createsidechainproposal`) and activates once
  enough blocks ack it: 7 200 blocks (five days) on the main network, 60 on the test network.
- **Deposits** go into the slot's escrow output; the sidechain creates the coins.
- **Withdrawals.** A sidechain node hands its node the bundle it wants paid
  (`receivewithdrawalbundle`). Miners vote on it for up to 129 600 blocks (90 days) on the main
  network, 600 on the test network, and it pays out once it has the score. A node upvotes the
  bundle its own sidechain node handed it and abstains on others; a pool that does not run every
  sidechain can follow the miners who do, `setdefaultwithdrawalvote follow`.
- **Blind merged mining.** A sidechain block exists only if a Chains block commits to it; the
  sidechain pays the miner a fee for that, in CHN.
- The messages (M1 to M8) and the escrow script use the byte layouts of the LayerTwo Labs
  `bip300301_enforcer`, so pools and tools made for it read Chains blocks.

The sidechain side is in the sidechain repositories, `doc/sidechain.md`.

## Credits

Chains is based on [Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its own README is
in [doc/README-bitcoin-core.md](doc/README-bitcoin-core.md). Drivechains are Paul Sztorc's
design (BIP300 and BIP301); the message layouts follow LayerTwo Labs.

## License

MIT: see [COPYING](COPYING).

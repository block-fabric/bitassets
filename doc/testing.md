# Testing Chains

What the tests of Chains check, beyond the tests it inherits from Bitcoin
Core, and how to run them. Every drivechain rule (BIP300 sidechains,
deposits and withdrawals; BIP301 blind merged mining) has unit tests against
the sidechain database directly, functional tests against running nodes, and
fuzz targets.

## Running them

```
cmake -B build && cmake --build build -j4
build/bin/test_bitcoin --run_test=drivechain_tests,chains_limits_tests
python3 build/test/functional/test_runner.py feature_drivechain feature_drivechain_rules feature_drivechain_forks
```

Fuzzing needs clang:

```
cmake -B build_fuzz -DBUILD_FOR_FUZZING=ON -DSANITIZERS=address,fuzzer,undefined \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz -j4
FUZZ=drivechain_scdb build_fuzz/bin/fuzz
FUZZ=drivechain_messages build_fuzz/bin/fuzz
```

## Unit tests

`src/test/drivechain_tests.cpp` connects blocks to a sidechain database through
one helper, which checks three things for **every** block:

- a valid block, undone, gives back exactly the database it was connected to,
  through serialization of the undo data;
- the database written out and read back is the same;
- a block that fails part way is taken back exactly by the undo data it has so
  far. Validation updates the database in place and relies on this.

| Test | What it checks |
|---|---|
| `scripts` | Every message script (M1–M8, escrow, deposit destination) parses back to what built it. |
| `enforcer_byte_layouts` | The exact bytes of the messages, as the LayerTwo Labs enforcer writes and reads them. |
| `proposal_limits` | Proposals that are too large or malformed are not proposals. |
| `activation` | A proposal acked long enough activates its slot. |
| `activation_failures` | A proposal that too many blocks do not ack is dropped. |
| `acks` | Acks count only for the proposal they name; the proposing block counts as one. |
| `acks_are_per_slot` | An ack of a sidechain in one slot does not count for the same sidechain in another. |
| `proposal_cap` | Proposals in a block beyond `MAX_PROPOSALS_PER_BLOCK` are not looked at. |
| `self_replacement_ignored` | Proposing the sidechain a slot already has is ignored (it would fail its bundles). |
| `replacement` | A sidechain is replaced in its slot by a proposal acked long enough. |
| `replacement_fails_pending_bundles` | The bundles of a replaced sidechain fail, so its software refunds them. |
| `deposits` | Deposits move the treasury (ctip) forward and are reported with their destination. |
| `deposit_address_format` | Deposit addresses format and parse back, and bad ones are refused. |
| `treasury_is_never_dust` | A withdrawal may leave the treasury small or empty, and is still relayed. |
| `escrow_inputs_of_two_sidechains` | One transaction cannot spend two treasuries. |
| `withdrawals` | A bundle with enough score can be paid by exactly the transaction it commits to. |
| `withdrawal_paying_more_than_the_treasury` | A withdrawal cannot pay out more than the treasury held. |
| `fee_script_encoding` | The fee output of a blind withdrawal has one encoding (the bundle id depends on it). |
| `withdrawal_votes` | Upvotes, downvotes and abstentions, in the one-byte and two-byte forms. |
| `leading_by_50` | The "leading by 50" vote form upvotes the bundle that leads by the margin. |
| `withdrawal_expiry` | A bundle that can no longer reach the score fails. |
| `full_queue_rejects` | A full queue makes room only by failing a bundle with no more votes than a new one. |
| `full_queue_bundle_after_own_vote` | The miner proposes into a full queue only when that will be accepted. |
| `one_withdrawal_per_sidechain_per_block` | Two bundles of one sidechain cannot both be paid in one block (the double payout). |
| `paying_a_bundle_fails_the_others` | Once one bundle of a sidechain is paid, its other pending bundles fail. |
| `single_payout_activation` | Below `single_payout_height`, the old rule (others stay pending) still applies. |
| `follow_vote` | The miner upvotes the bundle its sidechain node handed it. |
| `upvote_last_handed_bundle` | A bundle handed before the last is one the sidechain gave up: no upvote. |
| `vouching` | The sidechain node vouches for its bundle, or for none; the miner's default vote follows. |
| `idle_bundles_expire` | From `idle_expiry_height`, a bundle at score 0 for `idle_expiry_blocks` fails. |
| `unvoted_bundles_expire` | From `audit2_height`, a bundle that `upvote_expiry_blocks` blocks in a row did not upvote fails; an upvote, in any form, starts the count again. |
| `miner_keeps_its_bundle_upvoted` | The bundle the miner's sidechain node vouches for is upvoted from the block after its proposal on, and paid, however short `upvote_expiry_blocks`. |
| `failed_bundles_are_forgotten` | From `audit2_height`, a failed bundle is forgotten a withdrawal period after it failed (and can be proposed again); a paid one never is. |
| `state_snapshot_and_index` | The failed bundles by height are rebuilt when a snapshot is read. |
| `bmm` | Blind merged mining: a request is valid only with the accept for it, on the block it names. |
| `block_without_coinbase` | A block without a coinbase is refused with a reason, not a crash. |
| `many_blocks_undo` | A long history is undone block by block, matching every state it went through. |
| `undo_holds_changes_not_slots` | The undo data of a block holds its changes (a vote, a removed bundle), not copies of the slots it changed. |
| `database_records` | The drivechain database: undo data kept or erased per block while its events stay, deposits listed after one found by its txid, the snapshot with its version, a failed read that leaves nothing behind, a wipe. |

`src/test/chains_limits_tests.cpp` checks the limits that regtest does not use:

| Test | What it checks |
|---|---|
| `coinbase_maturity_is_360` | Coinbase outputs mature after 360 blocks on every network. |
| `block_weight_split` | Transactions may weigh 4M of the 6M block weight; the rest is the coinbase's. |

## Functional tests

| Test | What it checks |
|---|---|
| `feature_drivechain.py` | The whole life of a sidechain on running nodes: <br>• proposal, acks, activation, rejection; <br>• deposits, and the treasury that nobody can take; <br>• a bundle voted through and paid, one that nobody upvotes failing after `upvote_expiry_blocks`; <br>• a payout mined by a miner without the sidechain; <br>• no drivechain transactions in packages; <br>• blind merged mining; a stale BMM request abandoned by the wallet; <br>• `getsidechainevents` with the proposed bundles; <br>• restarts, reindex and reorgs of the sidechain database. |
| `feature_drivechain_rules.py` | <br>• the RPCs refuse what they cannot do, with a reason; <br>• the mempool takes BMM requests only in output 0 and for an active sidechain; <br>• templates carry the drivechain coinbase outputs for pools; <br>• templates have BMM requests only for mining software that puts the drivechain messages in its coinbase (`coinbasetxn`, or `drivechain` in the capabilities or rules), and a block made by software that does not is valid; <br>• `getsidechainevents` near the largest height, `listsidechaindeposits` at most 1000 at a time; <br>• BMM requests and treasury transactions in the mempool take no unconfirmed children but small ones of their kind; <br>• a reorg evicts deposits to a sidechain whose activation is undone, and withdrawals whose bundle lost its score; <br>• a reorg that takes the first deposit of a sidechain brings it back, with the deposit chained on it. |
| `feature_drivechain_forks.py` | Two branches disagree about a bundle and a deposit: <br>• the nodes rejoin on the longer branch; <br>• the bundle is voted again and paid exactly once; <br>• a deep reorg and back, and a reindex, give the same state. |

On regtest, every block that fails to connect is also checked against a copy
of the sidechain database taken beforehand: the in-place rollback must give it
back exactly.

## Fuzz targets

| Target | What it checks |
|---|---|
| `drivechain_messages` | Fuzzed scripts: <br>• each message parser reads back what its builder makes of the result; <br>• deposit addresses round-trip. |
| `drivechain_scdb` | Blocks of fuzzed proposals, acks, bundles, votes, deposits, withdrawals and BMM accepts, under fuzzed parameters: <br>• every block undone gives back the database before it; <br>• every block that fails part way, undone with its partial undo data, does too; <br>• queues stay within their bound, treasuries within the money range, and no closed bundle is pending. |

## Network faults

The sidechain repositories run the network-fault tests against a Chains node:

- `feature_sidechain_network.py` runs two mainchain nodes and a line of
  sidechain nodes. It covers:
  - a mainchain that splits in two and joins again;
  - a mainchain node that stops;
  - late joiners;
  - restarts and reindexes.
- `feature_sidechain.py` covers:
  - mainchain reorgs across deposits, bundles and BMM;
  - a mainchain node that is gone, or that takes connections and never
    answers;
  - the double-payout attempt.

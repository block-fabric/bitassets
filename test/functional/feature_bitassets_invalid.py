#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BitAssets transactions the rules refuse, and the assets across undo, restarts and reindexes.

- Transactions refused by the mempool (testmempoolaccept, sendrawtransaction), each with its reason:
  markers the rules refuse, and what the state refuses; the mempool's own rules (a token of a
  transaction not in a block yet, two registrations of one name).
- Blocks with such a transaction, refused (generateblock, and submitblock of a block the mainchain
  committed to).
- Blocks that make, bid on and collect an auction, disconnected and connected again.
- The state after a restart, a -reindex and a -reindex-chainstate.
"""

import hashlib
import struct
from decimal import Decimal

from feature_bitassets import BitAssetsTest
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.blocktools import WITNESS_COMMITMENT_HEADER, get_witness_script
from test_framework.messages import (
    COutPoint,
    CBlock,
    CTransaction,
    CTxIn,
    CTxOut,
    from_hex,
    ser_compact_size,
    ser_string,
    tx_from_hex,
    uint256_from_str,
)
from test_framework.script import CScript, OP_RETURN
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

TAG = b"BAST"
ASSET, CONTROL, RESERVATION, LP, RECEIPT = range(5)
# The operations, by the kind byte of a marker.
RESERVE, REGISTER, MINT, UPDATE, BURN, SWAP, ADD, REMOVE, AUCTION, BID, COLLECT, RELEASE = range(1, 13)
CHN = bytes(32)


def varint(n):
    """VARINT of serialize.h: base 128, most significant first, each byte but the last with its top bit and one less."""
    out = bytearray()
    while True:
        out.insert(0, (n & 0x7F) | (0x80 if out else 0))
        if n <= 0x7F:
            return bytes(out)
        n = (n >> 7) - 1


def asset_id(hex_id):
    """The bytes of an asset as the chain has them, from its hash as the commands show it."""
    return bytes.fromhex(hex_id)[::-1]


def name_id(name):
    return hashlib.sha256(name.encode()).digest()


def token(kind, ident, amount=1):
    """A token as a marker lists it: its kind, id and, for coins and shares, amount."""
    return bytes([kind]) + ident + (varint(amount) if kind in (ASSET, LP) else b"")


def marker(kind=0, op=b"", outputs=()):
    """The data of a marker: the tag, version 0, the operation, then the outputs it lists ((n, token or None for a result))."""
    data = TAG + bytes([0, kind]) + op + ser_compact_size(len(outputs))
    for n, tok in outputs:
        data += varint(n) + (b"\xff" if tok is None else tok)
    return data


def marker_script(data):
    return CScript([OP_RETURN, data])


def script_of(node, address):
    return bytes.fromhex(node.getaddressinfo(address)["scriptPubKey"])


class BitAssetsInvalidTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def build(self, node, outputs, inputs=(), sign=True):
        """A transaction of `outputs` ((value in satoshis, script bytes)), spending `inputs` ((txid, n)),
        funded by the wallet of `node`, its change last (so that the outputs keep their places)."""
        tx = CTransaction()
        for txid, n in inputs:
            tx.vin.append(CTxIn(COutPoint(int(txid, 16), n)))
        for value, script in outputs:
            tx.vout.append(CTxOut(value, script))
        funded = node.fundrawtransaction(tx.serialize().hex(), {"changePosition": len(outputs), "fee_rate": 10})["hex"]
        if not sign:
            return funded
        signed = node.signrawtransactionwithwallet(funded)
        assert signed["complete"]
        return signed["hex"]

    def assert_refused(self, node, tx_hex, reason, send=False):
        result = node.testmempoolaccept([tx_hex])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], reason)
        if send:
            assert_raises_rpc_error(-26, reason, node.sendrawtransaction, tx_hex)

    def recommit(self, block):
        """The witness commitment of a block changed by the test, in its place in the coinbase."""
        coinbase = block.vtx[0]
        nonce = 0
        if coinbase.wit.vtxinwit and coinbase.wit.vtxinwit[0].scriptWitness.stack:
            nonce = uint256_from_str(coinbase.wit.vtxinwit[0].scriptWitness.stack[0])
        script = get_witness_script(block.calc_witness_merkle_root(), nonce)
        found = False
        for out in coinbase.vout:
            if bytes(out.scriptPubKey).startswith(bytes([OP_RETURN, 0x24]) + WITNESS_COMMITMENT_HEADER):
                out.scriptPubKey = script
                found = True
        assert found
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()

    def template(self, txs):
        """A block of the node, of these transactions, that the node does not take (generateblock, not submitted)."""
        return from_hex(CBlock(), self.generateblock(self.nodes[0], self.side_address, txs, submit=False, sync_fun=self.no_op)["hex"])

    def submit_invalid(self, tx_hex, reason):
        """A block of the next transactions and `tx_hex`: refused when only checked, and refused once
        the mainchain committed to it and it is submitted."""
        side = self.nodes[0]
        tip = side.getbestblockhash()
        assert_raises_rpc_error(-25, f"TestBlockValidity failed: {reason}", self.template, [tx_hex])
        block = self.template([])
        block.vtx.append(tx_from_hex(tx_hex))
        self.recommit(block)
        self.main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
        self.mine_main()
        assert_equal(side.submitblock(block.serialize().hex()), reason)
        assert_equal(side.getbestblockhash(), tip)
        assert_equal(side.getblockheader(block.hash_hex)["confirmations"], -1)

    def snapshot(self, node):
        """What the node knows of the assets, as the commands show it."""
        return (
            node.getbitassetsinfo(),
            node.listassets(),
            node.listpools(),
            node.listauctions(True),
            node.getassethistory("GOLD"),
            node.listmyassets(),
            node.getsidechainstate(),
        )

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund both wallets, register GOLD")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitAssets", "Assets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): 1 for _ in range(10)})
        other.sendmany("", {other.getnewaddress(): 1 for _ in range(10)})
        self.mine_txs()
        side.reserveasset("GOLD")
        self.mine_txs()
        self.mine_txs()
        registration = side.registerasset("GOLD", 1000, 0, {"info": "Gold"})
        self.mine_txs()
        gold = asset_id(registration["asset"])
        # The control coin, then the supply.
        control = (registration["txid"], 0)
        coins = (registration["txid"], 1)
        mine = script_of(side, side.getnewaddress())

        self.log.info("The mempool refuses markers the rules refuse")
        cases = [
            ("a marker of version 1", [(0, marker_script(TAG + b"\x01\x00\x00"))], (), "bad-ba-marker"),
            ("a marker with a byte too many", [(0, marker_script(marker() + b"\x00"))], (), "bad-ba-marker"),
            ("two markers", [(0, marker_script(marker())), (0, marker_script(marker()))], (), "bad-ba-markers"),
            ("CHN burned without an operation", [(1000, marker_script(marker()))], (), "bad-ba-marker-value"),
            ("an output past the end", [(0, marker_script(marker(outputs=[(5, token(ASSET, gold, 1))])))], (), "bad-ba-outputs"),
            ("a token output of value", [(1000, mine), (0, marker_script(marker(outputs=[(0, token(ASSET, gold, 1))])))], (), "bad-ba-token-output"),
            ("coins of no asset", [(0, mine), (0, marker_script(marker(outputs=[(0, token(ASSET, CHN, 1))])))], (), "bad-ba-token-id"),
            ("a swap of GOLD for GOLD", [(0, marker_script(marker(SWAP, gold + varint(1) + gold + varint(0) + ser_string(b""))))], (), "bad-ba-swap"),
            ("an auction of no blocks", [(0, marker_script(marker(AUCTION, gold + varint(1) + CHN + varint(10) + varint(10) + struct.pack("<ii", 1000, 0))))], (), "bad-ba-auction-duration"),
            ("a registration naming another name", [(0, mine), (0, marker_script(marker(REGISTER, name_id("NOPE") + bytes(32) + varint(1) + b"\x00" + b"\x00" * 6 + b"\x01" + ser_string(b"YEP"), [(0, token(CONTROL, name_id("NOPE")))])))], (), "bad-ba-name-text"),
            # Against the state.
            ("GOLD out of nothing", [(0, mine), (0, marker_script(marker(outputs=[(0, token(ASSET, gold, 5))])))], (), "bad-ba-tokens-unbacked"),
            ("GOLD spent without a marker", [(100000, mine)], [coins], "bad-ba-tokens-lost"),
            ("a registration without a reservation", [(0, mine), (0, mine), (0, marker_script(marker(REGISTER, name_id("NOPE") + bytes(32) + varint(1) + b"\x00" + b"\x00" * 6 + b"\x01" + ser_string(b"NOPE"), [(0, token(CONTROL, name_id("NOPE"))), (1, token(ASSET, name_id("NOPE"), 1))])))], (), "bad-ba-no-reservation"),
            ("a mint of an asset nobody registered", [(0, mine), (0, marker_script(marker(MINT, name_id("NOBODY") + varint(5), [(0, token(ASSET, name_id("NOBODY"), 5))])))], (), "bad-ba-asset-unknown"),
            ("a mint without the control coin", [(0, mine), (0, marker_script(marker(MINT, gold + varint(5), [(0, token(ASSET, gold, 5))])))], (), "bad-ba-no-control"),
            ("a swap in a pool there is not", [(0, marker_script(marker(SWAP, gold + varint(1) + CHN + varint(1) + ser_string(mine))))], [coins], "bad-ba-no-pool"),
            ("a bid on an auction there is not", [(1000, marker_script(marker(BID, bytes(range(32)) + varint(1000) + varint(1) + ser_string(b"")))), ], (), "bad-ba-no-auction"),
            ("retiring GOLD, alive", [(0, marker_script(marker(RELEASE, gold)))], (), "bad-ba-release-alive"),
        ]
        refused = {}
        for name, outputs, inputs, reason in cases:
            self.log.debug(name)
            tx_hex = self.build(side, outputs, inputs)
            self.assert_refused(side, tx_hex, reason)
            refused[name] = tx_hex
        # As sendrawtransaction tells it.
        self.assert_refused(side, refused["two markers"], "bad-ba-markers", send=True)
        self.assert_refused(side, refused["GOLD spent without a marker"], "bad-ba-tokens-lost", send=True)
        self.assert_refused(side, refused["a mint without the control coin"], "bad-ba-no-control", send=True)
        # What each was close to is taken: a mint with the control coin.
        good_mint = self.build(side, [(0, script_of(side, side.getnewaddress())), (0, mine), (0, marker_script(marker(MINT, gold + varint(5), [(0, token(CONTROL, gold)), (1, token(ASSET, gold, 5))])))], [control])
        assert_equal(side.testmempoolaccept([good_mint])[0]["allowed"], True)

        self.log.info("A token of a transaction not in a block yet is not spent")
        sent = side.sendasset(other.getnewaddress(), "GOLD", 10)["txid"]
        self.sync_mempools()
        unconfirmed = self.build(other, [(0, script_of(other, other.getnewaddress())), (0, marker_script(marker(outputs=[(0, token(ASSET, gold, 10))])))], [(sent, 0)])
        self.assert_refused(other, unconfirmed, "ba-token-unconfirmed", send=True)
        # A rule of the mempool, not of blocks: in a block after the transaction, it is spent.
        block = self.template([side.gettransaction(sent)["hex"], unconfirmed])
        main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
        self.mine_main()
        assert_equal(side.submitblock(block.serialize().hex()), None)
        assert_equal(side.getbestblockhash(), block.hash_hex)
        self.sync_blocks()
        assert_equal(self.holding(other, "GOLD")["balance"], 10)
        assert_equal(self.holding(side, "GOLD")["balance"], 990)

        self.log.info("Blocks with what the rules refuse are refused")
        # Made now, of coins not spent since.
        self.submit_invalid(self.build(side, [(1000, marker_script(marker(BID, bytes(range(32)) + varint(1000) + varint(1) + ser_string(b""))))]), "bad-ba-no-auction")
        self.submit_invalid(self.build(side, [(100000, mine)], [(sent, 1)]), "bad-ba-tokens-lost")
        self.submit_invalid(self.build(side, [(0, marker_script(TAG + b"\x01\x00\x00"))]), "bad-ba-marker")
        self.submit_invalid(self.build(side, [(0, mine), (0, marker_script(marker(MINT, gold + varint(5), [(0, token(ASSET, gold, 5))])))]), "bad-ba-no-control")
        # The chain goes on.
        self.bmm()
        self.check_in_sync()

        self.log.info("One registration of a name at a time")
        side.reserveasset("TWICE")
        other.reserveasset("TWICE")
        self.mine_txs()
        self.mine_txs()
        first = side.registerasset("TWICE", 1)["txid"]
        self.sync_mempools()
        # The wallet's registration is refused by the mempool: abandoned, its reservation kept.
        with other.assert_debug_log(["ba-registration-in-mempool"]):
            assert_raises_rpc_error(-4, "not accepted into the mempool", other.registerasset, "TWICE", 1)
        assert_equal([r["name"] for r in other.listmyassets()["reservations"]], ["TWICE"])
        assert "register" not in [a["operation"] for a in other.listassetactivity()]
        abandoned = [t for t in other.listtransactions("*", 100) if t.get("abandoned")]
        assert_equal(len(abandoned), 1)
        self.mine_txs()
        assert_equal(side.getasset("TWICE")["registration"], first)
        assert_equal(other.listmyassets()["reservations"][0]["taken"], True)
        other.releaseassetreservation("TWICE")
        self.mine_txs()
        assert_equal(other.listmyassets()["reservations"], [])

        self.log.info("A pool and an auction, for what follows")
        side.addliquidity("GOLD", 500, "CHN", 5)
        self.mine_txs()
        before_auction = side.getbestblockhash()
        made = side.createauction("GOLD", 100, "CHN", 10, 1, 5)["txid"]
        made_block = self.mine_txs()["hash"]
        bid = other.bidauction(made, 2)["txid"]
        self.mine_txs()
        assert_equal(side.getauction(made)["bids"], 1)

        self.log.info("Blocks of an auction disconnected, then connected again")
        after = self.snapshot(side)
        gold_after = self.holding(side, "GOLD")["balance"]
        side.invalidateblock(made_block)
        assert_equal(side.getbestblockhash(), before_auction)
        assert_raises_rpc_error(-8, "No such auction", side.getauction, made)
        assert_equal(side.listauctions(True), [])
        # The GOLD it sold is back in the coins it was taken from, which the auction, waiting again,
        # spends: what it gives back is on its way again.
        assert_equal(self.holding(side, "GOLD")["balance"], 0)
        assert_equal(self.holding(side, "GOLD")["pending"], gold_after)
        assert_equal(side.getbitassetsinfo()["auctions"], 0)
        # The auction waits again; the bid on it cannot be mined before it, and left the mempool.
        assert made in side.getrawmempool()
        assert bid not in side.getrawmempool()
        side.reconsiderblock(made_block)
        assert_equal(self.snapshot(side), after)
        self.check_in_sync()

        self.log.info("The assets survive a restart, and a reindex rebuilds them")
        side.updateasset("GOLD", {"info": "Gold, kept in Zurich", "ipv4": "203.0.113.7:8333"})
        self.mine_txs()
        expected = self.snapshot(side)
        tip = side.getbestblockhash()
        self.restart_node(0)
        assert_equal(self.snapshot(side), expected)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: side.getbestblockhash() == tip)
        assert_equal(self.snapshot(side), expected)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        self.wait_until(lambda: side.getbestblockhash() == tip)
        assert_equal(self.snapshot(side), expected)
        self.connect_nodes(0, 1)
        # And they go on from there.
        side.mintasset("GOLD", 1)
        self.mine_txs()
        assert_equal(side.getasset("GOLD")["minted"], 1001)
        self.check_in_sync()
        assert_greater_than(len(side.listassetactivity()), 3)


if __name__ == "__main__":
    BitAssetsInvalidTest(__file__).main()

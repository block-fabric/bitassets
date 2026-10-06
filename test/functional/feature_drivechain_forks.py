#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Drivechain consensus across competing forks.

Three nodes; node 2 is split off and mines a longer branch whose drivechain
history conflicts with the other branch: a different deposit spending the same
treasury output, votes against a bundle the other branch paid out. After the
reorg every node must hold the same drivechain state, the losing branch's
deposit and payout must be gone (from blocks and mempools), the bundle must
be paid exactly once in the end, and the treasury must equal deposits minus
payouts. Then a deep invalidate/reconsider and a reindex must land on the same
state.
"""
from decimal import Decimal

from test_framework.address import address_to_scriptpubkey
from test_framework.messages import COIN, CTransaction, CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ACTIVATION_PERIOD = 20
WITHDRAWAL_MIN_SCORE = 30
SLOT = 1


class DrivechainForksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0002"]] * 3

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def states(self, nodes=None):
        return [n.getdrivechaininfo()["statehash"] for n in (nodes or self.nodes)]

    def assert_same_state(self):
        self.sync_all()
        assert_equal(len(set(self.states())), 1)

    def treasury(self, node):
        return node.getsidechain(SLOT)["escrow"]["amount"]

    def check_books(self, node):
        """The treasury is what came in minus what went out, as the node's own deposit list tells it."""
        changes = node.listsidechaindeposits(SLOT)
        total = Decimal(0)
        for change in changes:
            # A payout reports no deposit, only the new total: the drop is what it paid (payouts and fee).
            total = change["total"] if change["destination"] == "D" else total + change["amount"]
            assert_equal(change["total"], total)
        assert_equal(self.treasury(node), total)

    def run_test(self):
        n0, n1, n2 = self.nodes
        self.generate(n0, 110)
        n2_addr = n2.getnewaddress()
        n0.sendtoaddress(n2_addr, 20)
        self.generate(n0, 1)

        self.log.info("Activate a sidechain and fund its treasury")
        n0.createsidechainproposal(SLOT, "Forks", "a sidechain that sees reorgs")
        self.generate(n0, ACTIVATION_PERIOD)
        assert_equal(len(n0.listactivesidechains()), 1)
        n0.createsidechaindeposit(SLOT, "alice", 10)
        self.generate(n0, 1)

        self.log.info("A bundle both branches know, voted up to just short of payable")
        payout_address = n1.getnewaddress()
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])),
                       CTxOut(1 * COIN, address_to_scriptpubkey(payout_address))]
        bundle_hex = bundle.serialize().hex()
        bundle_hash = n0.receivewithdrawalbundle(SLOT, bundle_hex)["hash"]
        assert_equal(n2.receivewithdrawalbundle(SLOT, bundle_hex)["hash"], bundle_hash)
        self.generate(n0, 1)
        score = n0.getwithdrawalbundle(SLOT, bundle_hash)["score"]
        self.generate(n0, WITHDRAWAL_MIN_SCORE - 2 - score)
        assert_equal(n0.getwithdrawalbundle(SLOT, bundle_hash)["score"], WITHDRAWAL_MIN_SCORE - 2)
        self.assert_same_state()
        split_height = n0.getblockcount()

        self.log.info("Split: branch A pays the bundle out and takes a deposit")
        self.disconnect_nodes(0, 2)
        self.disconnect_nodes(1, 2)
        deposit_a = n0.createsidechaindeposit(SLOT, "bob", 2)["txid"]
        self.generate(n0, 3, sync_fun=lambda: self.sync_blocks([n0, n1]))
        assert_equal(n0.getwithdrawalbundle(SLOT, bundle_hash), {"status": "paid"})
        assert_equal(n1.getreceivedbyaddress(payout_address, 1), 1)
        self.check_books(n0)

        self.log.info("Branch B, longer: a conflicting deposit, and votes against the bundle")
        deposit_b = n2.createsidechaindeposit(SLOT, "carol", 3)["txid"]
        n2.setwithdrawalvote(SLOT, "downvote")
        self.generate(n2, 6, sync_fun=self.no_op)
        assert_equal(n2.getwithdrawalbundle(SLOT, bundle_hash)["status"], "pending")
        self.check_books(n2)
        state_b = n2.getdrivechaininfo()["statehash"]

        self.log.info("Rejoin: everyone takes branch B")
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        self.sync_blocks()
        assert_equal(self.states(), [state_b] * 3)
        for n in self.nodes:
            assert_equal(n.getwithdrawalbundle(SLOT, bundle_hash)["status"], "pending")
            assert_equal(self.treasury(n), 13)
            self.check_books(n)
        # The payout and the deposit of branch A spent a treasury output branch B spent otherwise: they are gone for good.
        for n in (n0, n1):
            mempool = n.getrawmempool()
            assert deposit_a not in mempool
            assert all(n.getrawtransaction(txid, True)["vin"][0].get("txid") != deposit_a for txid in mempool)
        assert_equal(n1.getreceivedbyaddress(payout_address, 1), 0)
        assert deposit_b in [tx for tx in n0.getblock(n0.getblockhash(split_height + 1))["tx"]]

        self.log.info("The bundle is voted through again on the winning branch and paid exactly once")
        n2.setwithdrawalvote(SLOT, "default")
        for _ in range(200):
            if n0.getwithdrawalbundle(SLOT, bundle_hash)["status"] != "pending":
                break
            self.generate(n0, 1)
        assert_equal(n0.getwithdrawalbundle(SLOT, bundle_hash), {"status": "paid"})
        self.sync_all()
        assert_equal(n1.getreceivedbyaddress(payout_address, 1), 1)
        assert_equal(self.treasury(n0), 13 - Decimal(1 * COIN + 1000) / COIN)
        self.assert_same_state()
        for n in self.nodes:
            self.check_books(n)

        self.log.info("A deep reorg back past the split and forward again restores the same state")
        final = n0.getdrivechaininfo()["statehash"]
        tip = n0.getbestblockhash()
        deep = n0.getblockhash(split_height - 5)
        n0.invalidateblock(deep)
        assert n0.getdrivechaininfo()["statehash"] != final
        n0.reconsiderblock(deep)
        assert_equal(n0.getbestblockhash(), tip)
        assert_equal(n0.getdrivechaininfo()["statehash"], final)

        self.log.info("A reindexed node agrees")
        self.restart_node(1, extra_args=["-reindex"])
        self.connect_nodes(1, 0)
        self.connect_nodes(1, 2)
        self.sync_blocks()
        assert_equal(self.states(), [final] * 3)


if __name__ == "__main__":
    DrivechainForksTest(__file__).main()

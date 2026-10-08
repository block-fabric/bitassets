#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""What the drivechain database keeps for sidechain software, and how it is kept.

- getsidechainevents reports, for every block, the bundles pending after it with their score: the
  same at any later time, after a reorg, a restart and a rebuild of the database.
- A database derived under other drivechain parameters (an activation height that moved) is rebuilt.
"""
from test_framework.address import address_to_scriptpubkey
from test_framework.messages import COIN, CTransaction, CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Regtest drivechain parameters, see CRegTestParams.
ACTIVATION_PERIOD = 20
WITHDRAWAL_MIN_SCORE = 30
SLOT = 1


class DrivechainHistoryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0002"], ["-fallbackfee=0.0002"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def blind_bundle(self, node, amount):
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])),
                       CTxOut(amount, address_to_scriptpubkey(node.getnewaddress()))]
        return bundle.serialize().hex()

    def pending_now(self, node):
        """The bundles pending at the tip, as the sidechain database has them."""
        return [{"hash": b["hash"], "score": b["score"]} for b in node.listwithdrawalbundles(SLOT)]

    def mine_and_record(self, node, count=1, sync=True):
        """Mine blocks one at a time and remember what is pending after each."""
        for _ in range(count):
            self.generate(node, 1, sync_fun=self.sync_all if sync else self.no_op)
            self.expected[node.getblockcount()] = self.pending_now(node)

    def events(self, node):
        return node.getsidechainevents(SLOT, 0, 2000)

    def check_pending(self, node):
        events = self.events(node)
        assert_equal(len(events), node.getblockcount() + 1)
        for e in events:
            assert_equal(e["pending"], self.expected.get(e["height"], []))
        # Another slot has none, at any height.
        assert all(e["pending"] == [] for e in node.getsidechainevents(SLOT + 1, 0, 2000))
        return events

    def run_test(self):
        self.expected = {}
        self.test_pending()
        self.test_params_change()

    def test_pending(self):
        self.log.info("getsidechainevents: the bundles pending after each block, with their score")
        n0, n1 = self.nodes
        self.generate(n0, 110)
        n0.createsidechainproposal(SLOT, "History")
        self.generate(n0, ACTIVATION_PERIOD)
        n0.createsidechaindeposit(SLOT, "alice", 10)
        self.generate(n0, 1)

        # Bundle A from node 0: proposed in its next block, then upvoted.
        bundle_a = n0.receivewithdrawalbundle(SLOT, self.blind_bundle(n0, COIN))["hash"]
        self.mine_and_record(n0)
        events = n0.getsidechainevents(SLOT, n0.getblockcount())[0]
        assert_equal(events["proposed"], [bundle_a])
        assert_equal(events["pending"], [{"hash": bundle_a, "score": 1}])
        self.mine_and_record(n0, 3)
        assert_equal(n0.getsidechainevents(SLOT, n0.getblockcount())[0]["pending"], [{"hash": bundle_a, "score": 4}])

        # Bundle B from node 1, which upvotes it; node 0 keeps upvoting A, which downvotes B.
        bundle_b = n1.receivewithdrawalbundle(SLOT, self.blind_bundle(n1, COIN // 2))["hash"]
        self.mine_and_record(n1)
        assert_equal([b["hash"] for b in self.pending_now(n0)], [bundle_a, bundle_b])
        self.mine_and_record(n1, 2)
        self.mine_and_record(n0, 2)
        self.check_pending(n0)

        self.log.info("A reorg replaces the records of the blocks it takes out")
        fork = n0.getblockcount()
        self.disconnect_nodes(0, 1)
        self.mine_and_record(n0, 2, sync=False)
        stale = {h: self.expected.pop(h) for h in range(fork + 1, fork + 3)}
        # Node 1's branch, longer: it downvotes A, which so falls behind.
        n1.setwithdrawalvote(SLOT, "downvote")
        self.mine_and_record(n1, 4, sync=False)
        assert stale[fork + 1] != self.expected[fork + 1]
        n1.setwithdrawalvote(SLOT, "default")
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.check_pending(n0)
        self.check_pending(n1)

        self.log.info("Closure: B fails (nobody upvotes it any more), A is paid; nothing is pending after that")
        for _ in range(200):
            if n0.getwithdrawalbundle(SLOT, bundle_a)["status"] != "pending":
                break
            self.mine_and_record(n0)
        assert_equal(n0.getwithdrawalbundle(SLOT, bundle_a), {"status": "paid"})
        assert_equal(n0.getwithdrawalbundle(SLOT, bundle_b), {"status": "failed"})
        before = self.check_pending(n0)
        assert_equal(self.events(n1), before)
        failed_at = [e for e in before if {"hash": bundle_b, "paid": False} in e["bundles"]][0]
        assert_equal([p["hash"] for p in failed_at["pending"]], [bundle_a])
        assert_equal(before[-1]["bundles"], [{"hash": bundle_a, "paid": True}])
        assert_equal(before[-1]["pending"], [])
        assert_equal(before[-2]["pending"], [{"hash": bundle_a, "score": WITHDRAWAL_MIN_SCORE}])

        self.log.info("The same after a restart, and after the database is rebuilt")
        self.restart_node(0)
        assert_equal(self.events(n0), before)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: n0.getblockcount() == before[-1]["height"])
        assert_equal(self.events(n0), before)
        self.events_before = before

    def test_params_change(self):
        self.log.info("A database derived under other drivechain parameters is rebuilt from the blocks")
        n0 = self.nodes[0]
        state = n0.getdrivechaininfo()["statehash"]
        # A parameter this chain's history does not depend on: the state comes out the same.
        for extra in (["-testdrivechainparam=max_pending_bundles@63"], []):
            with n0.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks",
                                      "Bringing the sidechain database from height 0 to the chain tip"]):
                self.restart_node(0, extra_args=self.extra_args[0] + extra)
            assert_equal(n0.getdrivechaininfo()["statehash"], state)
            assert_equal(self.events(n0), self.events_before)
        # Restarted with the same parameters, the snapshot is used.
        with n0.assert_debug_log([], unexpected_msgs=["it is rebuilt from the blocks", "Bringing the sidechain database"]):
            self.restart_node(0)
        assert_equal(n0.getdrivechaininfo()["statehash"], state)
        # A parameter this chain's history depends on: the same blocks give another state.
        with n0.assert_debug_log(["derived under other drivechain parameters"]):
            self.restart_node(0, extra_args=self.extra_args[0] + ["-testdrivechainparam=upvote_expiry_blocks@1000"])
        assert n0.getdrivechaininfo()["statehash"] != state
        self.restart_node(0)
        assert_equal(n0.getdrivechaininfo()["statehash"], state)
        self.connect_nodes(0, 1)
        self.sync_all()


if __name__ == "__main__":
    DrivechainHistoryTest(__file__).main()

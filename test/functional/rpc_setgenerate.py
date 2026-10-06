#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the built-in CPU miner (setgenerate and getgenerate)."""

import time

from test_framework.address import ADDRESS_RCHN1_UNSPENDABLE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet


class SetGenerateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [[], ["-txindex"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("The miner is off when the node starts")
        state = node.getgenerate()
        assert_equal(state["generate"], False)
        assert_equal(state["threads"], 0)
        assert_equal(state["blocksfound"], 0)
        assert "address" not in state

        self.log.info("Invalid requests")
        assert_raises_rpc_error(-8, "An address is required to mine", node.setgenerate, True)
        assert_raises_rpc_error(-5, "Invalid address", node.setgenerate, True, "not an address")
        assert_raises_rpc_error(-8, "Invalid number of threads", node.setgenerate, True, ADDRESS_RCHN1_UNSPENDABLE, 0)
        assert_equal(node.getgenerate()["generate"], False)

        self.log.info("Mine with two threads")
        start = node.getblockcount()
        assert_equal(node.setgenerate(True, ADDRESS_RCHN1_UNSPENDABLE, 2), {"generate": True, "threads": 2})
        self.wait_until(lambda: node.getblockcount() >= start + 3)
        state = node.getgenerate()
        assert_equal(state["generate"], True)
        assert_equal(state["threads"], 2)
        assert_equal(state["address"], ADDRESS_RCHN1_UNSPENDABLE)
        assert_greater_than_or_equal(state["blocksfound"], 3)
        assert_greater_than_or_equal(state["hashes"], 3)

        self.log.info("The blocks pay the address and reach the other node")
        block = node.getblock(node.getblockhash(start + 1), 2)
        assert_equal(block["tx"][0]["vout"][0]["scriptPubKey"]["address"], ADDRESS_RCHN1_UNSPENDABLE)
        self.wait_until(lambda: self.nodes[1].getblockcount() >= start + 3)

        self.log.info("Transactions get mined")
        wallet = MiniWallet(self.nodes[1])
        txid = wallet.send_self_transfer(from_node=self.nodes[1])["txid"]
        self.wait_until(lambda: txid not in self.nodes[1].getrawmempool() and txid not in node.getrawmempool())
        assert_greater_than_or_equal(self.nodes[1].getrawtransaction(txid, True)["confirmations"], 1)

        self.log.info("Changing the number of threads restarts the miner")
        assert_equal(node.setgenerate(True, ADDRESS_RCHN1_UNSPENDABLE, 1), {"generate": True, "threads": 1})

        self.log.info("Stop mining")
        assert_equal(node.setgenerate(False), {"generate": False, "threads": 0})
        height = node.getblockcount()
        time.sleep(2.5)
        assert_equal(node.getblockcount(), height)
        assert_equal(node.getgenerate()["generate"], False)

        self.log.info("A node shuts down cleanly while it mines")
        node.setgenerate(True, ADDRESS_RCHN1_UNSPENDABLE, 2)
        self.restart_node(0)
        assert_equal(node.getgenerate()["generate"], False)


if __name__ == '__main__':
    SetGenerateTest(__file__).main()

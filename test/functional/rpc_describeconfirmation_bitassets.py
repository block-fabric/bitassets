#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""BitAssets's own questions through the host over RPC (src/webui/remotehost.h), as the Chains app asks them.

Each call of BitAssets's pages that pays or signs (its describers of src/webui/local.cpp) is described by
describeconfirmation, run nothing, with what it does in its rows (the values given: names, addresses, texts);
a call changed after its question is refused, and its token used up; the same token twice is refused.
(The kit's own questions, sends, a locked wallet, files: rpc_describeconfirmation.py.)
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

REFUSED = -32802


class LocalDescribeConfirmationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def cases(self, node, address):
        """(method, params, a word of its title, values its rows show)."""
        return [
            ("reserveasset", ["GOLD"], "Reserve", ["GOLD"]),
            ("sendasset", [address, "GOLD", 5], "Send an asset", [address, "GOLD"]),
            ("swapasset", ["GOLD", 100, "CHN", 1, 10, "0.001"], "Swap", ["GOLD", "CHN"]),
            ("addliquidity", ["GOLD", 100, "CHN", "0.2"], "Add liquidity", ["GOLD", "CHN"]),
            ("bidauction", ["aa" * 32, 1], "Bid", ["aa" * 32]),
            ("transferassetcontrol", ["GOLD", address], "Transfer", [address]),
        ]

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("main")
        w = node.get_wallet_rpc("main")
        address = w.getnewaddress()
        self.generatetoaddress(node, 101, address)
        confirmed = node.webuihost({"kind": "host", "method": "rpc.confirmedMethods", "params": {}})["result"]["confirmed"]
        first = None
        for method, params, title, values in self.cases(node, address):
            self.log.info(f"{method}: described, not run")
            assert method in confirmed, f"{method} is not confirmed by the host"
            d = node.describeconfirmation(method, params, "main")
            assert_equal(d["needed"], True)
            assert title.lower() in d["title"].lower(), f"{method}: {d['title']}"
            shown = [r["value"] for r in d["rows"]]
            for v in values:
                assert any(v in s for s in shown), f"{method}: {v} not in {shown}"
            assert_equal(d["request"]["params"]["method"], method)
            first = first or (method, params, d)
        self.log.info("a call changed after its question is refused, its token used up")
        method, params, d = first
        changed = dict(d["request"])
        changed["params"] = dict(d["request"]["params"], params=[p + "x" if isinstance(p, str) else (p * 2 if isinstance(p, (int, float)) and not isinstance(p, bool) else p) for p in params])
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", node.webuihost, changed, d["token"])
        assert_raises_rpc_error(REFUSED, "not known", node.webuihost, d["request"], d["token"])


if __name__ == "__main__":
    LocalDescribeConfirmationTest(__file__).main()

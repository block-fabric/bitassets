#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test listassettransactions: the asset moves of a wallet, with the other side.

- A registration (+ its supply), a send (- the amount, to the address of the other wallet, with the fee), a mint,
  a burn, a logo.
- The other wallet: a receive (+ the amount) from the addresses that paid it, in the mempool and in a block.
- Filters by asset, pages.
"""

from decimal import Decimal

from feature_bitassets import BitAssetsTest
from feature_bitassets_logo import b64, make_png
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class BitAssetsTransactionsTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitAssets", "Assets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): 1 for _ in range(5)})
        self.mine_txs()

        self.log.info("A registration, then a send to the other wallet")
        side.reserveasset("GOLD")
        self.mine_txs()
        self.mine_txs()
        side.registerasset("GOLD", 1000, 2)
        self.mine_txs()
        gold = side.getasset("GOLD")
        to = other.getnewaddress()
        sent = side.sendasset(to, "GOLD", 12.5)["txid"]

        self.log.info("The receiver sees it in the mempool, from the sender's addresses")
        self.sync_mempools()
        received = [e for e in other.listassettransactions() if e["txid"] == sent]
        assert_equal(len(received), 1)
        r = received[0]
        assert_equal((r["category"], r["sign"], r["amount"], r["asset"], r["seq"], r["confirmations"]), ("receive", 1, Decimal("12.50"), gold["asset"], gold["seq"], 0))
        assert_equal(r["to"], [{"address": to, "mine": True, "roles": ["asset"]}])
        senders = {f["address"] for f in r["from"]}
        assert senders and all(side.getaddressinfo(a)["ismine"] for a in senders)
        assert all(not f["mine"] for f in r["from"])
        assert "fee" not in r

        self.mine_txs()
        r = next(e for e in other.listassettransactions() if e["txid"] == sent)
        assert_equal(r["confirmations"], 1)
        assert_equal({f["address"] for f in r["from"]}, senders)
        assert_equal(r["blockhash"], side.getbestblockhash())

        s = next(e for e in side.listassettransactions() if e["txid"] == sent)
        assert_equal((s["category"], s["sign"], s["amount"], s["label"]), ("send", -1, Decimal("12.50"), "GOLD"))
        assert_equal(s["to"], [{"address": to, "mine": False, "roles": ["asset"]}])
        # The sender's change of CHN, its own (the change of GOLD is no destination of the send).
        assert s["change"] and all(side.getaddressinfo(a)["ismine"] for a in s["change"])
        assert "change" not in r
        assert_greater_than(s["fee"], 0)
        assert_equal(s["fee"], -side.gettransaction(sent)["fee"])

        self.log.info("Registration, mint, burn and logo")
        side.mintasset("GOLD", 5)
        self.mine_txs()
        side.burnasset("GOLD", 2)
        self.mine_txs()
        side.setassetlogo("GOLD", b64(make_png([1, 2, 3, 4, 5, 6])))
        self.mine_txs()
        rows = side.listassettransactions()
        assert_equal([e["category"] for e in rows][:4], ["logo", "burn", "mint", "send"])
        assert_equal([(e["sign"], e.get("amount")) for e in rows][:4], [(0, None), (-1, Decimal("2.00")), (1, Decimal("5.00")), (-1, Decimal("12.50"))])
        register = next(e for e in rows if e["category"] == "register")
        assert_equal((register["sign"], register["amount"]), (1, Decimal("1000.00")))
        # Its outputs, the wallet's, each with what it carries: the units and the control coin.
        assert all(p["mine"] for p in register["to"])
        assert_equal(sorted(role for p in register["to"] for role in p["roles"]), ["asset", "control coin"])
        assert any(e["category"] == "reserve" and "asset" not in e for e in rows)

        self.log.info("Filters and pages")
        assert_equal(side.listassettransactions(2, 1), rows[1:3])
        assert_equal(side.listassettransactions(100, 0, "GOLD"), [e for e in rows if e.get("asset") == gold["asset"]])
        assert_equal(other.listassettransactions(100, 0, gold["seq"])[0]["txid"], sent)
        assert_raises_rpc_error(-8, "count", side.listassettransactions, 1001)


if __name__ == "__main__":
    BitAssetsTransactionsTest(__file__).main()

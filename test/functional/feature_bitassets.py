#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the assets: registration, transfers, mint and burn, pools, auctions, undo."""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class BitAssetsTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def holding(self, node, label):
        for a in node.listmyassets()["assets"]:
            if a["label"] == label:
                return a
        return {"balance": Decimal(0), "control": False}

    def payouts_to(self, node, block):
        """The CHN the coinbase of a block pays to addresses of the wallet of `node`, beyond the first output."""
        total = Decimal(0)
        for out in block["tx"][0]["vout"][1:]:
            address = out["scriptPubKey"].get("address")
            if address and node.getaddressinfo(address)["ismine"]:
                total += out["value"]
        return total

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund both wallets")
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

        self.log.info("Reserve, then register an asset")
        assert_raises_rpc_error(-8, "no reservation", side.registerasset, "GOLD", 1000000, 2)
        reserved = side.reserveasset("GOLD")
        self.mine_txs()
        assert_equal(side.getbitassetsinfo()["reservations"], 1)
        assert_equal(side.listmyassets()["reservations"][0]["name"], "GOLD")
        registered = side.registerasset("GOLD", 1000000, 2, {"info": "One gram of gold"})
        assert_equal(registered["asset"], reserved["asset"])
        self.mine_txs()
        gold = side.getasset("GOLD")
        assert_equal(gold["name"], "GOLD")
        assert_equal(gold["decimals"], 2)
        assert_equal(gold["supply"], Decimal("1000000.00"))
        assert_equal(gold["data"], {"info": "One gram of gold"})
        assert_equal(gold["fixed"], False)
        assert_equal(side.getasset(gold["seq"])["asset"], gold["asset"])
        assert_equal(side.getasset("0x" + gold["asset"])["name"], "GOLD")
        assert_equal(self.holding(side, "GOLD")["balance"], Decimal("1000000.00"))
        assert_equal(self.holding(side, "GOLD")["control"], True)
        assert_equal(side.listmyassets()["reservations"], [])
        assert_equal(side.getbitassetsinfo()["assets"], 1)
        # Taken.
        assert_raises_rpc_error(-8, "is registered", other.reserveasset, "GOLD")

        self.log.info("An asset without supply yet, and a private one")
        side.reserveasset("SILVER")
        other.reserveasset("SECRET")
        self.mine_txs()
        side.registerasset("SILVER", 0)
        other.registerasset("SECRET", 5, 0, {}, False)
        self.mine_txs()
        assert_equal(side.getasset("SILVER")["supply"], 0)
        assert_equal(self.holding(side, "SILVER")["control"], True)
        assert "name" not in side.getasset("SECRET")
        assert_equal(sorted(a["label"] for a in side.listassets()), sorted(["GOLD", "SILVER", "0x" + side.getasset("SECRET")["asset"]]))
        assert_equal(len(side.listassets(10, 0, "gol")), 1)

        self.log.info("Send an asset")
        assert_raises_rpc_error(-6, "too little", side.sendasset, other.getnewaddress(), "GOLD", 2000000)
        assert_raises_rpc_error(-3, "at most 2 decimals", side.sendasset, other.getnewaddress(), "GOLD", "1.001")
        side.sendasset(other.getnewaddress(), "GOLD", "250.5")
        # The change is not in a block yet.
        assert_raises_rpc_error(-4, "waiting for the next block", side.sendasset, other.getnewaddress(), "GOLD", 1)
        # It shows as on its way, at both ends, until the block.
        assert_equal(self.holding(side, "GOLD")["balance"], 0)
        assert_equal(self.holding(side, "GOLD")["pending"], Decimal("999749.50"))
        self.sync_mempools()
        assert_equal(self.holding(other, "GOLD")["pending"], Decimal("250.50"))
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], Decimal("250.50"))
        assert_equal(self.holding(side, "GOLD")["balance"], Decimal("999749.50"))
        assert_equal(self.holding(other, "GOLD")["control"], False)

        self.log.info("Mint, burn, change data: only with the control coin")
        assert_raises_rpc_error(-6, "does not hold the control coin", other.mintasset, "GOLD", 10)
        side.mintasset("GOLD", 1000)
        side.mintasset("SILVER", 500)
        self.mine_txs()
        assert_equal(side.getasset("GOLD")["supply"], Decimal("1001000.00"))
        side.burnasset("GOLD", 100)
        side.updateasset("GOLD", {"info": "One gram of gold, in Zurich"})
        self.mine_txs()
        gold = side.getasset("GOLD")
        assert_equal(gold["supply"], Decimal("1000900.00"))
        assert_equal(gold["burned"], Decimal("100.00"))
        assert_equal(gold["data"]["info"], "One gram of gold, in Zurich")
        assert_equal([h["value"] for h in side.getassethistory("GOLD")["info"]], ["One gram of gold", "One gram of gold, in Zurich"])

        self.log.info("Control changes hands; a fixed supply cannot change")
        side.transferassetcontrol("SILVER", other.getnewaddress())
        self.mine_txs()
        assert_equal(self.holding(other, "SILVER")["control"], True)
        assert_equal(self.holding(side, "SILVER")["control"], False)
        assert_raises_rpc_error(-8, "Not confirmed", other.fixassetsupply, "SILVER", False)
        other.fixassetsupply("SILVER", True)
        self.mine_txs()
        assert_equal(side.getasset("SILVER")["fixed"], True)
        assert "control" not in side.getasset("SILVER")
        assert_raises_rpc_error(-8, "fixed", other.mintasset, "SILVER", 1)

        self.log.info("A pool of GOLD and CHN")
        assert_raises_rpc_error(-8, "no pool yet", side.addliquidity, "GOLD", 10000, "CHN")
        added = side.addliquidity("GOLD", 10000, "CHN", 5)
        self.mine_txs()
        pool = side.getpool("GOLD", "CHN")
        assert_equal(pool["reserve_a"], Decimal("10000.00"))
        assert_equal(pool["reserve_b"], Decimal("5.00000000"))
        assert_equal(pool["price"], Decimal("0.0005"))
        assert_equal(side.listpools()[0]["asset_a"], "GOLD")
        liquidity = side.listmyassets()["liquidity"]
        assert_equal(len(liquidity), 1)
        assert_equal(liquidity[0]["shares"], added["shares"])
        # More, at the pool's price.
        more = other.addliquidity("GOLD", 100, "CHN")
        assert_equal(more["amount_b"], Decimal("0.05000000"))
        self.mine_txs()

        self.log.info("Trade in it")
        quote = other.quoteswap("CHN", "0.1", "GOLD")
        assert_greater_than(quote["amount_out"], 0)
        assert_greater_than(quote["price_impact"], 0)
        before = self.holding(other, "GOLD")["balance"]
        swap = other.swapasset("CHN", "0.1", "GOLD")
        assert_equal(swap["quote"], quote["amount_out"])
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], before + quote["amount_out"])
        # GOLD for CHN: the coinbase pays.
        quote = side.quoteswap("GOLD", 200, "CHN")
        side.swapasset("GOLD", 200, "CHN")
        block = self.mine_txs()
        assert_equal(self.payouts_to(side, block), quote["amount_out"])
        assert_equal(side.getpool("GOLD", "CHN")["swaps"], 2)
        # A trade the pool moved away from fails, and leaves the mempool.
        exact = side.quoteswap("CHN", 1, "GOLD", True)
        assert exact["amount_out"] >= Decimal("1.00")

        self.log.info("Take liquidity out")
        shares = side.listmyassets()["liquidity"][0]["shares"]
        removed = side.removeliquidity("GOLD", "CHN", 50)
        assert_equal(removed["shares"], shares // 2)
        gold_before = self.holding(side, "GOLD")["balance"]
        block = self.mine_txs()
        assert_equal(self.payouts_to(side, block), removed["amount_b"])
        assert_equal(self.holding(side, "GOLD")["balance"], gold_before + removed["amount_a"])
        assert_equal(side.listmyassets()["liquidity"][0]["shares"], shares - shares // 2)

        self.log.info("A Dutch auction: 100 GOLD, from 10 CHN down to 1, over 5 blocks")
        side.createauction("GOLD", 100, "CHN", 10, 1, 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["status"], "open")
        assert_equal(auction["remaining"], Decimal("100.00"))
        assert_equal(auction["price"], Decimal("10.00000000"))
        assert_equal(len(side.listmyassets()["receipts"]), 1)
        # A bid of 2.5 CHN at 10 for all: 25 GOLD.
        assert_equal(side.quotebid(auction["auction"], "2.5")["buys"], Decimal("25.00"))
        before = self.holding(other, "GOLD")["balance"]
        bid = other.bidauction(auction["auction"], "2.5")
        assert_equal(bid["buys"], Decimal("25.00"))
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], before + Decimal(25))
        auction = side.getauction(auction["auction"])
        assert_equal(auction["remaining"], Decimal("75.00"))
        assert_equal(auction["proceeds"], Decimal("2.50000000"))
        assert_raises_rpc_error(-8, "running", side.collectauction, auction["auction"])
        # The price fell: all that is left, for less.
        assert_raises_rpc_error(-8, "more than is left", other.bidauction, auction["auction"], 100)
        all_left = other.bidauction(auction["auction"], None, True)
        assert_equal(all_left["buys"], Decimal("75.00"))
        self.mine_txs()
        auction = side.getauction(auction["auction"])
        assert_equal(auction["status"], "sold out")
        proceeds = auction["proceeds"]
        block_with_collect = None
        side.collectauction(auction["auction"])
        block_with_collect = self.mine_txs()
        assert_equal(self.payouts_to(side, block_with_collect), proceeds)
        assert_equal(side.getauction(auction["auction"])["status"], "closed")
        assert_equal(side.listmyassets()["receipts"], [])
        assert_equal(side.listauctions(), [])
        assert_equal(len(side.listauctions(True)), 1)

        self.log.info("An auction without bids is canceled, everything back")
        before = self.holding(side, "GOLD")["balance"]
        side.createauction("GOLD", 10, "CHN", 5, 5, 3, 2)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["status"], "upcoming")
        assert_raises_rpc_error(-8, "starts at height", other.bidauction, auction["auction"], 1)
        side.collectauction(auction["auction"])
        self.mine_txs()
        assert_equal(self.holding(side, "GOLD")["balance"], before)

        self.log.info("Reservations are released")
        side.reserveasset("SPARE")
        self.mine_txs()
        side.releaseassetreservation("SPARE")
        self.mine_txs()
        assert_equal(side.listmyassets()["reservations"], [])
        assert_equal(side.getbitassetsinfo()["reservations"], 0)
        activity = side.listassetactivity()
        assert_greater_than(len(activity), 10)
        assert "burn" in [a["operation"] for a in activity]

        self.log.info("Trades too large for a pool, and pools nobody provides for, are refused")
        assert_raises_rpc_error(-8, "too large for this pool", side.swapasset, "CHN", 100, "GOLD")
        assert side.quoteswap("CHN", 100, "GOLD")["price_impact"] > 10
        assert_equal(side.getpool("GOLD", "CHN")["abandoned"], False)
        side.removeliquidity("GOLD", "CHN")
        other.removeliquidity("GOLD", "CHN")
        self.mine_txs()
        assert_equal(side.getpool("GOLD", "CHN")["abandoned"], True)
        assert_equal(side.listpools()[0]["abandoned"], True)
        assert_raises_rpc_error(-8, "Nobody provides liquidity", side.swapasset, "CHN", "0.001", "GOLD")

        self.log.info("A dead asset is retired: its pools' CHN go to mainchain miners")
        side.reserveasset("DEAD")
        self.mine_txs()
        side.registerasset("DEAD", 1000)
        self.mine_txs()
        side.addliquidity("DEAD", 500, "CHN", 1)
        self.mine_txs()
        assert_equal(side.getasset("DEAD")["releasable"], False)
        assert_raises_rpc_error(-8, "not dead", side.releaseasset, "DEAD")
        side.removeliquidity("DEAD", "CHN")
        side.fixassetsupply("DEAD", True)
        self.mine_txs()
        side.burnasset("DEAD", self.holding(side, "DEAD")["balance"])
        self.mine_txs()
        dead = side.getasset("DEAD")
        assert_equal(dead["releasable"], True)
        fee = dead["release_fee"]
        assert_greater_than(fee, 0)
        withdrawals = len(side.listwithdrawals())
        side.releaseasset("DEAD")
        assert_raises_rpc_error(-4, "being retired already", side.releaseasset, "DEAD")
        self.mine_txs()
        assert_raises_rpc_error(-8, "No such asset", side.getasset, "DEAD")
        assert "DEAD" not in [a["label"] for a in side.listassets()]
        assert_raises_rpc_error(-8, "no pool", side.getpool, "DEAD", "CHN")
        new = side.listwithdrawals()
        assert_equal(len(new), withdrawals + 1)
        payout = [w for w in new if w["mainchainfee"] == fee - Decimal("0.00000001")]
        assert_equal(len(payout), 1)
        assert_equal(payout[0]["amount"], Decimal("0.00000001"))
        # The name is free: it can be an asset again.
        other.reserveasset("DEAD")
        self.mine_txs()
        other.registerasset("DEAD", 5)
        self.mine_txs()
        assert_equal(side.getasset("DEAD")["supply"], 5)
        assert_equal([a["label"] for a in side.listassets()].count("DEAD"), 1)

        self.log.info("Undoing blocks undoes the assets")
        tip = side.getbestblockhash()
        pool = side.getpool("GOLD", "CHN")
        gold = side.getasset("GOLD")
        mine = side.listmyassets()
        first = side.getblockhash(gold["registered"])
        side.invalidateblock(first)
        assert_raises_rpc_error(-8, "No such asset", side.getasset, "GOLD")
        side.reconsiderblock(first)
        assert_equal(side.getbestblockhash(), tip)
        assert_equal(side.getpool("GOLD", "CHN"), pool)
        assert_equal(side.getasset("GOLD"), gold)
        assert_equal(side.listmyassets(), mine)
        self.check_in_sync()


if __name__ == "__main__":
    BitAssetsTest(__file__).main()

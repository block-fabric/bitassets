#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test tickers and display names (doc/bitassets.md, "Tickers and display names").

- Before ticker_height: any name, as before; the wallet refuses a display name, and the mempool a marker that
  sets one (bad-ba-display-name-inactive).
- From it: the name of a new asset is a ticker (2-12 of A-Z, 0-9; the wallet uppercases); an asset registered
  before keeps its name and keeps working (mint, display name).
- A display name set at the registration, shown by getasset, listassets and searchassets, found by search; the
  controller changes and removes it; anyone else is refused (wallet and rules); text the rules refuse is refused.
- A reorganization puts the display name before back, in the state and in the search; both nodes agree.
"""

import unicodedata

from feature_bitassets import BitAssetsTest
from feature_bitassets_invalid import CONTROL, asset_id, marker, marker_script, script_of, token
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.messages import CTransaction, CTxIn, CTxOut, COutPoint, ser_string
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

SET_DISPLAY_NAME = 34
TICKER_HEIGHT = 14


class BitAssetsTickerTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [args + [f"-testactivationheight=ticker@{TICKER_HEIGHT}"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def set_name_tx(self, node, asset, text, control):
        """A transaction of `node`'s wallet whose marker sets the display name of an asset, spending `control` ((txid, n)), or nothing."""
        data = marker(SET_DISPLAY_NAME, asset_id(asset) + ser_string(text.encode()), [(0, token(CONTROL, asset_id(asset)))])
        tx = CTransaction()
        if control:
            tx.vin.append(CTxIn(COutPoint(int(control[0], 16), control[1])))
        tx.vout.append(CTxOut(0, script_of(node, node.getnewaddress())))
        tx.vout.append(CTxOut(0, marker_script(data)))
        funded = node.fundrawtransaction(tx.serialize().hex(), {"changePosition": 2, "fee_rate": 2})["hex"]
        signed = node.signrawtransactionwithwallet(funded)
        assert signed["complete"]
        return signed["hex"]

    def assert_refused(self, node, tx_hex, reason):
        result = node.testmempoolaccept([tx_hex])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], reason)

    def control_of(self, node, name):
        control = node.getasset(name)["control"]
        return (control["txid"], control["vout"])

    def search(self, node, query):
        return [(a["label"], a.get("display_name"), a.get("match")) for a in node.searchassets(query)["assets"]]

    def assert_display(self, name, display):
        for node in self.nodes:
            asset = node.getasset(name)
            assert_equal(asset.get("display_name"), display)
            listed = next(a for a in node.listassets() if a["asset"] == asset["asset"])
            assert_equal(listed.get("display_name"), display)

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
        info = side.getbitassetsinfo()
        assert_equal(info["ticker_height"], TICKER_HEIGHT)
        assert_equal(info["tickers_active"], False)
        assert_equal((info["min_ticker_length"], info["max_ticker_length"], info["max_display_name_bytes"]), (2, 12, 64))

        self.log.info("Before ticker_height: names as before, no display name")
        side.reserveasset("gold bar")
        side.reserveasset("EARLY")
        side.reserveasset("lower")
        self.mine_txs()
        self.mine_txs()
        assert_greater_than(TICKER_HEIGHT - 2, side.getblockcount() + 1)
        assert_raises_rpc_error(-4, "not in the rules of the chain yet", side.registerasset, "EARLY", 100, 0, {}, True, None, "Early bird")
        side.registerasset("gold bar", 500)
        side.registerasset("EARLY", 100)
        self.mine_txs()
        assert_equal(side.getasset("gold bar")["name"], "gold bar")
        assert_raises_rpc_error(-4, "not in the rules of the chain yet", side.setassetdisplayname, "EARLY", "Early bird")
        early_tx = self.set_name_tx(side, side.getasset("EARLY")["asset"], "Early bird", self.control_of(side, "EARLY"))
        self.assert_refused(side, early_tx, "bad-ba-display-name-inactive")

        while side.getblockcount() + 1 < TICKER_HEIGHT:
            self.mine_txs()
        assert_equal(side.getbitassetsinfo()["tickers_active"], True)
        self.log.info("From ticker_height: the marker of before is taken")
        side.sendrawtransaction(early_tx)
        self.mine_txs()
        self.assert_display("EARLY", "Early bird")

        self.log.info("The name of a new asset is a ticker")
        for bad in ["Gold Bar", "G", "ABCDEFGHIJKLM", "GO-LD", "#1", "café"]:
            assert_raises_rpc_error(-8, "is a ticker", side.reserveasset, bad)
        assert_raises_rpc_error(-8, "reads as another asset", side.reserveasset, "chn")
        # A name reserved before that is no ticker is not registered from the height.
        assert_raises_rpc_error(-8, "is a ticker", side.registerasset, "gold bar 2", 1)
        reserved = side.reserveasset("gold")
        side.reserveasset("SILV")
        self.mine_txs()
        self.mine_txs()
        assert_raises_rpc_error(-8, "public cannot be false", side.registerasset, "GOLD", 1, 0, {}, False)
        for bad, why in [("Gold‮Bar", "U+202E"), ("Gold​Bar", "U+200B"), (" Gold", "space"), ("x" * 65, "longer than 64"),
                         (unicodedata.normalize("NFD", "Café"), "U+0301"), ("Gold Bar", "U+00A0")]:
            assert_raises_rpc_error(-8, why, side.registerasset, "GOLD", 1000, 2, {}, True, None, bad)
        registered = side.registerasset("gold", 1000, 2, {"info": "One gram"}, True, None, "Gold Bar Token, 1 g")
        assert_equal(registered["name"], "GOLD")
        assert_equal(registered["asset"], reserved["asset"])
        side.registerasset(name="SILV", supply=50, display_name="Silver and gold dust")
        self.mine_txs()
        self.assert_display("GOLD", "Gold Bar Token, 1 g")
        self.assert_display("SILV", "Silver and gold dust")
        assert "display_name" not in side.getasset("gold bar")

        self.log.info("Found by display name; the names first")
        self.assert_display("gold bar", None)
        assert_equal(self.search(side, "gold"), [("GOLD", "Gold Bar Token, 1 g", "exact"), ("gold bar", None, "prefix"), ("SILV", "Silver and gold dust", "display_word")])
        assert_equal(self.search(side, "gold bar"), [("gold bar", None, "exact"), ("GOLD", "Gold Bar Token, 1 g", "display_name")])
        assert_equal(self.search(side, "token"), [("GOLD", "Gold Bar Token, 1 g", "display_word")])
        assert_equal(self.search(side, "dust"), [("SILV", "Silver and gold dust", "display_word")])
        assert_equal(self.search(other, "silver"), [("SILV", "Silver and gold dust", "display_name")])
        sorted_by_supply = [a["label"] for a in side.searchassets("gold", {}, "supply")["assets"]]
        # In units: 1000 GOLD of 2 decimals are 100000.
        assert_equal(sorted_by_supply, ["GOLD", "gold bar", "SILV"])
        assert_equal([a["label"] for a in side.searchassets("gold", {"exclude_keywords": ["dust"]})["assets"]], ["GOLD", "gold bar"])

        self.log.info("An asset of before keeps working: mint, a display name")
        side.mintasset("gold bar", 5)
        self.mine_txs()
        side.setassetdisplayname("gold bar", "Gold Bar Token, 1 g")
        self.mine_txs()
        assert_equal(side.getasset("gold bar")["supply"], 505)
        # Display names are not unique.
        self.assert_display("gold bar", "Gold Bar Token, 1 g")

        self.log.info("The controller changes it")
        changed = side.setassetdisplayname("GOLD", "Gold Bar", {"fee_rate": 3})
        assert_equal(changed["display_name"], "Gold Bar")
        assert_equal(changed["fee"], side.getmempoolentry(changed["txid"])["fees"]["base"])
        self.mine_txs()
        self.assert_display("GOLD", "Gold Bar")
        change_block = side.getbestblockhash()
        side.setassetdisplayname("GOLD", "金条 Gold")
        self.mine_txs()
        self.assert_display("GOLD", "金条 Gold")
        unicode_block = side.getbestblockhash()
        assert_equal(self.search(side, "金条")[0][:2], ("GOLD", "金条 Gold"))

        self.log.info("Nobody else can")
        assert_raises_rpc_error(-6, "control coin", other.setassetdisplayname, "GOLD", "Mine")
        stolen = self.set_name_tx(other, side.getasset("GOLD")["asset"], "Mine", None)
        self.assert_refused(other, stolen, "bad-ba-no-control")

        self.log.info("Display names the rules refuse")
        for bad in ["Gold‮Bar", "GO‍LD", "﻿GOLD", "Gold\n", "Gold ", "Å"]:
            assert_raises_rpc_error(-8, "bad-ba-display-name", side.setassetdisplayname, "GOLD", bad)
            self.assert_refused(side, self.set_name_tx(side, side.getasset("GOLD")["asset"], bad, self.control_of(side, "GOLD")), "bad-ba-display-name")

        self.log.info("A reorganization puts the display name before back")
        side.invalidateblock(unicode_block)
        self.assert_display_on(side, "GOLD", "Gold Bar")
        side.invalidateblock(change_block)
        self.assert_display_on(side, "GOLD", "Gold Bar Token, 1 g")
        # Both have it again (not unique): the older first.
        assert_equal([r[:2] for r in self.search(side, "token")], [("gold bar", "Gold Bar Token, 1 g"), ("GOLD", "Gold Bar Token, 1 g")])
        side.reconsiderblock(change_block)
        self.assert_display_on(side, "GOLD", "金条 Gold")
        self.check_in_sync()

        self.log.info("The controller removes it")
        side.setassetdisplayname("GOLD", "")
        self.mine_txs()
        self.assert_display("GOLD", None)
        assert_raises_rpc_error(-8, "no display name to remove", side.setassetdisplayname, "GOLD", "")
        assert_equal(self.search(side, "金条"), [])
        self.check_in_sync()

        self.log.info("Its transactions are listed")
        assert any(r["category"] == "display_name" and r["label"] == "GOLD" for r in side.listassettransactions(1000))

        self.log.info("A restart keeps it all (no index derived again)")
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.assert_display("SILV", "Silver and gold dust")
        assert_equal(self.search(side, "dust"), [("SILV", "Silver and gold dust", "display_word")])
        self.check_in_sync()

    def assert_display_on(self, node, name, display):
        assert_equal(node.getasset(name).get("display_name"), display)


if __name__ == "__main__":
    BitAssetsTickerTest(__file__).main()

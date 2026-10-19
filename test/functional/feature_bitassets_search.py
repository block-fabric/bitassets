#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the search index of the assets: searchassets, searchpools, searchauctions, getassetindexinfo.

- A couple of thousand assets, registered by the wallets in a few blocks (RPC batches).
- Every order and filter of searchassets against the same computed here from the assets as the
  state has them (listassets, listpools), page by page with the cursor, at several page sizes.
- Activity, volume over the window (regtest: 10 blocks) and its expiry, pools, logos, burns,
  retirement, the wallet's (mine), a number or a hash.
- A block disconnected and connected again, a restart, and the other node: the same pages.
"""

from decimal import Decimal
import os
import re
import threading

from feature_bitassets import BitAssetsTest
from feature_bitassets_logo import b64, make_png
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

# (A smaller count, for a quick run while working on it: BITASSETS_SEARCH_COUNT=600.)
COUNT = int(os.environ.get("BITASSETS_SEARCH_COUNT", 2000))
WINDOW = 10
WORDS = ["Gold", "silver", "Coin", "token", "MOON", "doge", "cat", "pepe", "inu", "dao", "swap", "chain", "layer", "meta", "ape", "frog"]


def norm(name):
    return name.strip(" ").lower()


def words(text):
    out = []
    for w in re.findall(r"[a-z0-9]+", text):
        if w not in out:
            out.append(w)
    return out


def later_words(text):
    """The words that start somewhere after the start of the name: those the index lists the name under."""
    out = []
    for m in re.finditer(r"[a-z0-9]+", text):
        if m.start() > 0 and m.group(0) not in out:
            out.append(m.group(0))
    return out


def tier(name, text):
    """How a normalised name matches normalised text: 0 whole, 1 start, 2 start of a later word, None."""
    if not text:
        return None
    if name == text:
        return 0
    if name.startswith(text):
        return 1
    for p in range(1, len(name) - len(text) + 1):
        if name[p].isalnum() and not name[p - 1].isalnum() and name[p:].startswith(text):
            return 2
    return None


class BitAssetsSearchTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()
        self.rpc_timeout = 600
        # Thousands of wallet transactions: no trace of each database write.
        self.extra_args = [args + ["-debugexclude=walletdb", "-debugexclude=estimatefee"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def pages(self, node, query="", filters=None, sort="relevance", limit=50, method="searchassets"):
        """Every page of a search, by its cursor: the assets in order (the app's positional arguments, null for no cursor)."""
        out, cursor, calls = [], None, 0
        while True:
            page = getattr(node, method)(query, filters or {}, sort, cursor, limit)
            assert len(page["assets"]) <= limit
            calls += 1
            out += page["assets"]
            if page["next_cursor"] is None:
                return out
            cursor = page["next_cursor"]
            assert calls < 10000

    def rows(self, node):
        """Every asset as the index has it, by number (oldest first), the retired ones included."""
        return self.pages(node, "", {"exclude_retired": False}, "oldest", 200)

    def expect(self, table, query="", filters=None, sort="relevance"):
        """What a search returns, computed from the rows (`table`, oldest first) as the index orders them."""
        f = filters or {}
        height = self.nodes[0].getblockcount()
        text = norm(query)
        seq_rank = {r["seq"]: i for i, r in enumerate(table)}
        out = []
        for r in table:
            if f.get("exclude_retired", True) and r["retired"]:
                continue
            if f.get("has_pool") and not r["has_pool"]:
                continue
            if f.get("has_logo") and not r["has_logo"]:
                continue
            if f.get("exclude_burned") and r["supply"] == 0:
                continue
            if "min_supply" in f and r["supply"] < Decimal(str(f["min_supply"])):
                continue
            within = min([WINDOW if f[k] is True else f[k] for k in ("active_within_blocks", "exclude_dead") if k in f], default=None)
            if within is not None and r["last_activity"] < height - within + 1:
                continue
            if "created_after" in f and r["created"] <= f["created_after"]:
                continue
            name = norm(r.get("name", ""))
            if any(w in words(name) for kw in f.get("exclude_keywords", []) for w in words(norm(kw))):
                continue
            t = None
            if text:
                if "name" not in r:
                    continue
                t = tier(name, text)
                if t is None:
                    continue
            out.append((r, name, t))
        rank = lambda e: seq_rank[e[0]["seq"]]
        if sort == "relevance" and text:
            first = re.match(r"[a-z0-9]*", text).group(0)

            def key(e):
                r, name, t = e
                if t < 2:
                    return (0, name.encode(), rank(e))
                # By the first of its later words that starts with the text's first word.
                w = min(w for w in later_words(name) if w.startswith(first))
                return (1, w.encode(), rank(e))
            out.sort(key=key)
        elif sort == "name":
            out.sort(key=lambda e: (0, e[1].encode(), rank(e)) if "name" in e[0] else (1, b"", rank(e)))
        elif sort == "newest":
            out.sort(key=lambda e: -rank(e))
        elif sort == "oldest":
            out.sort(key=rank)
        elif sort == "supply":
            out.sort(key=lambda e: (-e[0]["supply"] * 10 ** e[0]["decimals"], rank(e)))
        elif sort in ("activity", "relevance"):
            out.sort(key=lambda e: (-e[0]["last_activity"], rank(e)))
        elif sort == "volume":
            out = sorted((e for e in out if e[0]["volume"] > 0), key=lambda e: (-e[0]["volume"], rank(e)))
        elif sort == "pool_depth":
            out = sorted((e for e in out if e[0]["pool_depth"] > 0), key=lambda e: (-e[0]["pool_depth"], rank(e)))
        return [e[0]["seq"] for e in out]

    def check(self, node, table, query="", filters=None, sort="relevance", limits=(50,)):
        expected = self.expect(table, query, filters, sort)
        for limit in limits:
            got = self.pages(node, query, filters, sort, limit)
            assert_equal([a["seq"] for a in got], expected)
        return expected

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund both wallets")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitAssets", "Assets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 40)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): Decimal("0.005") for _ in range(COUNT // 2 + 10)})
        other.sendmany("", {other.getnewaddress(): Decimal("0.005") for _ in range(COUNT // 2 + 10)})
        self.mine_txs()
        info = side.getassetindexinfo()
        assert_equal(info["count"], 0)
        assert_equal(info["volume_window"], WINDOW)
        assert_equal(info["synced"], True)
        assert_equal(side.searchassets()["assets"], [])

        self.log.info(f"Register {COUNT} assets, a few blocks of RPC batches")
        names = ["GOLD", "Golden Goose", "my gold coin", "goldy gold", "GOLD scam airdrop", "$gold", "Silver", "gold x gold coin"]
        i = 0
        while len(names) < COUNT:
            a, b = WORDS[i % len(WORDS)], WORDS[(i * 7 + 3) % len(WORDS)]
            names.append(f"{a} {b} {i}" if i % 3 else f"{a}-{i}")
            i += 1
        # Half by each wallet, at once (a wallet's own work grows with what it holds); at most 400 at a
        # time: the wallet does not spend while more than 500 of its transactions wait for a block.
        owner = [other if k >= 8 and k % 2 == 1 and k != 9 else side for k in range(COUNT)]
        supplies = {}
        reqs = {side: [], other: []}
        for k, n in enumerate(names):
            supply = 0 if k % 10 == 9 else (k * 7919) % 1000000 + 1 if k >= 8 else 1000000 + k
            supplies[n] = supply
            # Every 50th private (no name published).
            reqs[owner[k]].append((n, (n, supply, k % 4, {}, k % 50 != 49)))

        def run_batches(node, method, args):
            for r in node.batch([getattr(node, method).get_request(*a) for a in args]):
                assert r.get("error") is None, r

        def both(method, start):
            threads = [threading.Thread(target=run_batches, args=(node, method, [a if method == "registerasset" else (a[0],) for a in
                                                                                 [x[1] for x in reqs[node][start:start + 400]]]))
                       for node in (side, other)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.mine_txs()

        for start in range(0, COUNT // 2 + 4, 400):
            both("reserveasset", start)
        self.mine_txs()
        for start in range(0, COUNT // 2 + 4, 400):
            both("registerasset", start)
        info = side.getassetindexinfo()
        assert_equal(info["count"], COUNT)
        assert_equal(info["retired"], 0)
        assert_equal(other.getassetindexinfo()["count"], COUNT)

        self.log.info("Activity: transfers, mints, burns, pools, swaps, logos, auctions, a retirement")
        side.sendasset(other.getnewaddress(), "my gold coin", 10)
        side.sendasset(other.getnewaddress(), "goldy gold", 1)
        side.mintasset("Silver", 1000)
        side.burnasset("Golden Goose", side.getasset("Golden Goose")["supply"])
        side.setassetlogo("GOLD", b64(make_png([255, 0, 0, 0, 0, 255])))
        side.setassetlogo(names[300], b64(make_png([0, 255, 0, 0, 0, 255])))
        # Supply 0, fixed: dead, and retired in the next block.
        dead = names[9]
        assert_equal(supplies[dead], 0)
        side.fixassetsupply(dead, True)
        self.mine_txs()
        touched = self.nodes[0].getblockcount()
        side.releaseasset(dead)
        for n in ["GOLD", "goldy gold", "Silver", names[100], names[200]]:
            side.addliquidity(n, 1000, "CHN", "0.5")
        self.mine_txs()
        side.swapasset("CHN", "0.04", "GOLD")
        side.swapasset("CHN", "0.01", "Silver")
        side.createauction("GOLD", 10, "CHN", 1, "0.1", 30)
        side.createauction("Silver", 10, "CHN", 1, "0.1", 8)
        self.mine_txs()
        swapped = self.nodes[0].getblockcount()
        info = side.getassetindexinfo()
        assert_equal(info["count"], COUNT)
        assert_equal(info["retired"], 1)
        assert_equal(info["pools"], 5)

        table = self.rows(side)
        assert_equal(len(table), COUNT)
        by_name = {r["name"]: r for r in table if "name" in r}
        by_seq = {r["seq"]: r for r in table}
        # As the state has them.
        listed = []
        for skip in range(0, COUNT, 1000):
            listed += side.listassets(1000, skip)
        assert_equal(len(listed), COUNT - 1)
        live = {a["asset"]: a for a in listed}
        heights = [r["created"] for r in table]
        assert_equal(heights, sorted(heights))
        for r in table:
            if r["retired"]:
                continue
            a = live[r["asset"]]
            for k in ("seq", "decimals", "supply", "minted", "burned", "fixed"):
                assert_equal(r[k], a[k])
            assert_equal(r["created"], a["registered"])
            assert_equal(r.get("name"), a.get("name"))
            assert_equal(r["has_logo"], "logo" in a)
        assert_equal([r["retired"] for r in table].count(True), 1)
        assert_equal(next(r for r in table if r["retired"]).get("name"), dead)
        pools = side.listpools()
        assert_equal(len(pools), 5)
        for p in pools:
            r = by_name[p["asset_a"]]
            assert_equal(r["has_pool"], True)
            assert_equal(r["pool_depth"], p["reserve_b"])
        assert_equal(by_name["GOLD"]["last_activity"], swapped)
        assert_equal(by_name["Silver"]["last_activity"], swapped)
        assert_equal(by_name["my gold coin"]["last_activity"], touched)
        assert_equal(by_name["Golden Goose"]["supply"], 0)
        assert_equal(by_name["GOLD"]["has_logo"], True)
        assert_greater_than(by_name["GOLD"]["volume"], Decimal("0.039"))
        assert_greater_than(by_name["Silver"]["volume"], Decimal("0.0099"))
        assert_equal(by_name["goldy gold"]["volume"], 0)

        self.log.info("Every order and filter, against the same computed here, at several page sizes")
        for sort in ["relevance", "name", "newest", "oldest", "supply", "activity", "volume", "pool_depth"]:
            self.check(side, table, "", {}, sort, limits=(200, 37) if sort in ("name", "supply") else (200,))
        gold = self.check(side, table, "gold", {}, "relevance", limits=(50, 3))
        assert_equal(gold[0], by_name["GOLD"]["seq"])
        assert by_name["$gold"]["seq"] in gold and by_name["my gold coin"]["seq"] in gold
        matches = [a["match"] for a in self.pages(side, "gold")]
        assert_equal(matches, sorted(matches, key=["exact", "prefix", "word"].index))
        assert_equal(matches[:2], ["exact", "prefix"])
        assert_equal(next(a for a in self.pages(side, "gold") if a["name"] == "my gold coin")["match"], "word")
        for query in ["GOLD", "coin", "moon cat", "pepe", "inu 1", "  Swap ", "Gold-1", "zzz", "$", "gold coin"]:
            for sort in ["relevance", "supply", "name", "newest", "activity"]:
                self.check(side, table, query, {}, sort)
        filters = [
            {"exclude_retired": False},
            {"has_pool": True},
            {"has_logo": True},
            {"exclude_burned": True},
            {"min_supply": 500000},
            {"min_supply": "12.5", "exclude_burned": True},
            {"active_within_blocks": 3},
            {"exclude_dead": 2},
            {"exclude_dead": True},
            {"created_after": table[COUNT // 2]["created"]},
            {"exclude_keywords": ["doge", "PEPE", "frog inu"]},
            {"exclude_keywords": ["scam"], "exclude_burned": True, "min_supply": 10},
        ]
        for f in filters:
            for sort in ["relevance", "name", "supply", "activity", "newest", "pool_depth"]:
                self.check(side, table, "", f, sort)
            self.check(side, table, "gold", f, "relevance")
            self.check(side, table, "coin", f, "supply")

        self.log.info("A number, a hash, the wallet's")
        r = by_name["Silver"]
        for query in [r["seq"], "#" + r["seq"], r["asset"], "0x" + r["asset"]]:
            assert_equal([a["asset"] for a in side.searchassets(query)["assets"]], [r["asset"]])
        assert_equal(side.searchassets("0000-0000-0001")["assets"], [])
        retired = next(r for r in table if r["retired"])
        assert_equal(side.searchassets(retired["asset"])["assets"], [])
        for node in (side, other):
            mine = {a["asset"] for a in node.listmyassets()["assets"] if a["balance"] > 0 or a["control"]}
            got = self.pages(node, "", {}, "name", 200, "searchmyassets")
            assert_equal({a["asset"] for a in got}, mine)
            assert_equal([a["seq"] for a in got], [s for s in self.expect(table, "", {}, "name") if by_seq[s]["asset"] in mine])
            got = self.pages(node, "gold", {}, "relevance", 7, "searchmyassets")
            assert_equal([a["seq"] for a in got], [s for s in self.expect(table, "gold") if by_seq[s]["asset"] in mine])
        assert {"goldy gold", "my gold coin"} <= {a.get("name") for a in self.pages(other, "gold", {}, "relevance", 50, "searchmyassets")}
        # The wallet's fields of getasset come with each (unless retired).
        page = side.searchassets("GOLD")["assets"][0]
        assert_equal(page["logo"]["hash"], side.getasset("GOLD")["logo"]["hash"])
        assert_equal(page["registered"], page["created"])
        assert "control" in page and "releasable" in page
        # The node's command reads no wallet: mine is the wallet's command.
        assert_raises_rpc_error(-8, "searchmyassets", side.searchassets, "", {"mine": True})
        # An explicit list of assets instead.
        listed = [by_name["GOLD"]["seq"], "Silver", "0x" + by_name["my gold coin"]["asset"]]
        assert_equal([a["name"] for a in side.searchassets("", {"assets": listed}, "name")["assets"]], ["GOLD", "my gold coin", "Silver"])
        # Several wallets: the request names one.
        side.createwallet("second")
        assert_raises_rpc_error(-19, "Multiple wallets are loaded", side.searchmyassets)
        assert_equal(side.get_wallet_rpc("second").searchmyassets()["assets"], [])
        assert_equal(len(side.get_wallet_rpc(self.default_wallet_name).searchmyassets("", {}, "name", None, 200)["assets"]), 200)
        side.unloadwallet("second")

        self.log.info("Bad arguments")
        assert_raises_rpc_error(-8, "Unknown sort", side.searchassets, "", {}, "price")
        assert_raises_rpc_error(-8, "Unknown filter", side.searchassets, "", {"colour": "red"})
        assert_raises_rpc_error(-8, "limit is from 1 to 200", side.searchassets, "", {}, "name", "", 201)
        name_cursor = side.searchassets("", {}, "name", None, 1)["next_cursor"]
        assert_raises_rpc_error(-8, "not one of this search", side.searchassets, "", {}, "supply", name_cursor)
        assert_raises_rpc_error(-8, "not one a search returned", side.searchassets, "", {}, "supply", "zz")

        self.log.info("Pools and auctions")
        pools = side.searchpools()
        assert_equal(len(pools["pools"]), 5)
        depths = [p["depth"] for p in pools["pools"]]
        assert_equal(depths, sorted(depths, reverse=True))
        by_volume = side.searchpools("", {}, "volume")["pools"]
        assert_equal([p["asset_a"] for p in by_volume], ["GOLD", "Silver"])
        changes = [p["price_change"] for p in side.searchpools("", {}, "price_change")["pools"]]
        assert_equal(changes, sorted(changes, reverse=True))
        assert_greater_than(changes[0], 0)
        pooled = ["GOLD", "goldy gold", "Silver", names[100], names[200]]
        assert_equal(sorted(p["asset_a"] for p in side.searchpools("gold")["pools"]), sorted(n for n in pooled if tier(norm(n), "gold") is not None))
        newest = side.searchpools("", {}, "newest", "", 2)
        assert_equal(len(newest["pools"]), 2)
        rest = side.searchpools("", {}, "newest", newest["next_cursor"], 200)
        assert_equal(rest["next_cursor"], None)
        # The app's filters and sorts of the assets are taken by the pools and auctions too.
        assert_equal(len(side.searchpools("", {"exclude_dead": True, "has_logo": True}, "pool_depth")["pools"]), 5)
        assert_equal([p["asset_a"] for p in side.searchpools("", {"exclude_keywords": ["silver"]}, "activity")["pools"]], ["GOLD"])
        assert_equal(len(rest["pools"]), 3)
        auctions = side.searchauctions()["auctions"]
        assert_equal([a["base"] for a in auctions], ["Silver", "GOLD"])
        assert_equal(sorted(a["base"] for a in side.searchauctions("", {}, "newest")["auctions"]), ["GOLD", "Silver"])
        assert_equal([a["base"] for a in side.searchauctions("silver")["auctions"]], ["Silver"])

        self.log.info("Volume leaves the window; a block taken back and connected again")
        for _ in range(WINDOW - 1):
            self.mine_txs()
        assert_greater_than(side.searchassets("GOLD")["assets"][0]["volume"], 0)
        self.mine_txs()
        assert_equal(side.searchassets("GOLD")["assets"][0]["volume"], 0)
        assert_equal(side.searchassets("", {}, "volume")["assets"], [])
        assert_equal(side.searchpools("", {}, "volume")["pools"], [])
        # Ended auctions are no longer active.
        assert_equal([a["base"] for a in side.searchauctions()["auctions"]], ["GOLD"])
        assert_equal([a["base"] for a in side.searchauctions("", {"status": "ended"})["auctions"]], ["Silver"])
        before = [self.rows(side), self.pages(side, "gold", {}, "relevance"), self.pages(side, "", {}, "activity", 200)]
        side.mintasset("GOLD", 5)
        side.sendasset(other.getnewaddress(), names[500], 1)
        self.mine_txs()
        tip = side.getbestblockhash()
        after = self.rows(side)
        assert_equal(next(r for r in after if r.get("name") == "GOLD")["last_activity"], side.getblockcount())
        side.invalidateblock(tip)
        assert_equal([self.rows(side), self.pages(side, "gold", {}, "relevance"), self.pages(side, "", {}, "activity", 200)], before)
        side.reconsiderblock(tip)
        assert_equal(self.rows(side), after)

        self.log.info("The other node, and after a restart: the same")
        self.sync_blocks()
        assert_equal(self.rows(other), after)
        assert_equal(other.getassetindexinfo(), side.getassetindexinfo())
        self.restart_node(0)
        assert_equal(self.rows(self.nodes[0]), after)
        # Without the wallet: the search, but no searchmyassets.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-disablewallet"])
        assert_equal(self.rows(self.nodes[0]), after)
        assert_raises_rpc_error(-32601, "Method not found", self.nodes[0].searchmyassets)
        assert_raises_rpc_error(-8, "searchmyassets", self.nodes[0].searchassets, "", {"mine": True})
        self.check_in_sync()


if __name__ == '__main__':
    BitAssetsSearchTest(__file__).main()

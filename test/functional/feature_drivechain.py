#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test drivechains: sidechain proposals, deposits, withdrawals and blind merged mining.

Node 0 mines and takes the drivechain decisions. Node 1 only validates; after
every step both nodes must have the same sidechain database.
"""
from decimal import Decimal

from test_framework.messages import (
    tx_from_hex,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_RETURN,
)
from test_framework.address import address_to_scriptpubkey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Regtest drivechain parameters, see CRegTestParams.
ACTIVATION_PERIOD = 20
ACTIVATION_MAX_FAILURES = 9
WITHDRAWAL_PERIOD = 60
WITHDRAWAL_MIN_SCORE = 30
UPVOTE_EXPIRY_BLOCKS = 20

SLOT = 1


class DrivechainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0002"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, count=1):
        return self.generate(self.nodes[0], count)

    def check_in_sync(self):
        self.sync_all()
        info = [node.getdrivechaininfo() for node in self.nodes]
        assert_equal(info[0]["statehash"], info[1]["statehash"])
        assert_equal(info[0]["height"], self.nodes[0].getblockcount())
        return info[0]

    def run_test(self):
        node = self.nodes[0]
        self.mine(101)

        self.test_proposals()
        self.test_deposits()
        self.test_withdrawal()
        self.test_withdrawal_by_any_miner()
        self.test_mempool_drivechain_rules()
        self.test_bmm()
        self.test_restart_and_reindex()
        self.test_reorg()

    def test_proposals(self):
        self.log.info("Propose a sidechain and ack it until it activates")
        node = self.nodes[0]
        info = node.getdrivechaininfo()
        assert_equal(info["activationperiod"], ACTIVATION_PERIOD)
        assert_equal(info["activesidechains"], 0)

        assert_raises_rpc_error(-8, "out of range", node.createsidechainproposal, 512, "too far")
        assert_raises_rpc_error(-8, "Invalid proposal", node.createsidechainproposal, SLOT, "")
        proposal = node.createsidechainproposal(SLOT, "Testchain", "A sidechain for testing", "11" * 32, "22" * 20)
        assert_equal(len(node.listsidechainproposals()["queued"]), 1)
        assert_equal(node.listsidechainproposals()["pending"], [])

        self.mine()
        proposals = node.listsidechainproposals()
        assert_equal(proposals["queued"], [])
        assert_equal(len(proposals["pending"]), 1)
        pending = proposals["pending"][0]
        assert_equal(pending["proposalhash"], proposal["proposalhash"])
        assert_equal(pending["title"], "Testchain")
        assert_equal(pending["age"], 1)
        assert_equal(pending["ack"], True)
        assert_equal(pending["blocksleft"], ACTIVATION_PERIOD - 1)
        # The other node sees the proposal in the chain but does not ack it itself.
        self.sync_all()
        assert_equal(self.nodes[1].listsidechainproposals()["pending"][0]["ack"], False)

        self.mine(ACTIVATION_PERIOD - 2)
        assert_equal(node.listactivesidechains(), [])
        assert_equal(node.listsidechainproposals()["pending"][0]["acks"], ACTIVATION_PERIOD - 2)
        self.mine()
        assert_equal(node.listsidechainproposals()["pending"], [])
        active = node.listactivesidechains()
        assert_equal(len(active), 1)
        assert_equal(active[0]["slot"], SLOT)
        assert_equal(active[0]["title"], "Testchain")
        assert_equal(active[0]["hashid1"], "11" * 32)
        assert_equal(active[0]["activationheight"], node.getblockcount())
        assert "escrow" not in active[0]
        assert_equal(node.getsidechain(SLOT)["proposalhash"], proposal["proposalhash"])
        assert_raises_rpc_error(-8, "No active sidechain", node.getsidechain, 2)
        assert_equal(self.check_in_sync()["activesidechains"], 1)
        # Software that adds its own votes learns how many entries a vote has, and the bundles of each.
        assert_equal(node.getblocktemplate({"rules": ["segwit"]})["drivechain_votable"], [0])
        assert "mediantime" in node.getsidechainevents(SLOT, node.getblockcount())[0]

        self.log.info("A proposal that miners do not ack is rejected")
        rejected = node.createsidechainproposal(2, "Unwanted")
        node.acksidechain(rejected["proposalhash"], False)
        self.mine()
        assert_equal(len(node.listsidechainproposals()["pending"]), 1)
        self.mine(ACTIVATION_MAX_FAILURES - 1)
        assert_equal(node.listsidechainproposals()["pending"][0]["failures"], ACTIVATION_MAX_FAILURES - 1)
        # Stop proposing it, or the node would make the proposal again as soon as it is gone.
        assert_equal(node.removesidechainproposal(rejected["proposalhash"]), True)
        assert_equal(node.removesidechainproposal(rejected["proposalhash"]), False)
        self.mine()
        assert_equal(node.listsidechainproposals()["pending"], [])
        assert_equal(self.check_in_sync()["activesidechains"], 1)

    def test_deposits(self):
        self.log.info("Deposit to the sidechain")
        node = self.nodes[0]
        assert_raises_rpc_error(-8, "No active sidechain", node.createsidechaindeposit, 3, "alice", 1)
        assert_raises_rpc_error(-8, "Invalid destination", node.createsidechaindeposit, SLOT, "D", 1)
        assert_raises_rpc_error(-8, "Invalid destination", node.createsidechaindeposit, SLOT, "x" * 101, 1)
        # A deposit address names its sidechain and has a checksum; a destination in another form is taken as it is.
        deposit_address = "s1_erin_b91ea9"
        assert_raises_rpc_error(-5, "is for the sidechain in slot 1, not for the one in slot 2", node.createsidechaindeposit, 2, deposit_address, 1)
        assert_raises_rpc_error(-5, "not valid: check it for typing errors", node.createsidechaindeposit, SLOT, deposit_address[:-1] + "0", 1)
        assert_raises_rpc_error(-5, "not valid: check it for typing errors", node.createsidechaindeposit, SLOT, deposit_address.replace("erin", "eric"), 1)

        first = node.createsidechaindeposit(SLOT, "alice", 5)
        assert_equal(first["total"], 5)
        # A second deposit before the first one confirms spends the escrow output the first one creates.
        second = node.createsidechaindeposit(SLOT, "bob", 2)
        assert_equal(second["total"], 7)
        assert_equal(set(node.getrawmempool()), {first["txid"], second["txid"]})
        spent = node.getrawtransaction(second["txid"], True)["vin"]
        assert first["txid"] in [vin["txid"] for vin in spent]
        self.sync_mempools()

        block = self.mine()[0]
        escrow = node.getsidechain(SLOT)["escrow"]
        assert_equal(escrow["txid"], second["txid"])
        assert_equal(escrow["amount"], 7)
        deposits = node.listsidechaindeposits(SLOT)
        assert_equal([d["destination"] for d in deposits], ["alice", "bob"])
        assert_equal([d["amount"] for d in deposits], [5, 2])
        assert_equal([d["total"] for d in deposits], [5, 7])
        assert_equal([d["txid"] for d in deposits], [first["txid"], second["txid"]])
        assert_equal(deposits[0]["blockhash"], block)
        assert_equal(deposits[0]["confirmations"], 1)
        tx = node.decoderawtransaction(deposits[1]["hex"])
        assert_equal(tx["vout"][deposits[1]["burnindex"]]["scriptPubKey"]["hex"], node.getsidechain(SLOT)["escrowscript"])
        assert_equal(node.listsidechaindeposits(SLOT, first["txid"]), deposits[1:])
        assert_equal(node.listsidechaindeposits(SLOT, second["txid"]), [])
        assert_equal(len(node.listsidechaindeposits(SLOT, None, 1)), 1)
        assert_raises_rpc_error(-8, "not an escrow change", node.listsidechaindeposits, SLOT, "00" * 32)
        assert_equal(self.nodes[1].listsidechaindeposits(SLOT), deposits)
        assert_equal(self.check_in_sync()["escrowtotal"], 7)

        self.log.info("Nobody can take the coins out of the escrow")
        address = node.getnewaddress()
        theft = node.createrawtransaction([{"txid": escrow["txid"], "vout": escrow["n"]}], [{address: Decimal("6.999")}])
        assert_raises_rpc_error(-26, "bad-dc-escrow-spend", node.sendrawtransaction, theft)
        assert_raises_rpc_error(-25, "bad-dc-escrow-spend", self.generateblock, node, address, [theft])

        # Nor move them back into the escrow while keeping a part: that is a withdrawal nobody approved.
        skim = CTransaction()
        skim.vin = [CTxIn(COutPoint(int(escrow["txid"], 16), escrow["n"]))]
        skim.vout = [
            CTxOut(6 * COIN, bytes.fromhex(node.getsidechain(SLOT)["escrowscript"])),
            CTxOut(1 * COIN, address_to_scriptpubkey(address)),
        ]
        assert_raises_rpc_error(-26, "bad-dc-withdrawal-unknown", node.sendrawtransaction, skim.serialize().hex())
        assert_raises_rpc_error(-25, "bad-dc-withdrawal-unknown", self.generateblock, node, address, [skim.serialize().hex()])

    def test_withdrawal(self):
        self.log.info("Vote a withdrawal bundle through and pay it out")
        node = self.nodes[0]
        payout_address = self.nodes[1].getnewaddress()
        payout = 3 * COIN
        fee = COIN // 100

        # The bundle as a sidechain builds it, in the blind form of BIP300: no inputs, and the fee in place of the treasury output.
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [
            CTxOut(0, CScript([OP_RETURN, fee.to_bytes(8, "big")])),
            CTxOut(payout, address_to_scriptpubkey(payout_address)),
        ]
        assert_raises_rpc_error(-8, "No active sidechain", node.receivewithdrawalbundle, 3, bundle.serialize().hex())
        bundle_hash = node.receivewithdrawalbundle(SLOT, bundle.serialize().hex())["hash"]
        assert_equal(node.listwithdrawalbundles(), [])
        assert_equal(node.getwithdrawalbundle(SLOT, bundle_hash), {"status": "unknown"})

        self.mine()
        # The node has the bundle's transaction: it says what it pays, while miners vote.
        payouts = [{"amount": Decimal(payout) / COIN, "script": address_to_scriptpubkey(payout_address).hex()}]
        proposed_at = node.getblockcount()
        assert_equal(node.getwithdrawalbundle(SLOT, bundle_hash), {"status": "pending", "score": 1, "lastupvote": proposed_at, "blocksleft": WITHDRAWAL_PERIOD - 1, "payable": False, "payouts": payouts, "fee": Decimal(fee) / COIN})
        bundles = node.listwithdrawalbundles(SLOT)
        assert_equal(len(bundles), 1)
        assert_equal(bundles[0]["hash"], bundle_hash)
        assert_equal(bundles[0]["score"], 1)
        assert_equal(bundles[0]["blocksleft"], WITHDRAWAL_PERIOD - 1)
        assert_equal(bundles[0]["payable"], False)
        assert_equal(bundles[0]["known"], True)
        assert_equal(bundles[0]["payouts"], payouts)
        assert_equal(bundles[0]["fee"], Decimal(fee) / COIN)
        # By default a node upvotes the bundle a sidechain node handed to it, and no other.
        assert_equal(bundles[0]["vote"], "upvote")
        self.sync_all()
        assert_equal(self.nodes[1].listwithdrawalbundles()[0]["known"], False)
        # A node without it leaves them out: unknown, not "pays nothing".
        assert "payouts" not in self.nodes[1].listwithdrawalbundles()[0]
        assert "payouts" not in self.nodes[1].getwithdrawalbundle(SLOT, bundle_hash)
        assert_equal(self.nodes[1].listwithdrawalbundles()[0]["vote"], "abstain")
        assert_raises_rpc_error(-8, "vote must be upvote, follow, downvote or abstain", node.setdefaultwithdrawalvote, "yes")
        # Following the miners who check bundles is opt-in.
        assert_equal(node.getdrivechaininfo()["defaultwithdrawalvote"], "upvote")
        node.setdefaultwithdrawalvote("follow")
        assert_equal(node.getdrivechaininfo()["defaultwithdrawalvote"], "follow")
        node.setdefaultwithdrawalvote("abstain")
        assert_equal(node.listwithdrawalbundles()[0]["vote"], "abstain")

        # Without votes nothing moves.
        self.mine(2)
        assert_equal(node.listwithdrawalbundles()[0]["score"], 1)

        assert_raises_rpc_error(-8, "needs the hash", node.setwithdrawalvote, SLOT, "upvote")
        node.setwithdrawalvote(SLOT, "downvote")
        self.mine()
        assert_equal(node.listwithdrawalbundles()[0]["score"], 0)
        node.setwithdrawalvote(SLOT, "upvote", bundle_hash)
        assert_equal(node.listwithdrawalbundles()[0]["vote"], "upvote")
        self.mine(WITHDRAWAL_MIN_SCORE - 1)
        assert_equal(node.listwithdrawalbundles()[0]["score"], WITHDRAWAL_MIN_SCORE - 1)
        assert_equal(node.listwithdrawalbundles()[0]["lastupvote"], node.getblockcount())
        assert_equal(self.nodes[1].getreceivedbyaddress(payout_address, 0), 0)
        self.mine()
        bundles = node.listwithdrawalbundles()
        assert_equal(bundles[0]["score"], WITHDRAWAL_MIN_SCORE)
        assert_equal(bundles[0]["payable"], True)
        self.check_in_sync()

        # A deposit in the same block as the payout: the payout spends the escrow output the deposit leaves.
        deposit = node.createsidechaindeposit(SLOT, "carol", 1)
        block = node.getblock(self.mine()[0], 2)
        assert_equal(block["tx"][1]["txid"], deposit["txid"])
        withdrawal = block["tx"][-1]
        assert_equal(withdrawal["vin"][0]["txid"], deposit["txid"])
        assert_equal(node.listwithdrawalbundles(), [])
        escrow = node.getsidechain(SLOT)["escrow"]
        assert_equal(escrow["txid"], withdrawal["txid"])
        # Sidechain software learns what the withdrawal paid, with the bundle it paid out.
        events = node.getsidechainevents(SLOT, block["height"], 1)[0]["deposits"]
        paid = [e for e in events if e["destination"] == "D"]
        assert_equal(len(paid), 1)
        assert_equal(paid[0]["bundle"], bundle_hash)
        assert_equal(paid[0]["payouts"], [{"amount": Decimal(payout) / COIN, "script": address_to_scriptpubkey(payout_address).hex()}])
        # BIP300 M6: the treasury output is the only input, and the new one is output 0.
        assert_equal(len(withdrawal["vin"]), 1)
        assert_equal(escrow["n"], 0)
        assert_equal(withdrawal["vout"][0]["scriptPubKey"]["hex"], node.getsidechain(SLOT)["escrowscript"])
        # Sidechain software asks what became of the bundle, on any node.
        self.sync_all()
        for n in self.nodes:
            assert_equal(n.getwithdrawalbundle(SLOT, bundle_hash), {"status": "paid"})
        # The block has the deposit and the withdrawal, which paid a fee.
        average = node.getaveragefee(1)
        assert_equal(average["transactions"], 2)
        assert_greater_than(average["feeaverage"], 0)
        assert_raises_rpc_error(-8, "between 1 and 1000", node.getaveragefee, 0)
        assert_equal(escrow["amount"], Decimal(8) - Decimal(payout + fee) / COIN)
        self.sync_all()
        assert_equal(self.nodes[1].getreceivedbyaddress(payout_address, 1), Decimal(payout) / COIN)
        # The miner collected the fee of the bundle.
        assert node.getblockstats(block["hash"])["totalfee"] >= fee

        deposits = node.listsidechaindeposits(SLOT)
        assert_equal([d["destination"] for d in deposits], ["alice", "bob", "carol", "D"])
        assert_equal(deposits[-1]["amount"], 0)
        assert_equal(deposits[-1]["total"], escrow["amount"])
        self.check_in_sync()

        # A bundle that was paid out cannot come back.
        node.receivewithdrawalbundle(SLOT, bundle.serialize().hex())
        self.mine()
        assert_equal(node.listwithdrawalbundles(), [])

        self.log.info("A bundle nobody votes for fails")
        other = CTransaction()
        other.vin = bundle.vin
        other.vout = bundle.vout[:1] + [CTxOut(1 * COIN, address_to_scriptpubkey(payout_address))]
        node.setwithdrawalvote(SLOT, "default")
        other_hash = node.receivewithdrawalbundle(SLOT, other.serialize().hex())["hash"]
        self.mine()
        assert_equal(len(node.listwithdrawalbundles()), 1)
        # It gets no upvote: once UPVOTE_EXPIRY_BLOCKS blocks in a row did not upvote it, it fails
        # (well before the blocks left could no longer bring it to the score).
        assert_equal(node.getdrivechaininfo()["upvoteexpiryblocks"], UPVOTE_EXPIRY_BLOCKS)
        self.mine(UPVOTE_EXPIRY_BLOCKS)
        assert_equal(len(node.listwithdrawalbundles()), 1)
        self.mine()
        assert_equal(node.listwithdrawalbundles(), [])
        self.sync_all()
        for n in self.nodes:
            assert_equal(n.getwithdrawalbundle(SLOT, other_hash), {"status": "failed"})
            assert_equal(n.getwithdrawalbundle(SLOT, bundle_hash), {"status": "paid"})
        # What a sidechain reads to follow this chain: every block, with what it did to the sidechain.
        for n in self.nodes:
            events = n.getsidechainevents(SLOT, 0, 2000)
            assert_equal([e["height"] for e in events], list(range(n.getblockcount() + 1)))
            assert "previousblockhash" not in events[0]
            assert all(events[i]["previousblockhash"] == events[i - 1]["hash"] for i in range(1, len(events)))
            closed = {b["hash"]: b["paid"] for e in events for b in e["bundles"]}
            assert_equal(closed, {bundle_hash: True, other_hash: False})
            assert_equal(events[-1]["bundles"], [{"hash": other_hash, "paid": False}])
            deposits = [d for e in events for d in e["deposits"]]
            assert_equal([{k: d[k] for k in ("destination", "amount", "txid", "burnindex")} for d in deposits],
                         [{k: d[k] for k in ("destination", "amount", "txid", "burnindex")} for d in n.listsidechaindeposits(SLOT)])
            # Withdrawals say which bundle they paid, and what.
            assert all(("bundle" in d and "payouts" in d) == (d["destination"] == "D") for d in deposits)
            assert "D" in [d["destination"] for d in deposits]
            # Each bundle is reported proposed once, by the block that proposed it.
            proposed = [h for e in events for h in e["proposed"]]
            assert_equal(sorted(proposed), sorted([bundle_hash, other_hash]))
            assert_equal(n.getsidechainevents(SLOT + 1, 0, 2000), [{k: v for k, v in e.items() if k != "bmm"} | {"deposits": [], "bundles": [], "proposed": [], "pending": []} for e in events])
            assert_equal(n.getsidechainevents(SLOT, n.getblockcount() + 1, 5), [])
        assert_raises_rpc_error(-8, "between 1 and 2000", node.getsidechainevents, SLOT, 0, 2001)
        # The record of a closed bundle goes away with the block that closed it.
        failing_block = node.getbestblockhash()
        node.invalidateblock(failing_block)
        assert_equal(node.getwithdrawalbundle(SLOT, other_hash)["status"], "pending")
        node.reconsiderblock(failing_block)
        assert_equal(node.getwithdrawalbundle(SLOT, other_hash), {"status": "failed"})
        self.check_in_sync()

    def test_withdrawal_by_any_miner(self):
        self.log.info("A miner that does not run the sidechain mines the payout of a bundle from the mempool")
        node, pool = self.nodes
        payout_address = pool.getnewaddress()
        payout = COIN // 2
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [
            CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])),
            CTxOut(payout, address_to_scriptpubkey(payout_address)),
        ]
        bundle_hash = node.receivewithdrawalbundle(SLOT, bundle.serialize().hex())["hash"]
        assert_equal(pool.sendwithdrawalbundle(SLOT, bundle_hash), {"sent": False, "reason": "this node was not handed the bundle (receivewithdrawalbundle)"})
        assert_equal(node.sendwithdrawalbundle(SLOT, bundle_hash), {"sent": False, "reason": "the bundle is not pending"})
        node.setwithdrawalvote(SLOT, "default")
        node.setdefaultwithdrawalvote("upvote")
        self.mine(WITHDRAWAL_MIN_SCORE)
        assert_equal(node.listwithdrawalbundles(SLOT)[0]["score"], WITHDRAWAL_MIN_SCORE)
        # The pool was never handed the bundle: with no 50-vote lead to follow, it abstains.
        self.sync_all()
        assert_equal(pool.listwithdrawalbundles(SLOT)[0]["vote"], "abstain")

        sent = node.sendwithdrawalbundle(SLOT, bundle_hash)
        assert_equal(sent["sent"], True)
        assert_equal(node.sendwithdrawalbundle(SLOT, bundle_hash), {"sent": False, "reason": "the withdrawal is in the mempool already"})
        self.sync_mempools()
        assert sent["txid"] in pool.getrawmempool()
        block = pool.getblock(self.generate(pool, 1)[0], 2)
        assert sent["txid"] in [tx["txid"] for tx in block["tx"]]
        for n in self.nodes:
            assert_equal(n.getwithdrawalbundle(SLOT, bundle_hash), {"status": "paid"})
        assert_equal(pool.getreceivedbyaddress(payout_address, 1), Decimal(payout) / COIN)
        self.check_in_sync()

    def bmm_script(self, side_hash, prev):
        return "6a44" + "00bf00" + "%02x" % SLOT + bytes.fromhex(side_hash)[::-1].hex() + bytes.fromhex(prev)[::-1].hex()

    def test_mempool_drivechain_rules(self):
        self.log.info("Drivechain transactions do not travel in packages, and treasury inputs carry nothing")
        node = self.nodes[0]
        tip = node.getbestblockhash()
        # A parent and a child that each ask for a different sidechain block in the next block:
        # each passes on its own, the two together would make every block template invalid.
        parent = node.fundrawtransaction(node.createrawtransaction([], [{"data": self.bmm_script("11" * 32, tip)[4:]}]), {"changePosition": 1, "fee_rate": 0.05})["hex"]
        parent = node.signrawtransactionwithwallet(parent)["hex"]
        parent_tx = node.decoderawtransaction(parent)
        assert_equal(parent_tx["vout"][0]["scriptPubKey"]["hex"], self.bmm_script("11" * 32, tip))
        change = parent_tx["vout"][1]
        child = node.createrawtransaction([{"txid": parent_tx["txid"], "vout": 1}], [{"data": self.bmm_script("22" * 32, tip)[4:]}, {node.getnewaddress(): change["value"] - Decimal("0.001")}])
        child = node.signrawtransactionwithwallet(child, [{"txid": parent_tx["txid"], "vout": 1, "scriptPubKey": change["scriptPubKey"]["hex"], "amount": change["value"]}])["hex"]
        result = node.submitpackage([parent, child])
        assert_equal(result["package_msg"], "package-drivechain-tx")
        assert_equal(node.getrawmempool(), [])

        # A deposit whose treasury input carries a scriptSig is refused: a third party could pad a
        # withdrawal that way so that the original, whose fee is fixed, could not replace it.
        escrow = node.getsidechain(SLOT)["escrow"]
        coin = node.listunspent()[0]
        padded = CTransaction()
        padded.vin = [CTxIn(COutPoint(int(escrow["txid"], 16), escrow["n"]), CScript([b"x" * 100])),
                      CTxIn(COutPoint(int(coin["txid"], 16), coin["vout"]))]
        padded.vout = [CTxOut(int(escrow["amount"] * COIN) + COIN // 10, bytes.fromhex(node.getsidechain(SLOT)["escrowscript"])),
                       CTxOut(0, CScript([OP_RETURN, b"pad"])),
                       CTxOut(int(coin["amount"] * COIN) - COIN // 10 - 10000, address_to_scriptpubkey(node.getnewaddress()))]
        # Signing rewrites the scriptSig of the input the wallet cannot sign: pad it afterwards. The
        # signature of the wallet's (segwit) input does not cover the scriptSig of the other one.
        signed = tx_from_hex(node.signrawtransactionwithwallet(padded.serialize().hex())["hex"])
        assert_equal(signed.vin[0].scriptSig, b"")
        signed.vin[0].scriptSig = CScript([b"x" * 100])
        assert_raises_rpc_error(-26, "dc-escrow-input-not-empty", node.sendrawtransaction, signed.serialize().hex())
        # Without the padding it is an ordinary deposit (its destination output follows the treasury output).
        signed.vin[0].scriptSig = CScript()
        assert node.sendrawtransaction(signed.serialize().hex()) in node.getrawmempool()
        self.mine()

    def test_bmm(self):
        self.log.info("Blind merged mining")
        node = self.nodes[0]
        side_hash = "ab" * 32
        assert_raises_rpc_error(-4, "not accepted into the mempool", node.createbmmrequest, 3, side_hash, Decimal("0.001"))

        request = node.createbmmrequest(SLOT, side_hash, Decimal("0.001"))
        assert_equal(request["prevblockhash"], node.getbestblockhash())
        assert request["txid"] in node.getrawmempool()
        block = self.mine()[0]
        assert_equal(node.getrawmempool(), [])
        verified = node.verifybmm(block, SLOT, side_hash)
        assert_equal(verified["verified"], True)
        assert_equal(verified["height"], node.getblockcount())
        assert_equal(node.verifybmm(block, SLOT, "cd" * 32)["verified"], False)
        assert_equal(node.getsidechainevents(SLOT, node.getblockcount())[0]["bmm"], side_hash)
        assert "bmm" not in node.getsidechainevents(2, node.getblockcount())[0]
        assert_equal(node.verifybmm(block, 2, side_hash)["verified"], False)
        self.sync_all()
        assert_equal(self.nodes[1].verifybmm(block, SLOT, side_hash)["verified"], True)

        # A better offer for the same sidechain replaces the one in the mempool; a worse one is refused.
        low = node.createbmmrequest(SLOT, "01" * 32, Decimal("0.0005"))
        assert_raises_rpc_error(-26, "Outbid", node.createbmmrequest, SLOT, "02" * 32, Decimal("0.0001"))
        high = node.createbmmrequest(SLOT, "03" * 32, Decimal("0.002"))
        assert_equal(node.getrawmempool(), [high["txid"]])
        assert low["txid"] != high["txid"]
        block = self.mine()[0]
        assert_equal(node.verifybmm(block, SLOT, "03" * 32)["verified"], True)
        assert_equal(node.verifybmm(block, SLOT, "01" * 32)["verified"], False)

        # A request is only good for the next block.
        stale = node.createbmmrequest(SLOT, "04" * 32, Decimal("0.001"))
        assert stale["txid"] in node.getrawmempool()
        block = self.generateblock(node, node.getnewaddress(), [])["hash"]
        assert_equal(node.getrawmempool(), [])
        assert_equal(node.verifybmm(block, SLOT, "04" * 32)["verified"], False)
        # The wallet gives its coins back: the request can never be mined.
        assert all(d["abandoned"] for d in node.gettransaction(stale["txid"])["details"])
        raw = node.gettransaction(stale["txid"])["hex"]
        assert_raises_rpc_error(-26, "dc-bmm-prev-block", node.sendrawtransaction, raw)
        assert_raises_rpc_error(-25, "bad-dc-bmm-prev-block", self.generateblock, node, node.getnewaddress(), [raw])
        self.check_in_sync()

    def test_restart_and_reindex(self):
        self.log.info("The sidechain database survives restarts and is rebuilt by a reindex")
        node = self.nodes[0]
        before = node.getdrivechaininfo()
        deposits = node.listsidechaindeposits(SLOT)
        sidechains = node.listactivesidechains()

        # What the node was told to do as a miner is kept as well.
        queued = node.createsidechainproposal(7, "Kept across restarts")
        node.acksidechain("77" * 32, True, 5)
        assert_raises_rpc_error(-1, "not supported on Chains", node.loadtxoutset, "snapshot.dat")
        assert_raises_rpc_error(-8, "No proposal with this hash is pending", node.acksidechain, "78" * 32)
        node.setdefaultwithdrawalvote("downvote")
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [
            CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])),
            CTxOut(COIN // 2, address_to_scriptpubkey(node.getnewaddress())),
        ]
        bundle_hash = node.receivewithdrawalbundle(SLOT, bundle.serialize().hex())["hash"]
        node.setwithdrawalvote(SLOT, "upvote", bundle_hash)

        self.restart_node(0, extra_args=self.extra_args[0])
        assert_equal(node.getdrivechaininfo(), before | {"defaultwithdrawalvote": "downvote"})
        assert_equal(node.listsidechaindeposits(SLOT), deposits)
        assert_equal([p["proposalhash"] for p in node.listsidechainproposals()["queued"]], [queued["proposalhash"]])

        # Mine one block with those settings: it proposes the sidechain and the bundle.
        self.connect_nodes(0, 1)
        self.mine()
        proposals = node.listsidechainproposals()
        assert_equal(proposals["queued"], [])
        assert_equal(proposals["pending"][0]["proposalhash"], queued["proposalhash"])
        assert_equal(proposals["pending"][0]["ack"], True)
        bundles = node.listwithdrawalbundles(SLOT)
        assert_equal(bundles[0]["hash"], bundle_hash)
        assert_equal(bundles[0]["known"], True)
        assert_equal(bundles[0]["vote"], "upvote")
        # Back to a state the rest of the test does not have to care about.
        node.removesidechainproposal(queued["proposalhash"])
        node.setdefaultwithdrawalvote("abstain")
        node.setwithdrawalvote(SLOT, "downvote")
        self.mine(ACTIVATION_MAX_FAILURES + 1)
        assert_equal(node.listsidechainproposals()["pending"], [])
        node.setwithdrawalvote(SLOT, "default")
        self.mine(WITHDRAWAL_PERIOD - WITHDRAWAL_MIN_SCORE)
        assert_equal(node.listwithdrawalbundles(), [])
        before = self.check_in_sync()
        deposits = node.listsidechaindeposits(SLOT)

        for option in ["-reindex-chainstate", "-reindex"]:
            self.restart_node(0, extra_args=self.extra_args[0] + [option])
            self.wait_until(lambda: node.getblockcount() == before["height"])
            assert_equal(node.getdrivechaininfo(), before)
            assert_equal(node.listactivesidechains(), sidechains)
            assert_equal(node.listsidechaindeposits(SLOT), deposits)
        self.connect_nodes(0, 1)
        self.check_in_sync()

    def test_reorg(self):
        self.log.info("A reorg rewinds the sidechain database")
        node = self.nodes[0]
        before = node.getdrivechaininfo()
        deposits = node.listsidechaindeposits(SLOT)

        deposit = node.createsidechaindeposit(SLOT, "dave", 4)
        first = self.mine()[0]
        proposal = node.createsidechainproposal(5, "Second")
        # Stay below the depth at which a node stops returning transactions of disconnected blocks to its mempool.
        self.mine(5)
        pending = node.listsidechainproposals()["pending"]
        assert_equal(len(pending), 1)
        assert_equal(pending[0]["acks"], 4)
        assert_equal(len(node.listsidechaindeposits(SLOT)), len(deposits) + 1)
        after = self.check_in_sync()
        assert after["statehash"] != before["statehash"]

        for n in self.nodes:
            n.invalidateblock(first)
        for n in self.nodes:
            info = n.getdrivechaininfo()
            assert_equal(info["statehash"], before["statehash"])
            assert_equal(info["height"], before["height"])
            assert_equal(n.listsidechaindeposits(SLOT), deposits)
            assert_equal(n.listsidechainproposals()["pending"], [])
        # The proposal is back in the queue of the node that made it, and the deposit back in the mempool.
        assert_equal(len(node.listsidechainproposals()["queued"]), 1)
        assert deposit["txid"] in node.getrawmempool()

        for n in self.nodes:
            n.reconsiderblock(first)
        assert_equal(self.check_in_sync()["statehash"], after["statehash"])
        assert_equal(node.listsidechainproposals()["pending"][0]["proposalhash"], proposal["proposalhash"])
        assert_equal(len(node.listsidechaindeposits(SLOT)), len(deposits) + 1)

        self.log.info("The proposal activates on the restored chain")
        self.mine(ACTIVATION_PERIOD - 5)
        assert_equal(node.getsidechain(5)["proposalhash"], proposal["proposalhash"])
        assert_equal(self.check_in_sync()["activesidechains"], 2)

        # The sidechain database is part of what a full verification of the chain checks.
        assert node.verifychain(4, 0)


if __name__ == '__main__':
    DrivechainTest(__file__).main()

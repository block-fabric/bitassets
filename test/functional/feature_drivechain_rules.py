#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the edges of drivechains: the errors of the drivechain commands, the mempool rules of BMM
requests, the template fields pools read, and the mempool after a reorg.

feature_drivechain.py goes through the life of a sidechain; this test checks what it leaves out.
"""
from decimal import Decimal

from test_framework.address import address_to_scriptpubkey
from test_framework.blocktools import add_witness_commitment, create_block
from test_framework.messages import COIN, CTransaction, CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Regtest drivechain parameters, see CRegTestParams.
ACTIVATION_PERIOD = 20
WITHDRAWAL_MIN_SCORE = 30
SLOT = 1


class DrivechainRulesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0002"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, count=1):
        return self.generate(self.nodes[0], count)

    def activate(self, slot, title):
        node = self.nodes[0]
        node.createsidechainproposal(slot, title)
        hashes = self.mine(ACTIVATION_PERIOD)
        assert any(s["slot"] == slot for s in node.listactivesidechains())
        return hashes

    def bmm_data(self, slot, side_hash, prev):
        """The data of an M8 request (what follows OP_RETURN and its push)."""
        return "00bf00" + "%02x" % slot + bytes.fromhex(side_hash)[::-1].hex() + bytes.fromhex(prev)[::-1].hex()

    def run_test(self):
        self.mine(101)
        self.activate(SLOT, "Rules")
        self.nodes[0].createsidechaindeposit(SLOT, "dest", 10)
        self.mine()

        self.test_rpc_errors()
        self.test_mempool_bmm()
        self.test_template_fields()
        self.test_template_bmm_requests()
        self.test_reorg_evicts_deposit()
        self.test_reorg_evicts_withdrawal()

    def test_rpc_errors(self):
        self.log.info("The drivechain commands refuse what they cannot do, with a message that says why")
        node = self.nodes[0]
        assert_raises_rpc_error(-8, "hashid2 must be 40 hexadecimal characters", node.createsidechainproposal, 2, "x", "", "00" * 32, "zz")
        assert_raises_rpc_error(-8, "Invalid proposal", node.createsidechainproposal, 2, "x", "", "00" * 32, "00" * 20, 1000)
        assert_raises_rpc_error(-8, "slot out of range", node.acksidechain, "00" * 32, True, 512)
        assert_raises_rpc_error(-8, "No proposal with this hash is pending", node.acksidechain, "00" * 32)
        assert_raises_rpc_error(-22, "TX decode failed", node.receivewithdrawalbundle, SLOT, "00")
        blind = CTransaction()
        blind.vout = [CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])), CTxOut(COIN, address_to_scriptpubkey(node.getnewaddress()))]
        assert_raises_rpc_error(-8, "No active sidechain in this slot", node.receivewithdrawalbundle, 3, blind.serialize().hex())
        # A bundle with inputs is not in the blind form.
        with_input = node.createrawtransaction([{"txid": "11" * 32, "vout": 0}], [{"data": "00"}])
        assert_raises_rpc_error(-8, None, node.receivewithdrawalbundle, SLOT, with_input)
        not_sent = node.sendwithdrawalbundle(SLOT, "00" * 32)
        assert_equal(not_sent["sent"], False)
        assert "was not handed the bundle" in not_sent["reason"]
        assert_raises_rpc_error(-8, "vote must be upvote, downvote, abstain or default", node.setwithdrawalvote, SLOT, "yes")
        assert_raises_rpc_error(-8, "An upvote needs the hash of the bundle", node.setwithdrawalvote, SLOT, "upvote")
        node.setwithdrawalvote(SLOT, "abstain")
        node.setwithdrawalvote(SLOT, "default")
        assert_raises_rpc_error(-8, "count must not be negative", node.listsidechaindeposits, SLOT, None, -1)
        assert_raises_rpc_error(-8, "The height must not be negative", node.getsidechainevents, SLOT, -1)
        assert_raises_rpc_error(-8, "The count must be between 1 and 2000", node.getsidechainevents, SLOT, 1, 0)
        # Heights past the tip are left out, up to the largest one (first + count does not overflow).
        assert_equal(node.getsidechainevents(SLOT, 2**31 - 1, 2000), [])
        assert_raises_rpc_error(-8, "The number of blocks must be between 1 and 1000", node.getaveragefee, 0)
        assert node.getaveragefee() is not None
        # A block that is not in the active chain.
        stale = self.mine()[0]
        node.invalidateblock(stale)
        assert_raises_rpc_error(-5, "Block not found in the active chain", node.verifybmm, stale, SLOT, "00" * 32)
        node.reconsiderblock(stale)
        assert_raises_rpc_error(-8, "Invalid sidechain slot", node.createsidechaindeposit, 256, "dest", 1)
        assert_raises_rpc_error(-8, "The amount must be positive", node.createsidechaindeposit, SLOT, "dest", 0)
        assert_raises_rpc_error(-8, "Invalid sidechain slot", node.createbmmrequest, 256, "00" * 32, Decimal("0.001"))
        assert_raises_rpc_error(-8, "The amount must be positive", node.createbmmrequest, SLOT, "00" * 32, 0)
        assert_raises_rpc_error(-8, "Invalid number of threads", node.setgenerate, True, node.getnewaddress(), 1025)
        # -1 is every core of the machine.
        node.setgenerate(True, node.getnewaddress(), -1)
        info = node.getgenerate()
        assert_equal(info["threads"], info["cores"])
        node.setgenerate(False)
        assert_equal(node.getgenerate()["generate"], False)
        self.sync_all()

    def test_mempool_bmm(self):
        self.log.info("The mempool refuses BMM requests for no sidechain, and two requests of one sidechain in a transaction")
        node = self.nodes[0]
        tip = node.getbestblockhash()

        def send(datas):
            # createrawtransaction takes one data output at most: the outputs are put in by hand.
            tx = CTransaction()
            tx.vout = [CTxOut(0, CScript([OP_RETURN, bytes.fromhex(d)])) for d in datas]
            raw = node.fundrawtransaction(tx.serialize().hex(), {"changePosition": len(datas), "fee_rate": 2})["hex"]
            return node.signrawtransactionwithwallet(raw)["hex"]

        assert_raises_rpc_error(-26, "dc-bmm-inactive-sidechain", node.sendrawtransaction, send([self.bmm_data(3, "11" * 32, tip)]))
        assert_equal(node.getrawmempool(), [])
        # Only the first output of a transaction can be a request (BIP301): a second one is data.
        txid = node.sendrawtransaction(send([self.bmm_data(SLOT, "11" * 32, tip), self.bmm_data(SLOT, "22" * 32, tip)]))
        block = self.mine()[0]
        assert txid in node.getblock(block)["tx"]
        assert_equal(node.verifybmm(block, SLOT, "11" * 32)["verified"], True)
        assert_equal(node.verifybmm(block, SLOT, "22" * 32)["verified"], False)
        self.sync_all()

    def test_template_fields(self):
        self.log.info("Templates carry the coinbase outputs of the drivechain decisions, for pools")
        node = self.nodes[0]
        node.createsidechainproposal(2, "Template fields")
        template = node.getblocktemplate({"rules": ["segwit"]})
        assert "txweightlimit" in template
        assert_equal(template["drivechain_votable"], [0])
        outputs = template["drivechain_coinbase_outputs"]
        assert len(outputs) >= 1
        with_cb = node.getblocktemplate({"rules": ["segwit"], "capabilities": ["coinbasetxn"]})
        coinbase = node.decoderawtransaction(with_cb["coinbasetxn"]["data"])
        coinbase_scripts = [o["scriptPubKey"]["hex"] for o in coinbase["vout"]]
        for out in outputs:
            assert out in coinbase_scripts
        # What the node mines carries the same outputs.
        block = self.mine()[0]
        mined = node.getblock(block, 2)["tx"][0]["vout"]
        mined_scripts = [o["scriptPubKey"]["hex"] for o in mined]
        for out in outputs:
            assert out in mined_scripts
        self.sync_all()

    def test_template_bmm_requests(self):
        self.log.info("Only templates for mining software that puts the drivechain messages in its coinbase have BMM requests")
        node = self.nodes[0]
        request = node.createbmmrequest(SLOT, "33" * 32, Decimal("0.001"))["txid"]
        assert request in node.getrawmempool()

        def txids(template):
            return [tx["txid"] for tx in template["transactions"]]

        accept = "6a25d1617368" + "%02x" % SLOT + "33" * 32
        # Software that knows nothing of drivechains: no request, no accept it would have to add.
        plain = node.getblocktemplate({"rules": ["segwit"]})
        assert request not in txids(plain)
        assert accept not in plain["drivechain_coinbase_outputs"]
        # A block made from it by such software, with a coinbase of its own, is valid.
        block = create_block(tmpl=plain, txlist=[tx["data"] for tx in plain["transactions"]])
        block.vtx[0].vout[0].nValue = plain["coinbasevalue"]
        add_witness_commitment(block)
        block.solve()
        # (Not submitted: it would take the request's block. TestBlockValidity says it is valid.)
        assert_equal(node.getblocktemplate({"rules": ["segwit"], "mode": "proposal", "data": block.serialize().hex()}), None)
        # Software that takes the coinbase, or says it puts the messages in, gets the request with its accept.
        for request_params in ({"rules": ["segwit"], "capabilities": ["coinbasetxn"]},
                               {"rules": ["segwit"], "capabilities": ["drivechain"]},
                               {"rules": ["segwit", "drivechain"]}):
            template = node.getblocktemplate(request_params)
            assert request in txids(template)
            assert accept in template["drivechain_coinbase_outputs"]
        # The two kinds of template are kept apart: the plain one still has no request.
        assert request not in txids(node.getblocktemplate({"rules": ["segwit"]}))
        # The node's own blocks take it.
        block_hash = self.mine()[0]
        assert request in node.getblock(block_hash)["tx"]
        assert_equal(node.verifybmm(block_hash, SLOT, "33" * 32)["verified"], True)
        self.sync_all()

    def test_reorg_evicts_deposit(self):
        self.log.info("A deposit to a sidechain whose activation is undone leaves the mempool")
        node = self.nodes[0]
        hashes = self.activate(3, "Undone")
        node.createsidechaindeposit(3, "dest", 1)
        deposit = node.getrawmempool()
        assert_equal(len(deposit), 1)
        node.invalidateblock(hashes[-1])
        assert not any(s["slot"] == 3 for s in node.listactivesidechains())
        assert_equal(node.getrawmempool(), [])
        node.reconsiderblock(hashes[-1])
        assert any(s["slot"] == 3 for s in node.listactivesidechains())
        self.mine()
        self.sync_all()

    def test_reorg_evicts_withdrawal(self):
        self.log.info("A withdrawal whose bundle loses its score in a reorg leaves the mempool")
        node = self.nodes[0]
        payout = 2 * COIN
        fee = COIN // 100
        bundle = CTransaction()
        bundle.vin = []
        bundle.vout = [CTxOut(0, CScript([OP_RETURN, fee.to_bytes(8, "big")])),
                       CTxOut(payout, address_to_scriptpubkey(self.nodes[1].getnewaddress()))]
        bundle_hash = node.receivewithdrawalbundle(SLOT, bundle.serialize().hex())["hash"]
        self.mine()  # proposes it, score 1
        self.mine(WITHDRAWAL_MIN_SCORE - 1)
        status = node.getwithdrawalbundle(SLOT, bundle_hash)
        assert_equal(status["score"], WITHDRAWAL_MIN_SCORE)
        sent = node.sendwithdrawalbundle(SLOT, bundle_hash)
        assert_equal(sent["sent"], True)
        assert sent["txid"] in node.getrawmempool()
        # One block less, one vote less: the bundle is no longer payable.
        node.invalidateblock(node.getbestblockhash())
        assert_equal(node.getwithdrawalbundle(SLOT, bundle_hash)["score"], WITHDRAWAL_MIN_SCORE - 1)
        assert sent["txid"] not in node.getrawmempool()
        # Templates stay valid, and the new branch overtakes the old one on both nodes.
        # (The first block only ties with the old tip: no sync until the second.)
        self.generateblock(node, node.getnewaddress(), [], sync_fun=self.no_op)
        self.generateblock(node, node.getnewaddress(), [])
        self.sync_all()


if __name__ == '__main__':
    DrivechainRulesTest(__file__).main()

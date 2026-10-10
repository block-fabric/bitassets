#!/usr/bin/env python3
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test basic signet functionality"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create
from test_framework.util import assert_equal, assert_raises_rpc_error

SIGNET_DEFAULT_CHALLENGE = '5121022eb69a435e256daf541a170b29fdedbd5ef5f6efbd66f67cfec24402a2b1f35551ae'

# The first blocks of the default signet, signed with its key.
signet_blocks = [
    '00000020734036081fcda67929e8ce105ec996101de999ee3d88e7e9bb40f29fca010000569ab40fcce566f3fc50c612a5593cf4e8d0c4d9c5879653cac6ce1bd76c826f8ca7be6aae77031ed558300001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025100feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa2490047304402203fa2df99b024137c91dc0cc48d7a8d2189df5a7214873437d03f2435e57dbc25022041786fc8f45cafea514c344610d72c072b4a6493493f1f72f4fe151fe85f590601000120000000000000000000000000000000000000000000000000000000000000000000000000',
    '00000020f209454905d2bb346fa2f6ecb43561b5f54cba4ba822853cd5f95141a0010000037db5e8d0238d56191db169d11a03a1a38040a6dceb688be4b465e6903e0582c8a7be6aae77031ea0eb910001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025200feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022068a7cf444afcacae9e003a2c5e0206ae0d02632781d638d4d63af95f50ddf367022039b8cae510e81d91864bf12a8bd41c268adba815d8c03d325e5a6efeb2b8a6b301000120000000000000000000000000000000000000000000000000000000000000000001000000',
    '00000020c10f9e170987d358d680b74c2735cdf3e4eb85fe62944ea79f86168c770000001b3f0d795fb01b62a7173513a7dbff65cc50a7e8c192d487efdbc7c6ab550bb004a8be6aae77031e6832440001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025300feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022032beeb59d21b11d4e6af6942dcadd112c1702629b7a2c1e9802a8ce64819e9da02207d338ada50400f85e08be8364ad91f52cc9ccbedc2ea64ca10d9a51f048dedae01000120000000000000000000000000000000000000000000000000000000000000000002000000',
    '00000020ea674abf12e29cf07d738317afe4b02ff93cd55b98a6de8b67cfd77f06000000bc8e230b1f77745c87d8f79e18e7311f51886aec985e90b2af0d80460159711440a8be6aae77031e2b1c050001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025400feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa249004730440220268c98469f5ea248b05fe38b3980c1529d4162adc7d38cbe4da6fe977b08b5ed022015cedcda7186b5ee58fa4e162d654b06269a27d82ec6c33d94912145f8fd02ff01000120000000000000000000000000000000000000000000000000000000000000000003000000',
    '0000002090ffd189d5e175fee47beea43a8eda4078f21bab11f4cf1a2c732d862f030000586ba344760c75ea6f1a72e446c9335fab9eabecc71f165dceb0f1e6cf71a6267ca8be6aae77031e3a083b0001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025500feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa2490047304402207571ed0352d33270ef0676caf962db67bf6a6e5127ced59db913c9a92a17de19022022d30a3b7772531ccdde43da2ae395940de09717f09e5d4faa3505a8f1b2406d01000120000000000000000000000000000000000000000000000000000000000000000004000000',
    '000000207bf8f8f317f90eb7ce4f53f7530d7da54c5221cff39139d69f63c23a58030000555075e08ac8a62004d4485e2ec4797ef0074f883b91ed045ba28216a03206ecb8a8be6aae77031eaf4c0f0001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025600feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa249004730440220093c47911890472fdc1315a1f6fd9a1e3d77a3bf46cb566f8490fb78f9b096170220134bd987c2fccc80d2c6167cc1925e0e9a7117e640ebfde8e8e56fc058e2f4b001000120000000000000000000000000000000000000000000000000000000000000000005000000',
    '00000020a7c78c0b01ea4fa9c5ff28183ec4bf985ad65a1fcfc266bc1328cc03dd010000c19377eb781c05f3a387507206483a5742d98531c8ad975375f6d41d6a533ad9f4a8be6aae77031e76b6670001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025700feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022027b1e40f85cf7bc2bc566c4b66aa0d5dd9f0e1896f2e8d86da323c6b19742b6a02207879ec35c63de836e74238f0b12e588c0eaa5ceeb94092c24955e045288648cc01000120000000000000000000000000000000000000000000000000000000000000000006000000',
    '0000002056cc40fff0c24f9060263a6e6fe5299e728ca45358f75c420a5cb5c22b0000003ae6ee2b59cc7d00b70122dedea90ab0731bdb755e61d01a25f30af3469f094c30a9be6aae77031e55600b0001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025800feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022042326ea385f206693106fbd78bacb05040c5f66eda89a03933cc8567df22fc4502201b495519d20a6250b517eb656c53d866f18fc30221ed5a6eb2c805dcf367a68401000120000000000000000000000000000000000000000000000000000000000000000007000000',
    '000000209c6c63b9014152f87d20923c7ef11b496568b3ad1b7bf0e2b0c6d37607030000e471df983207bff823fcfee235a804a7b36ef86822921591a14ba11ed2d8655e6ca9be6aae77031e10ff8d0001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025900feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022045631dba2defde4f0ad627ef1f9e0e36c2ba63f5f7419489b0b1b8d08ed21f1f022067ef3e1714dab5b4865e3f47bd177a5e4c559c54e2691db23a2cf025f23b418a01000120000000000000000000000000000000000000000000000000000000000000000008000000',
    '000000207be3f405432de98849fe4ea4479e5612ae80ec834721683714ea8edd0603000005eaccb642adfc6dc029d94723239930698d0ac8400b982bf95f81cdc3479391a8a9be6aae77031e283b230001020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff025a00feffffff0200f2052a010000001600148305c5701c3fb2d3213fc5b9ed331641cc86015d0000000000000000776a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf94c4fecc7daa24900473044022062688ecf892d23ad844faf7ac902fc537644e5e06c2081fe4ccfae152f38229c022028e7f37c57981c5beb13d91c1d50fb9c9a7b26b0b250f5984a5b9e4058424e5b01000120000000000000000000000000000000000000000000000000000000000000000009000000',
]

class SignetParams:
    def __init__(self, challenge=None):
        # Prune to prevent disk space warning on CI systems with limited space,
        # when using networks other than regtest.
        if challenge is None:
            self.challenge = SIGNET_DEFAULT_CHALLENGE
            self.shared_args = ["-prune=550"]
        else:
            self.challenge = challenge
            self.shared_args = ["-prune=550", f"-signetchallenge={challenge}"]

class SignetBasicTest(BitcoinTestFramework):
    def set_test_params(self):
        self.chain = "signet"
        self.num_nodes = 6
        self.setup_clean_chain = True
        self.signets = [
            SignetParams(challenge='51'), # OP_TRUE
            SignetParams(), # default challenge
            # the key of the default challenge as a 2-of-2, which means it should fail
            SignetParams(challenge='5221022eb69a435e256daf541a170b29fdedbd5ef5f6efbd66f67cfec24402a2b1f35521022eb69a435e256daf541a170b29fdedbd5ef5f6efbd66f67cfec24402a2b1f35552ae')
        ]

        self.extra_args = [
            self.signets[0].shared_args, self.signets[0].shared_args,
            self.signets[1].shared_args, self.signets[1].shared_args,
            self.signets[2].shared_args, self.signets[2].shared_args,
        ]

    def setup_network(self):
        self.setup_nodes()

        # Setup the three signets, which are incompatible with each other
        self.connect_nodes(0, 1)
        self.connect_nodes(2, 3)
        self.connect_nodes(4, 5)

    def run_test(self):
        self.log.info("basic tests using OP_TRUE challenge")

        self.log.info('getblockchaininfo')
        def check_getblockchaininfo(node_idx, signet_idx):
            blockchain_info = self.nodes[node_idx].getblockchaininfo()
            assert_equal(blockchain_info['chain'], 'signet')
            assert_equal(blockchain_info['signet_challenge'], self.signets[signet_idx].challenge)
        check_getblockchaininfo(node_idx=1, signet_idx=0)
        check_getblockchaininfo(node_idx=2, signet_idx=1)
        check_getblockchaininfo(node_idx=5, signet_idx=2)

        self.log.info('getmininginfo')
        def check_getmininginfo(node_idx, signet_idx):
            mining_info = self.nodes[node_idx].getmininginfo()
            assert_equal(mining_info['blocks'], 0)
            assert_equal(mining_info['chain'], 'signet')
            assert 'currentblocktx' not in mining_info
            assert 'currentblockweight' not in mining_info
            assert_equal(mining_info['networkhashps'], Decimal('0'))
            assert_equal(mining_info['pooledtx'], 0)
            assert_equal(mining_info['signet_challenge'], self.signets[signet_idx].challenge)
        check_getmininginfo(node_idx=0, signet_idx=0)
        check_getmininginfo(node_idx=3, signet_idx=1)
        check_getmininginfo(node_idx=4, signet_idx=2)

        self.log.info("the CPU miner does not mine blocks that need a signature")
        address = self.nodes[0].deriveaddresses(descsum_create("wpkh(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)"))[0]
        assert_raises_rpc_error(-1, "Blocks of a signet need a signature; use contrib/signet/miner instead", self.nodes[0].setgenerate, True, address)
        assert_equal(self.nodes[0].getgenerate()["generate"], False)

        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        self.log.info("pregenerated signet blocks check")

        height = 0
        for block in signet_blocks:
            assert_equal(self.nodes[2].submitblock(block), None)
            height += 1
            assert_equal(self.nodes[2].getblockcount(), height)

        self.log.info("pregenerated signet blocks check (incompatible solution)")

        assert_equal(self.nodes[4].submitblock(signet_blocks[0]), 'bad-signet-blksig')

        self.log.info("test that signet logs the network magic on node start")
        with self.nodes[0].assert_debug_log(["Signet derived magic (message start)"]):
            self.restart_node(0)
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(extra_args=["-signetchallenge=abc"], expected_msg="Error: -signetchallenge must be hex, not 'abc'.")
        self.nodes[0].assert_start_raises_init_error(extra_args=["-signetchallenge=abc"] * 2, expected_msg="Error: -signetchallenge cannot be multiple values.")


if __name__ == '__main__':
    SignetBasicTest(__file__).main()

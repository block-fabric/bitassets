#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""When the drivechain database is rebuilt from the blocks.

- -reindex-chainstate checks the format of the database as well: one derived under other drivechain
  parameters is wiped before the blocks are connected again.
- A block file it is rebuilt from that is missing stops the node at startup, with an error that says
  how to recover.
- A pruned node that would have to rebuild it from blocks it no longer has refuses to start, and
  leaves the database as it was.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error


class DrivechainRebuildTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-prune=1", "-fastprune"]]

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 800)
        state = node.getdrivechaininfo()["statehash"]
        other = ["-testdrivechainparam=max_pending_bundles@63"]

        self.log.info("-reindex-chainstate wipes a database derived under other parameters before connecting the blocks")
        self.restart_node(0, extra_args=self.extra_args[0] + other)
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks"]):
            self.restart_node(0, extra_args=["-reindex-chainstate"])
        self.wait_until(lambda: node.getblockcount() == 800)
        # Marked as derived under the parameters it was built with: a restart uses it as it is.
        with node.assert_debug_log([], unexpected_msgs=["it is rebuilt from the blocks", "Bringing the sidechain database"]):
            self.restart_node(0)
        assert_equal(node.getdrivechaininfo()["statehash"], state)

        self.log.info("A block file the database is rebuilt from that cannot be read: the node does not start, and says how to recover")
        blocks_dir = node.blocks_path
        blk = blocks_dir / "blk00001.dat"
        assert (blocks_dir / "blk00002.dat").exists()
        self.stop_node(0)
        saved = blk.read_bytes()
        # (A missing file is caught earlier, when the block index is loaded: the file stays, its blocks go.)
        blk.write_bytes(bytes(len(saved)))
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks",
                                    "RollForwardSidechainDB: failed to read block"]):
            node.assert_start_raises_init_error(
                extra_args=self.extra_args[0] + other,
                expected_msg="Error loading the sidechain database: a block it has to be derived from could not be read \\(see the log\\). "
                             "A pruned node no longer has the blocks: rebuild with -reindex.\nPlease restart with -reindex or -reindex-chainstate to recover.",
                match=ErrorMatch.FULL_REGEX)
        # With the file back, it is rebuilt; under the former parameters again, it is the same.
        blk.write_bytes(saved)
        with node.assert_debug_log(["Bringing the sidechain database from height 0 to the chain tip at height 800"]):
            self.start_node(0, extra_args=self.extra_args[0] + other)
        assert_equal(node.getblockcount(), 800)
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks"]):
            self.restart_node(0)
        assert_equal(node.getdrivechaininfo()["statehash"], state)

        self.log.info("A pruned node does not wipe a database it cannot rebuild")
        pruned = node.pruneblockchain(400)
        assert pruned > 0
        assert "pruneheight" in node.getblockchaininfo()
        # What the pruned blocks said cannot be told any more.
        assert_raises_rpc_error(-1, "Block 1 is not available (pruned?)", node.getsidechainevents, 0, 1, 10)
        assert_equal(len(node.getsidechainevents(0, 790, 10)), 10)
        self.stop_node(0)
        node.assert_start_raises_init_error(
            extra_args=self.extra_args[0] + other,
            expected_msg="The sidechain database has to be rebuilt from the blocks \\(it is derived under other drivechain parameters\\), and this pruned node no longer has block",
            match=ErrorMatch.PARTIAL_REGEX)
        # Nothing was changed: under the former parameters the node starts from its database.
        with node.assert_debug_log([], unexpected_msgs=["it is rebuilt from the blocks", "Bringing the sidechain database"]):
            self.start_node(0)
        assert_equal(node.getdrivechaininfo()["statehash"], state)
        assert_equal(node.getblockcount(), 800)


if __name__ == "__main__":
    DrivechainRebuildTest(__file__).main()

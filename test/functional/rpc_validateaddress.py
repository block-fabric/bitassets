#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test validateaddress for main chain"""

from test_framework.test_framework import BitcoinTestFramework

from test_framework.util import assert_equal

INVALID_DATA = [
    # BIP 173
    (
        "tc1qw508d6qejxtdg4y5r3zarvary0c5xw7kg3g4ty",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid hrp
        [],
    ),
    ("chn1qw508d6qejxtdg4y5r3zarvary0c5xw7kvmysw5", "Invalid Bech32 checksum", [42]),
    (
        "CHN13W508D6QEJXTDG4Y5R3ZARVARY0C5XW7KNFZ0VF",
        "Version 1+ witness address must use Bech32m checksum",
        [],
    ),
    (
        "chn1rw5x8kmcq",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "chn10w508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kw52pwnj3",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "CHN1QR508D6QEJXTDG4Y5R3ZARVARYVXPHSFJ",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "tchn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sL5k7",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Mixed case
        [],
    ),
    (
        "CHN1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KVMYSWk",
        "Invalid character or mixed case",  # chn1, Mixed case, not in BIP 173 test vectors
        [42],
    ),
    (
        "chn1zw508d6qejxtdg4y5r3zarvaryvqvw7tsr",
        "Version 1+ witness address must use Bech32m checksum",  # Wrong padding
        [],
    ),
    (
        "tchn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3pqx3u7m",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("chn1kr4p7r", "Empty Bech32 data section", []),
    # BIP 350
    (
        "tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid human-readable part
        [],
    ),
    (
        "chn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqedvj8q",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "tchn1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq6l3rk7",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "CHN1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ6JX40J",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "chn1qw508d6qejxtdg4y5r3zarvary0c5xw7ke85ut5",
        "Version 0 witness address must use Bech32 checksum",  # Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        "tchn1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqc4g9qf",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        "chn1p38j9r5y49hruaue7wxjce0updqjuyyx0kh56v8s25huc6995vvpql3jow4",
        "Invalid Base 32 character",  # Invalid character in checksum
        [60],
    ),
    (
        "CHN130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQS9XUHW",
        "Invalid Bech32 address witness version",
        [],
    ),
    ("chn1pw5hl5sxm", "Invalid Bech32 address program size (1 byte)", []),
    (
        "chn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav25cu562j",
        "Invalid Bech32 address program size (41 bytes)",
        [],
    ),
    (
        "CHN1QR508D6QEJXTDG4Y5R3ZARVARYVXPHSFJ",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "tchn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq47Zagq",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Mixed case
        [],
    ),
    (
        "chn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07q0edymp",
        "Invalid padding in Bech32 data section",  # zero padding of more than 4 bits
        [],
    ),
    (
        "tchn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vp6gv4q9",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("chn1kr4p7r", "Empty Bech32 data section", []),
]
VALID_DATA = [
    # BIP 350
    (
        "CHN1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KVMYSWK",
        "0014751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    # (
    #   "tchn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qas9frf",
    #   "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    # ),
    (
        "chn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qklphuu",
        "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    ),
    (
        "chn1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7ksr6wde",
        "5128751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    ("CHN1SW50QSWZEG2", "6002751e"),
    ("chn1zw508d6qejxtdg4y5r3zarvaryv7qer5r", "5210751e76e8199196d454941c45d1b3a323"),
    # (
    #   "tchn1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses3xd8zn",
    #   "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "chn1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses6ffeax",
        "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    # (
    #   "tchn1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesm3dw60",
    #   "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "chn1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsess7fs96",
        "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    (
        "chn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqv3u7zz",
        "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    ),
    # PayToAnchor(P2A)
    (
        "chn1pfeesgqaa53",
        "51024e73",
    ),
]


class ValidateAddressMainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = ""  # main
        self.num_nodes = 1
        self.extra_args = [["-prune=899"]] * self.num_nodes

    def check_valid(self, addr, spk):
        info = self.nodes[0].validateaddress(addr)
        assert_equal(info["isvalid"], True)
        assert_equal(info["scriptPubKey"], spk)
        assert "error" not in info
        assert "error_locations" not in info

    def check_invalid(self, addr, error_str, error_locations):
        res = self.nodes[0].validateaddress(addr)
        assert_equal(res["isvalid"], False)
        assert_equal(res["error"], error_str)
        assert_equal(res["error_locations"], error_locations)

    def test_validateaddress(self):
        for (addr, error, locs) in INVALID_DATA:
            self.check_invalid(addr, error, locs)
        for (addr, spk) in VALID_DATA:
            self.check_valid(addr, spk)

    def run_test(self):
        self.test_validateaddress()


if __name__ == "__main__":
    ValidateAddressMainTest(__file__).main()

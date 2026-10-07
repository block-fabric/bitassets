// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The limits of Chains that regtest does not have: coinbase maturity of 360 blocks, and the block
// weight split between the coinbase (which carries the drivechain messages) and the rest.

#include <chainparams.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(chains_limits_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(coinbase_maturity_is_360)
{
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        const int maturity{params->GetConsensus().coinbase_maturity};
        BOOST_CHECK_EQUAL(maturity, 360);

        CCoinsViewCache view{&CoinsViewEmpty::Get()};
        const COutPoint coinbase_out{Txid::FromUint256(uint256{1}), 0};
        view.AddCoin(coinbase_out, Coin{CTxOut{50 * COIN, CScript() << OP_TRUE}, /*nHeightIn=*/100, /*fCoinBaseIn=*/true}, false);
        CMutableTransaction spend;
        spend.vin.emplace_back(coinbase_out);
        spend.vout.emplace_back(49 * COIN, CScript() << OP_TRUE);

        CAmount fee;
        TxValidationState early;
        BOOST_CHECK(!Consensus::CheckTxInputs(CTransaction{spend}, early, view, 100 + maturity - 1, fee, maturity));
        BOOST_CHECK_EQUAL(early.GetRejectReason(), "bad-txns-premature-spend-of-coinbase");
        TxValidationState mature;
        BOOST_CHECK(Consensus::CheckTxInputs(CTransaction{spend}, mature, view, 100 + maturity, fee, maturity));
    }
}

namespace {
/** A transaction of about `weight` weight units: one made-up input, one large OP_RETURN output. */
CTransactionRef BigTx(uint8_t salt, size_t weight)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{salt}), 0});
    CScript script{CScript() << OP_RETURN};
    const std::vector<unsigned char> chunk(500, salt);
    while (script.size() * WITNESS_SCALE_FACTOR < weight) script << chunk;
    tx.vout.emplace_back(0, script);
    return MakeTransactionRef(std::move(tx));
}

/** A block on the tip with these transactions after its coinbase, checked without its proof of work. */
BlockValidationState Check(ChainstateManager& chainman, const std::vector<CTransactionRef>& txs) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    CBlock block;
    block.nVersion = 0x20000000;
    block.hashPrevBlock = tip->GetBlockHash();
    block.nTime = tip->GetMedianTimePast() + 600;
    block.nBits = GetNextWorkRequired(tip, &block, chainman.GetConsensus());
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << (tip->nHeight + 1) << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    for (const auto& tx : txs) block.vtx.push_back(tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return TestBlockValidity(chainman.ActiveChainstate(), block, /*check_pow=*/false, /*check_merkle_root=*/true);
}
} // namespace

BOOST_AUTO_TEST_CASE(block_weight_split)
{
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    BOOST_CHECK_EQUAL(consensus.max_block_weight, 6'000'000);
    BOOST_CHECK_EQUAL(consensus.max_block_tx_weight, 4'000'000);
    LOCK(cs_main);
    // Transactions of 4.2 million weight units, in a block well under 6 million: refused, the rest
    // of the block being the coinbase's.
    const BlockValidationState over{Check(*m_node.chainman, {BigTx(1, 2'100'000), BigTx(2, 2'100'000)})};
    BOOST_CHECK_EQUAL(over.GetRejectReason(), "bad-blk-tx-weight");
    // Under 4 million the weight passes; the made-up inputs are what fails then.
    const BlockValidationState under{Check(*m_node.chainman, {BigTx(3, 1'900'000), BigTx(4, 1'900'000)})};
    BOOST_CHECK(under.GetRejectReason() != "bad-blk-tx-weight");
    BOOST_CHECK(under.GetRejectReason() != "bad-blk-weight");
    BOOST_CHECK(under.GetRejectReason() != "bad-blk-length");
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The assets under fuzzing, on the store of the sidechain state: blocks of fuzzed reservations,
// registrations, mints, burns, pools, swaps, auctions and releases. After every block: undone, the
// store is exactly what it was (through serialization of the undo data too); a transaction that breaks
// a rule changes nothing; every index is exactly what the tables imply; and every asset's supply is
// what its coins, pools and auctions hold.

#include <bitassets/state.h>
#include <primitives/transaction.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>

#include <cassert>
#include <set>
#include <string>
#include <vector>

using namespace bitassets;

namespace {
void initialize_bitassets()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
}

const CScript HOLDER{CScript() << OP_TRUE};

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in, uint32_t salt)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(salt)}), salt});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (const auto& token : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), token});
        tx.vout.emplace_back(0, HOLDER);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

std::set<sidechain::StoreBytes> KeysUnder(const sidechain::StoreView& view, uint8_t table)
{
    std::set<sidechain::StoreBytes> keys;
    view.ForEach(sidechain::StoreBytes{table}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        keys.insert(key);
        return true;
    });
    return keys;
}

sidechain::StoreBytes Key2(uint8_t table, const uint256& a, const uint256& b)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, a);
    sidechain::KeyCodec<uint256>::Encode(key, b);
    return key;
}

/** Every index, rebuilt from the tables, is the one in the store. */
void CheckIndexes(const sidechain::StoreView& view)
{
    std::set<sidechain::StoreBytes> by_id, pools_by_asset, auctions_by_asset, by_commitment;
    sidechain::Table<COutPoint, Token>{0x30}.ForEach(view, [&](const COutPoint& outpoint, const Token& token) {
        sidechain::StoreBytes key{0x39};
        sidechain::KeyCodec<uint256>::Encode(key, token.id);
        sidechain::KeyCodec<COutPoint>::Encode(key, outpoint);
        assert(view.Get(key) == sidechain::EncodeValue(static_cast<uint8_t>(token.kind)));
        by_id.insert(key);
        return true;
    });
    sidechain::Table<uint256, Pool>{0x34}.ForEach(view, [&](const uint256& id, const Pool& pool) {
        assert(id == PoolId(pool.asset0, pool.asset1));
        pools_by_asset.insert(Key2(0x3a, pool.asset0, id));
        pools_by_asset.insert(Key2(0x3a, pool.asset1, id));
        return true;
    });
    sidechain::Table<uint256, Auction>{0x35}.ForEach(view, [&](const uint256& id, const Auction& auction) {
        auctions_by_asset.insert(Key2(0x3b, auction.base, id));
        auctions_by_asset.insert(Key2(0x3b, auction.quote, id));
        return true;
    });
    const sidechain::Table<uint256, uint64_t> order{0x36};
    sidechain::Table<uint256, uint256>{0x32}.ForEach(view, [&](const uint256& id, const uint256& commitment) {
        sidechain::StoreBytes key{0x3c};
        sidechain::KeyCodec<uint256>::Encode(key, commitment);
        sidechain::KeyCodec<uint64_t>::Encode(key, order.Get(view, id).value_or(0));
        sidechain::KeyCodec<uint256>::Encode(key, id);
        by_commitment.insert(key);
        return true;
    });
    assert(KeysUnder(view, 0x39) == by_id);
    assert(KeysUnder(view, 0x3a) == pools_by_asset);
    assert(KeysUnder(view, 0x3b) == auctions_by_asset);
    assert(KeysUnder(view, 0x3c) == by_commitment);

    // Supply: coins, pool reserves, and what auctions still sell or have taken in.
    const State state{view};
    std::map<AssetId, uint64_t> held;
    state.ForEachToken([&](const COutPoint&, const Token& token) {
        if (token.kind == Token::Kind::ASSET) held[token.id] += token.amount;
        return true;
    });
    state.ForEachPool([&](const uint256&, const Pool& pool) {
        held[pool.asset0] += pool.reserve0;
        held[pool.asset1] += pool.reserve1;
        return true;
    });
    state.ForEachAuction([&](const Txid&, const Auction& auction) {
        if (!auction.closed) {
            held[auction.base] += auction.remaining;
            held[auction.quote] += auction.proceeds;
        }
        return true;
    });
    state.ForEachAsset([&](const AssetId& id, const AssetRecord& record) {
        assert(held[id] == record.supply);
        return true;
    });
}
} // namespace

FUZZ_TARGET(bitassets_state, .init = initialize_bitassets)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const int pool_rules_height{fdp.ConsumeIntegralInRange<int>(0, 10)};
    const int release_height{fdp.ConsumeIntegralInRange<int>(0, 10)};
    const int audit_height{fdp.ConsumeIntegralInRange<int>(0, 10)};
    const std::vector<AssetId> names{HashName("GOLD"), HashName("SILVER"), HashName("LEAD")};

    sidechain::EmptyStore empty;
    sidechain::StoreOverlay cache{empty, /*journal=*/false};
    uint32_t salt{0};

    for (int height{1}; height <= 30 && fdp.remaining_bytes() > 0; ++height) {
        const uint256 before{sidechain::StoreHash(cache)};
        sidechain::StoreOverlay block{cache, /*journal=*/true};
        State state{block};
        const int txs{fdp.ConsumeIntegralInRange<int>(0, 6)};
        for (int t{0}; t < txs; ++t) {
            std::vector<std::pair<COutPoint, Token>> carried;
            state.ForEachToken([&](const COutPoint& o, const Token& tok) { carried.emplace_back(o, tok); return true; });
            std::vector<COutPoint> inputs;
            std::vector<Token> spent;
            const int n_in{carried.empty() ? 0 : fdp.ConsumeIntegralInRange<int>(0, std::min<int>(3, carried.size()))};
            for (int i{0}; i < n_in; ++i) {
                const auto& [outpoint, token]{carried[fdp.ConsumeIntegralInRange<size_t>(0, carried.size() - 1)]};
                inputs.push_back(outpoint);
                spent.push_back(token);
            }
            const AssetId a{names[fdp.ConsumeIntegralInRange<size_t>(0, names.size() - 1)]};
            const AssetId b{names[fdp.ConsumeIntegralInRange<size_t>(0, names.size() - 1)]};
            const uint64_t amount{fdp.ConsumeIntegralInRange<uint64_t>(0, 5000)};
            const uint256 nonce{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(1, 2))};
            std::optional<Operation> op;
            // The outputs: tokens named outright, or none for a result of the operation.
            std::vector<std::optional<Token>> outs;
            switch (fdp.ConsumeIntegralInRange<int>(0, 12)) {
            case 0:
                op = Reserve{ReservationCommitment(a, nonce)};
                outs.push_back(Token{Token::Kind::RESERVATION, uint256{}, 1});
                break;
            case 1: {
                bitassets::Register reg;
                reg.name = a;
                reg.nonce = nonce;
                reg.supply = amount;
                op = reg;
                outs.push_back(Token{Token::Kind::CONTROL, a, 1});
                if (amount) outs.push_back(Token{Token::Kind::ASSET, a, amount});
                break;
            }
            case 2: op = Mint{a, amount}; outs.push_back(Token{Token::Kind::ASSET, a, amount}); break;
            case 3: op = Burn{spent}; break;
            case 4: op = Swap{a, amount, b, 0, HOLDER}; outs.push_back(std::nullopt); break;
            case 5: op = AddLiquidity{a, b, amount, fdp.ConsumeIntegralInRange<uint64_t>(0, 5000), 0}; outs.push_back(std::nullopt); break;
            case 6: op = RemoveLiquidity{a, b, amount, 0, 0, HOLDER}; outs.push_back(std::nullopt); outs.push_back(std::nullopt); break;
            case 7: {
                CreateAuction auction{a, amount, b, fdp.ConsumeIntegralInRange<uint64_t>(1, 100), fdp.ConsumeIntegralInRange<uint64_t>(0, 100), height, fdp.ConsumeIntegralInRange<int32_t>(1, 10)};
                op = auction;
                outs.push_back(std::nullopt);
                break;
            }
            case 8:
            case 9: {
                // At an auction there is.
                std::optional<Txid> id;
                state.ForEachAuction([&](const Txid& auction, const Auction&) { id = auction; return fdp.ConsumeBool(); });
                if (!id) break;
                if (fdp.ConsumeBool()) {
                    op = Bid{*id, amount, 0, HOLDER};
                    outs.push_back(std::nullopt);
                } else {
                    op = Collect{*id, HOLDER};
                    outs.push_back(std::nullopt);
                    outs.push_back(std::nullopt);
                }
                break;
            }
            case 10: op = ReleaseAsset{a}; break;
            default: break; // a transfer
            }
            // What was spent and is not used up goes on, whole or split in two.
            for (const Token& token : spent) {
                if (!fdp.ConsumeBool()) continue;
                if (Token::Divisible(token.kind) && token.amount > 1 && fdp.ConsumeBool()) {
                    const uint64_t part{fdp.ConsumeIntegralInRange<uint64_t>(1, token.amount - 1)};
                    outs.push_back(Token{token.kind, token.id, part});
                    outs.push_back(Token{token.kind, token.id, token.amount - part});
                } else {
                    outs.push_back(token);
                }
            }
            const CTransaction tx{MakeTx(inputs, op, outs, fdp.ConsumeIntegralInRange<CAmount>(0, 10000), ++salt)};
            const uint256 before_tx{sidechain::StoreHash(block)};
            std::vector<CTxOut> payouts;
            std::string reason;
            CAmount released{0};
            if (!state.ApplyTx(tx, height, payouts, reason, pool_rules_height, &released, release_height, audit_height)) {
                assert(!reason.empty());
                assert(sidechain::StoreHash(block) == before_tx);
            }
        }
        const sidechain::StoreUndo undo{block.TakeUndo()};
        block.MergeInto(cache);
        CheckIndexes(cache);
        DataStream stream{};
        stream << undo;
        sidechain::StoreUndo read;
        stream >> read;
        sidechain::StoreOverlay reverted{cache, /*journal=*/false};
        reverted.Revert(read);
        assert(sidechain::StoreHash(reverted) == before);
        CheckIndexes(reverted);
    }
}

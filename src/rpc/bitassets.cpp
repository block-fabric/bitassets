// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <coins.h>
#include <core_io.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>

#include <univalue.h>

#include <cmath>

using bitassets::AssetId;
using bitassets::AssetLabel;
using bitassets::AmountToJSON;
using node::NodeContext;

namespace {

bitassets::State StateOf(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    return chainman.ActiveChainstate().SideState().BitAssets();
}

AssetId ParseAsset(const UniValue& value, const bitassets::State& state)
{
    return bitassets::ParseAssetArg(value, [&](uint32_t seq) { return state.AssetOfSeq(seq); });
}

std::optional<bitassets::AssetRecord> RecordOf(const bitassets::State& state, const AssetId& asset)
{
    return state.GetAsset(asset);
}

std::string LabelOf(const bitassets::State& state, const AssetId& asset)
{
    const auto record{RecordOf(state, asset)};
    return bitassets::AssetLabel(asset, record ? &*record : nullptr);
}

uint8_t DecimalsOf(const bitassets::State& state, const AssetId& asset)
{
    if (asset.IsNull()) return bitassets::CHN_DECIMALS;
    const auto record{RecordOf(state, asset)};
    return record ? record->decimals : 0;
}

/** A registered asset, else an error. */
bitassets::AssetRecord Registered(const bitassets::State& state, const AssetId& asset)
{
    const auto record{RecordOf(state, asset)};
    if (!record) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such asset is registered");
    return *record;
}

/** An asset that pools and auctions trade: CHN, or a registered asset. */
void CheckTradable(const bitassets::State& state, const AssetId& asset)
{
    if (!asset.IsNull()) Registered(state, asset);
}

/** Where a token is: the output that carries it, and its address. */
UniValue OutputToJSON(Chainstate& chainstate, const COutPoint& outpoint) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", outpoint.hash.GetHex());
    obj.pushKV("vout", outpoint.n);
    if (const auto coin{chainstate.CoinsTip().GetCoin(outpoint)}) {
        CTxDestination dest;
        if (ExtractDestination(coin->out.scriptPubKey, dest)) obj.pushKV("address", EncodeDestination(dest));
    }
    return obj;
}

std::vector<RPCResult> AssetResults()
{
    return {
        {RPCResult::Type::STR_HEX, "asset", "The hash of its name, by which the chain knows it"},
        {RPCResult::Type::STR, "name", /*optional=*/true, "Its name, if its registration published it"},
        {RPCResult::Type::STR, "label", "How it is shown: its name, or \"0x\" and its hash"},
        {RPCResult::Type::STR, "seq", "Its number, in the order of registration"},
        {RPCResult::Type::NUM, "decimals", "The decimals it is shown with"},
        {RPCResult::Type::NUM, "supply", "How much there is: all minted, less all burned"},
        {RPCResult::Type::NUM, "minted", "All ever minted, the initial supply included"},
        {RPCResult::Type::NUM, "burned", "All ever burned"},
        {RPCResult::Type::BOOL, "fixed", "Whether the control coin was burned: nothing more can be minted, the data cannot change"},
        {RPCResult::Type::NUM, "registered", "The height of the block that registered it"},
        {RPCResult::Type::STR_HEX, "registration", "The transaction that registered it"},
        {RPCResult::Type::OBJ, "data", /*optional=*/true, "Its data; left out at a height before its registration", bitassets::DataResults()},
        {RPCResult::Type::BOOL, "releasable", "Whether it is dead and can be retired (releaseasset): fixed supply, every unit in pools nobody provides for"},
        {RPCResult::Type::NUM, "release_fee", /*optional=*/true, "If releasable: the CHN its pools hold, which retiring it pays to mainchain miners"},
        {RPCResult::Type::OBJ, "control", /*optional=*/true, "The output that carries its control coin, unless burned",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM, "vout", ""},
            {RPCResult::Type::STR, "address", /*optional=*/true, "Who controls it"},
        }},
    };
}

UniValue AssetToJSON(Chainstate& chainstate, const bitassets::State& state, const AssetId& asset, const bitassets::AssetRecord& record, std::optional<int> height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("asset", asset.GetHex());
    if (!record.text.empty()) obj.pushKV("name", record.text);
    obj.pushKV("label", AssetLabel(asset, &record));
    obj.pushKV("seq", bitassets::FormatSeq(record.seq));
    obj.pushKV("decimals", record.decimals);
    obj.pushKV("supply", AmountToJSON(record.supply, record.decimals));
    obj.pushKV("minted", AmountToJSON(record.minted, record.decimals));
    obj.pushKV("burned", AmountToJSON(record.burned, record.decimals));
    obj.pushKV("fixed", record.fixed);
    obj.pushKV("registered", record.height);
    obj.pushKV("registration", record.registration.GetHex());
    const auto data{height ? state.DataAt(asset, *height) : std::optional{record.Current()}};
    if (data) obj.pushKV("data", bitassets::DataToJSON(*data));
    const bool releasable{state.Releasable(asset)};
    obj.pushKV("releasable", releasable);
    if (releasable) {
        CAmount fee{0};
        for (const auto& [id, pool] : state.PoolsOf(asset)) {
            if (pool.asset0.IsNull() && pool.asset1 == asset) fee += static_cast<CAmount>(pool.reserve0);
        }
        obj.pushKV("release_fee", ValueFromAmount(fee));
    }
    if (!record.fixed) {
        if (const auto outpoint{state.FirstTokenOf(asset, bitassets::Token::Kind::CONTROL)}) obj.pushKV("control", OutputToJSON(chainstate, *outpoint));
    }
    return obj;
}

/** A price: how much of `quote` one whole `base` is worth, from amounts in units. */
UniValue PriceToJSON(uint64_t base_units, uint8_t base_decimals, uint64_t quote_units, uint8_t quote_decimals)
{
    if (base_units == 0) return UniValue{};
    // quote per base, in whole assets, with 12 significant decimals.
    const double price{(static_cast<double>(quote_units) / std::pow(10.0, quote_decimals)) / (static_cast<double>(base_units) / std::pow(10.0, base_decimals))};
    UniValue value;
    // Plain decimals, with 12 significant digits however small (no exponent).
    const int decimals{price >= 1 ? 8 : std::min(30, 12 - static_cast<int>(std::floor(std::log10(price))))};
    std::string text{strprintf("%.*f", decimals, price)};
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.') text.pop_back();
    }
    value.setNumStr(text);
    return value;
}

std::vector<RPCResult> PoolResults()
{
    return {
        {RPCResult::Type::STR_HEX, "pool", "The id of the pool, which its shares carry"},
        {RPCResult::Type::STR, "asset_a", "The first asset, as asked for (else the lower)"},
        {RPCResult::Type::STR, "asset_b", "The second asset"},
        {RPCResult::Type::STR_HEX, "asset_a_id", "Its hash (null for CHN)"},
        {RPCResult::Type::STR_HEX, "asset_b_id", "Its hash"},
        {RPCResult::Type::NUM, "reserve_a", "What the pool holds of the first asset"},
        {RPCResult::Type::NUM, "reserve_b", "What it holds of the second"},
        {RPCResult::Type::NUM, "price", /*optional=*/true, "What one of the first asset is worth in the second"},
        {RPCResult::Type::NUM, "shares", "The shares there are, in units (a share has no decimals)"},
        {RPCResult::Type::NUM, "volume_a", "All of the first asset ever traded in"},
        {RPCResult::Type::NUM, "volume_b", "All of the second asset ever traded in"},
        {RPCResult::Type::NUM, "swaps", "How many trades it has made"},
        {RPCResult::Type::NUM, "created", "The height of the block that made it"},
        {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity any more: it holds only the minimum it keeps for good, and trading in it gets almost nothing"},
    };
}

UniValue PoolToJSON(const bitassets::State& state, const uint256& id, const bitassets::Pool& pool, const AssetId& first)
{
    const bool flip{pool.asset0 != first};
    const AssetId& a{flip ? pool.asset1 : pool.asset0};
    const AssetId& b{flip ? pool.asset0 : pool.asset1};
    const uint64_t reserve_a{flip ? pool.reserve1 : pool.reserve0}, reserve_b{flip ? pool.reserve0 : pool.reserve1};
    const uint64_t volume_a{flip ? pool.volume1 : pool.volume0}, volume_b{flip ? pool.volume0 : pool.volume1};
    const uint8_t da{DecimalsOf(state, a)}, db{DecimalsOf(state, b)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("pool", id.GetHex());
    obj.pushKV("asset_a", LabelOf(state, a));
    obj.pushKV("asset_b", LabelOf(state, b));
    obj.pushKV("asset_a_id", a.GetHex());
    obj.pushKV("asset_b_id", b.GetHex());
    obj.pushKV("reserve_a", AmountToJSON(reserve_a, da));
    obj.pushKV("reserve_b", AmountToJSON(reserve_b, db));
    if (UniValue price{PriceToJSON(reserve_a, da, reserve_b, db)}; !price.isNull()) obj.pushKV("price", std::move(price));
    obj.pushKV("shares", pool.shares);
    obj.pushKV("volume_a", AmountToJSON(volume_a, da));
    obj.pushKV("volume_b", AmountToJSON(volume_b, db));
    obj.pushKV("swaps", pool.swaps);
    obj.pushKV("created", pool.height);
    obj.pushKV("abandoned", bitassets::amm::Abandoned(pool));
    return obj;
}

std::vector<RPCResult> AuctionResults()
{
    return {
        {RPCResult::Type::STR_HEX, "auction", "The transaction that made it, which names it"},
        {RPCResult::Type::STR, "base", "What it sells"},
        {RPCResult::Type::STR, "quote", "What it sells for"},
        {RPCResult::Type::STR_HEX, "base_id", "The hash of what it sells (null for CHN)"},
        {RPCResult::Type::STR_HEX, "quote_id", "The hash of what it sells for"},
        {RPCResult::Type::NUM, "amount", "How much it sells, in all"},
        {RPCResult::Type::NUM, "remaining", "How much is left"},
        {RPCResult::Type::NUM, "proceeds", "What bids paid in, not collected yet"},
        {RPCResult::Type::NUM, "start_price", "The price of all of it at the start"},
        {RPCResult::Type::NUM, "end_price", "The price of all of it at the end"},
        {RPCResult::Type::NUM, "unit_price", "What one whole unit of it costs in the next block"},
        {RPCResult::Type::NUM, "price", "What all of it costs in the next block"},
        {RPCResult::Type::NUM, "start_height", "The first block that takes bids"},
        {RPCResult::Type::NUM, "end_height", "The last block that takes bids"},
        {RPCResult::Type::NUM, "bids", "How many bids it took"},
        {RPCResult::Type::STR, "status", "\"upcoming\", \"open\", \"sold out\", \"ended\" or \"closed\""},
        {RPCResult::Type::OBJ, "receipt", /*optional=*/true, "The output that carries its receipt, until it is collected",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM, "vout", ""},
            {RPCResult::Type::STR, "address", /*optional=*/true, "Who collects"},
        }},
    };
}

std::string AuctionStatus(const bitassets::Auction& auction, int height)
{
    if (auction.closed) return "closed";
    if (auction.remaining == 0) return "sold out";
    if (height < auction.start_height) return "upcoming";
    if (height > auction.EndHeight()) return "ended";
    return "open";
}

UniValue AuctionToJSON(Chainstate& chainstate, const bitassets::State& state, const Txid& id, const bitassets::Auction& auction, int height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const uint8_t db{DecimalsOf(state, auction.base)}, dq{DecimalsOf(state, auction.quote)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("auction", id.GetHex());
    obj.pushKV("base", LabelOf(state, auction.base));
    obj.pushKV("quote", LabelOf(state, auction.quote));
    obj.pushKV("base_id", auction.base.GetHex());
    obj.pushKV("quote_id", auction.quote.GetHex());
    obj.pushKV("amount", AmountToJSON(auction.base_amount, db));
    obj.pushKV("remaining", AmountToJSON(auction.remaining, db));
    obj.pushKV("proceeds", AmountToJSON(auction.proceeds, dq));
    obj.pushKV("start_price", AmountToJSON(auction.start_price, dq));
    obj.pushKV("end_price", AmountToJSON(auction.end_price, dq));
    const uint64_t price{auction.PriceAt(std::max(height, auction.start_height))};
    obj.pushKV("unit_price", PriceToJSON(auction.base_amount, db, price, dq));
    obj.pushKV("price", AmountToJSON(price, dq));
    obj.pushKV("start_height", auction.start_height);
    obj.pushKV("end_height", auction.EndHeight());
    obj.pushKV("bids", auction.bids);
    obj.pushKV("status", AuctionStatus(auction, height));
    if (!auction.closed) {
        if (const auto outpoint{state.FirstTokenOf(id.ToUint256(), bitassets::Token::Kind::RECEIPT)}) obj.pushKV("receipt", OutputToJSON(chainstate, *outpoint));
    }
    return obj;
}

RPCMethod getasset()
{
    return RPCMethod{
        "getasset",
        "A registered asset: its supply, its data, and who controls it.",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Its data as it was at the end of the block at this height"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", AssetResults()},
        RPCExamples{HelpExampleCli("getasset", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId asset{ParseAsset(request.params[0], state)};
    if (asset.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is the coin of the chain, not a registered asset");
    const auto& record{Registered(state, asset)};
    const std::optional<int> height{request.params[1].isNull() ? std::nullopt : std::optional<int>{request.params[1].getInt<int>()}};
    return AssetToJSON(chainman.ActiveChainstate(), state, asset, record, height);
},
    };
}

RPCMethod listassets()
{
    return RPCMethod{
        "listassets",
        "The registered assets, in the order of registration.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
            {"match", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only those whose public name contains this text (any case)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", AssetResults()}}},
        RPCExamples{HelpExampleCli("listassets", "10 0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int count{request.params[0].isNull() ? 100 : request.params[0].getInt<int>()};
    const int skip{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count or skip");
    const std::optional<std::string> match{request.params[2].isNull() ? std::nullopt : std::optional{ToLower(request.params[2].get_str())}};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    UniValue result(UniValue::VARR);
    int skipped{0};
    for (uint32_t seq{0}; seq < state.NextSeq() && result.size() < static_cast<size_t>(count); ++seq) {
        const auto asset{state.AssetOfSeq(seq)};
        if (!asset) continue;
        // A retired asset keeps its number, and its name may be registered again under another.
        const auto found{state.GetAsset(*asset)};
        if (!found || found->seq != seq) continue;
        const auto& record{*found};
        if (match && ToLower(record.text).find(*match) == std::string::npos) continue;
        if (skipped++ < skip) continue;
        result.push_back(AssetToJSON(chainman.ActiveChainstate(), state, *asset, record, std::nullopt));
    }
    return result;
},
    };
}

RPCMethod getassethistory()
{
    return RPCMethod{
        "getassethistory",
        "Every value each field of the data of an asset has had, with the transaction and height that set it.",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
        },
        RPCResult{RPCResult::Type::OBJ_DYN, "", "Per field, oldest first",
        {
            {RPCResult::Type::ARR, "field", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::ANY, "value", "The value; null where it was deleted"},
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::NUM, "height", ""},
            }}}},
        }},
        RPCExamples{HelpExampleCli("getassethistory", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId asset{ParseAsset(request.params[0], state)};
    Registered(state, asset);
    const bitassets::AssetHistory record{state.GetHistory(asset)};
    UniValue result(UniValue::VOBJ);
    const auto add{[&](const std::string& field, const auto& history, const auto& format) {
        UniValue entries(UniValue::VARR);
        for (const auto& entry : history) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("value", entry.value ? format(*entry.value) : UniValue{});
            obj.pushKV("txid", entry.txid.GetHex());
            obj.pushKV("height", entry.height);
            entries.push_back(std::move(obj));
        }
        result.pushKV(field, std::move(entries));
    }};
    const auto data_field{[](const char* key) {
        return [key](const auto& v) {
            bitassets::AssetData data;
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) data.info = v;
            if constexpr (std::is_same_v<T, uint256>) data.commitment = v;
            if constexpr (std::is_same_v<T, bitassets::SocketV4>) data.ipv4 = v;
            if constexpr (std::is_same_v<T, bitassets::SocketV6>) data.ipv6 = v;
            if constexpr (std::is_same_v<T, bitassets::EncryptionKey>) data.encryption_key = v;
            return bitassets::DataToJSON(data)[key];
        };
    }};
    add("info", record.info, data_field("info"));
    add("commitment", record.commitment, data_field("commitment"));
    add("ipv4", record.ipv4, data_field("ipv4"));
    add("ipv6", record.ipv6, data_field("ipv6"));
    add("encryptionkey", record.encryption_key, data_field("encryptionkey"));
    add("signingkey", record.signing_key, [](const bitassets::SigningKey& v) { return UniValue{HexStr(v)}; });
    return result;
},
    };
}

RPCMethod getbitassetsinfo()
{
    return RPCMethod{
        "getbitassetsinfo",
        "How many assets, reservations, pools and auctions there are.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "assets", "Registered assets"},
            {RPCResult::Type::NUM, "reservations", "Reservations not registered yet"},
            {RPCResult::Type::NUM, "pools", "Pools"},
            {RPCResult::Type::NUM, "auctions", "Auctions not closed"},
            {RPCResult::Type::NUM, "height", "The height of the next block, which the prices are for"},
        }},
        RPCExamples{HelpExampleCli("getbitassetsinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    UniValue result(UniValue::VOBJ);
    // Counted: for information.
    size_t assets{0}, reservations{0}, open_auctions{0};
    state.ForEachAsset([&](const AssetId&, const bitassets::AssetRecord&) { ++assets; return true; });
    state.ForEachReservation([&](const Txid&, const uint256&) { ++reservations; return true; });
    state.ForEachAuction([&](const Txid&, const bitassets::Auction& a) { if (!a.closed) ++open_auctions; return true; });
    result.pushKV("assets", assets);
    result.pushKV("reservations", reservations);
    result.pushKV("pools", state.PoolCount());
    result.pushKV("auctions", open_auctions);
    result.pushKV("height", chainman.ActiveChain().Height() + 1);
    return result;
},
    };
}

RPCMethod listpools()
{
    return RPCMethod{
        "listpools",
        "The pools, with what they hold and their prices.",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only the pools of this asset, which then comes first"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", PoolResults()}}},
        RPCExamples{HelpExampleCli("listpools", "") + HelpExampleCli("listpools", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const std::optional<AssetId> only{request.params[0].isNull() ? std::nullopt : std::optional{ParseAsset(request.params[0], state)}};
    UniValue result(UniValue::VARR);
    std::vector<std::pair<uint256, bitassets::Pool>> pools;
    if (only) {
        pools = state.PoolsOf(*only);
    } else {
        state.ForEachPool([&](const uint256& id, const bitassets::Pool& pool) { pools.emplace_back(id, pool); return true; });
    }
    for (const auto& [id, pool] : pools) {
        // A pool with CHN shows its price in CHN.
        AssetId first{pool.asset0.IsNull() ? pool.asset1 : pool.asset0};
        if (only) first = *only;
        result.push_back(PoolToJSON(state, id, pool, first));
    }
    return result;
},
    };
}

RPCMethod getpool()
{
    return RPCMethod{
        "getpool",
        "The pool of two assets.",
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", PoolResults()},
        RPCExamples{HelpExampleCli("getpool", "\"GOLD\" \"CHN\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId a{ParseAsset(request.params[0], state)}, b{ParseAsset(request.params[1], state)};
    const auto pool{state.FindPool(a, b)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets (addliquidity makes one)");
    return PoolToJSON(state, bitassets::PoolId(a, b), *pool, a);
},
    };
}

RPCMethod quoteswap()
{
    return RPCMethod{
        "quoteswap",
        "What a trade in a pool gives now: for an amount in, what comes out; or for an amount out, what has to go in.",
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + std::string{bitassets::ASSET_ARG_HELP}},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much goes in (or comes out, with exact_out)"},
            {"asset_out", RPCArg::Type::STR, RPCArg::Optional::NO, "What comes out"},
            {"exact_out", RPCArg::Type::BOOL, RPCArg::Default{false}, "The amount is what comes out"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "amount_in", "What goes in"},
            {RPCResult::Type::NUM, "amount_out", "What comes out"},
            {RPCResult::Type::NUM, "price", "What one of what comes out costs, in what goes in, for this trade"},
            {RPCResult::Type::NUM, "spot_price", "The same before the trade"},
            {RPCResult::Type::NUM, "price_impact", "How much worse the trade's price is than the spot price, in percent (the fee included)"},
            {RPCResult::Type::NUM, "fee", "What the pool keeps, in what goes in"},
            {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity to the pool any more (see listpools)"},
        }},
        RPCExamples{HelpExampleCli("quoteswap", "\"CHN\" 1 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId in{ParseAsset(request.params[0], state)}, out{ParseAsset(request.params[2], state)};
    if (in == out) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
    const auto pool{state.FindPool(in, out)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets");
    const bool zero_in{pool->asset0 == in};
    const uint64_t reserve_in{zero_in ? pool->reserve0 : pool->reserve1}, reserve_out{zero_in ? pool->reserve1 : pool->reserve0};
    const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
    const bool exact_out{!request.params[3].isNull() && request.params[3].get_bool()};
    uint64_t amount_in, amount_out;
    if (exact_out) {
        amount_out = bitassets::ParseUnits(request.params[1], dout);
        const auto needed{bitassets::amm::SwapIn(reserve_in, reserve_out, amount_out)};
        if (!needed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The pool does not hold that much");
        amount_in = *needed;
        amount_out = bitassets::amm::SwapOut(reserve_in, reserve_out, amount_in);
    } else {
        amount_in = bitassets::ParseUnits(request.params[1], di);
        amount_out = bitassets::amm::SwapOut(reserve_in, reserve_out, amount_in);
        if (amount_out == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it buys nothing");
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("amount_in", AmountToJSON(amount_in, di));
    result.pushKV("amount_out", AmountToJSON(amount_out, dout));
    result.pushKV("price", PriceToJSON(amount_out, dout, amount_in, di));
    result.pushKV("spot_price", PriceToJSON(reserve_out, dout, reserve_in, di));
    UniValue impact_value;
    impact_value.setNumStr(strprintf("%.4f", bitassets::amm::PriceImpact(reserve_in, reserve_out, amount_in, amount_out)));
    result.pushKV("price_impact", impact_value);
    result.pushKV("fee", AmountToJSON(amount_in * bitassets::SWAP_FEE_PER_MILLE / 1000, di));
    result.pushKV("abandoned", bitassets::amm::Abandoned(*pool));
    return result;
},
    };
}

RPCMethod listauctions()
{
    return RPCMethod{
        "listauctions",
        "The auctions, newest first.",
        {
            {"include_closed", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also those collected already"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", AuctionResults()}}},
        RPCExamples{HelpExampleCli("listauctions", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const bool closed{!request.params[0].isNull() && request.params[0].get_bool()};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const int height{chainman.ActiveChain().Height() + 1};
    std::vector<std::pair<Txid, bitassets::Auction>> auctions;
    state.ForEachAuction([&](const Txid& id, const bitassets::Auction& auction) {
        if (!auction.closed || closed) auctions.emplace_back(id, auction);
        return true;
    });
    std::stable_sort(auctions.begin(), auctions.end(), [](const auto& x, const auto& y) { return x.second.created > y.second.created; });
    UniValue result(UniValue::VARR);
    for (const auto& [id, auction] : auctions) result.push_back(AuctionToJSON(chainman.ActiveChainstate(), state, id, auction, height));
    return result;
},
    };
}

RPCMethod getauction()
{
    return RPCMethod{
        "getauction",
        "An auction.",
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", AuctionResults()},
        RPCExamples{HelpExampleCli("getauction", "\"txid\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto found{state.GetAuction(id)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    return AuctionToJSON(chainman.ActiveChainstate(), state, id, *found, chainman.ActiveChain().Height() + 1);
},
    };
}

RPCMethod quotebid()
{
    return RPCMethod{
        "quotebid",
        "What a bid on an auction buys in the next block.",
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "What the bid pays, in what the auction sells for"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "buys", "What it buys"},
            {RPCResult::Type::NUM, "remaining", "What is left to buy"},
            {RPCResult::Type::NUM, "cost_of_remaining", "What a bid for all that is left pays in the next block"},
            {RPCResult::Type::BOOL, "open", "Whether the next block takes bids"},
        }},
        RPCExamples{HelpExampleCli("quotebid", "\"txid\" 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto found{state.GetAuction(id)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    const bitassets::Auction auction{*found};
    const int height{chainman.ActiveChain().Height() + 1};
    const uint8_t db{DecimalsOf(state, auction.base)}, dq{DecimalsOf(state, auction.quote)};
    const uint64_t amount{bitassets::ParseUnits(request.params[1], dq)};
    const int at{std::max(height, auction.start_height)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("buys", AmountToJSON(std::min(auction.BuysAt(at, amount), auction.remaining), db));
    result.pushKV("remaining", AmountToJSON(auction.remaining, db));
    // Ceil(remaining * price / base_amount), which buys all of it.
    const unsigned __int128 cost{(static_cast<unsigned __int128>(auction.remaining) * auction.PriceAt(at) + auction.base_amount - 1) / auction.base_amount};
    result.pushKV("cost_of_remaining", AmountToJSON(static_cast<uint64_t>(std::min<unsigned __int128>(cost, bitassets::MAX_AMOUNT)), dq));
    result.pushKV("open", auction.OpenAt(height));
    return result;
},
    };
}

} // namespace

void RegisterBitAssetsRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"bitassets", &getasset},
        {"bitassets", &listassets},
        {"bitassets", &getassethistory},
        {"bitassets", &getbitassetsinfo},
        {"bitassets", &listpools},
        {"bitassets", &getpool},
        {"bitassets", &quoteswap},
        {"bitassets", &listauctions},
        {"bitassets", &getauction},
        {"bitassets", &quotebid},
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}

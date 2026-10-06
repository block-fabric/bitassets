// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <addresstype.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <core_io.h>
#include <hash.h>
#include <key_io.h>
#include <rpc/util.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/receive.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <cmath>

namespace wallet {
namespace {

using bitassets::AssetId;
using bitassets::Marker;
using bitassets::Token;

/** The label of the output of a reservation, so that the wallet knows the name it reserves. */
std::string ReservationLabel(const std::string& name) { return "bitasset:" + name; }

std::shared_ptr<CWallet> ReadyWallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return nullptr;
    pwallet->BlockUntilSyncedToCurrentChain();
    EnsureWalletIsUnlocked(*pwallet);
    return pwallet;
}

std::optional<std::string> LabelOf(const CWallet& wallet, const CTxDestination& dest)
{
    LOCK(wallet.cs_wallet);
    const CAddressBookData* entry{wallet.FindAddressBookEntry(dest)};
    if (!entry || !entry->label) return std::nullopt;
    return *entry->label;
}

CTxDestination NewDestination(CWallet& wallet, const std::string& label = "")
{
    const auto dest{wallet.GetNewDestination(OutputType::BECH32, label)};
    if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
    return *dest;
}

/** Where tokens the wallet keeps go: an address of its own, not shown as one to be paid at. */
CTxDestination ChangeDestination(CWallet& wallet)
{
    const auto dest{wallet.GetNewChangeDestination(OutputType::BECH32)};
    if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
    return *dest;
}

/** The nonce of a reservation: a signature by its key, of the name, hashed. Only this wallet can make it. */
uint256 ReservationNonce(const CWallet& wallet, const CTxDestination& dest, const AssetId& name)
{
    const auto* keyhash{std::get_if<WitnessV0KeyHash>(&dest)};
    if (!keyhash) throw JSONRPCError(RPC_WALLET_ERROR, "A reservation of this wallet is carried by an address of a single key");
    std::string signature;
    const SigningResult result{wallet.SignMessage(strprintf("BitAssets reservation %s", name.GetHex()), PKHash{ToKeyID(*keyhash)}, signature)};
    if (result != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Cannot make the nonce of the reservation: %s", SigningResultString(result)));
    return Hash(*Assert(DecodeBase64(signature)));
}

struct OwnedToken {
    COutPoint outpoint;
    CTxDestination dest;
    Token token;
};

/** The outputs of the wallet, in blocks and not spent, that carry tokens. */
std::vector<OwnedToken> OwnedTokens(CWallet& wallet)
{
    std::vector<OwnedToken> owned;
    LOCK(wallet.cs_wallet);
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        const CTransactionRef wtx_tx{wtx.GetTx()};
        for (uint32_t n{0}; n < wtx_tx->vout.size(); ++n) {
            const CTxOut& out{wtx_tx->vout[n]};
            if (out.nValue != 0 || !wallet.IsMine(out)) continue;
            const COutPoint outpoint{txid, n};
            if (wallet.IsSpent(outpoint)) continue;
            const auto token{wallet.chain().getBitAssetsToken(outpoint)};
            if (!token) continue;
            CTxDestination dest;
            ExtractDestination(out.scriptPubKey, dest);
            owned.push_back({outpoint, dest, *token});
        }
    }
    return owned;
}

/** Whether a token of the wallet is being spent by a transaction not in a block yet. */
bool Pending(CWallet& wallet, Token::Kind kind, const uint256& id)
{
    LOCK(wallet.cs_wallet);
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        if (wallet.GetTxDepthInMainChain(wtx) != 0 || wtx.isAbandoned()) continue;
        for (const CTxIn& in : wtx.GetTx()->vin) {
            const auto token{wallet.chain().getBitAssetsToken(in.prevout)};
            if (token && token->kind == kind && token->id == id) return true;
        }
    }
    return false;
}

[[noreturn]] void NotHeld(CWallet& wallet, Token::Kind kind, const uint256& id, const std::string& what)
{
    if (Pending(wallet, kind, id)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("The last change to %s is waiting for the next block: try again once it is in", what));
    }
    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("This wallet does not hold %s", what));
}

/** Tokens of one kind and id, enough for `amount`: the outputs, and what they carry in all. */
std::pair<std::vector<COutPoint>, uint64_t> Select(CWallet& wallet, Token::Kind kind, const uint256& id, uint64_t amount, const std::string& what)
{
    std::vector<OwnedToken> candidates;
    for (OwnedToken& t : OwnedTokens(wallet)) {
        if (t.token.kind == kind && t.token.id == id) candidates.push_back(std::move(t));
    }
    // The largest first: the fewest inputs.
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.token.amount > b.token.amount; });
    std::vector<COutPoint> inputs;
    uint64_t total{0};
    for (const OwnedToken& t : candidates) {
        if (total >= amount) break;
        inputs.push_back(t.outpoint);
        total += t.token.amount;
    }
    if (total < amount) {
        if (total == 0) NotHeld(wallet, kind, id, what);
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("This wallet holds too little of %s in blocks", what));
    }
    return {inputs, total};
}

struct AssetInfo {
    AssetId id;
    std::optional<bitassets::AssetRecord> record;
    uint8_t decimals{0};
    std::string label;
};

/** An asset an argument names: CHN, or a registered asset. */
AssetInfo ParseAsset(CWallet& wallet, const UniValue& value)
{
    AssetInfo info;
    info.id = bitassets::ParseAssetArg(value);
    if (info.id.IsNull()) {
        info.decimals = bitassets::CHN_DECIMALS;
        info.label = "CHN";
        return info;
    }
    info.record = wallet.chain().getBitAsset(info.id);
    if (!info.record) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset %s is registered", value.get_str()));
    info.decimals = info.record->decimals;
    info.label = bitassets::AssetLabel(info.id, &*info.record);
    return info;
}

/** A slippage in percent, as a fraction of 10000. */
uint64_t Slippage(const UniValue& value, double fallback)
{
    const double percent{value.isNull() ? fallback : value.get_real()};
    if (!(percent >= 0 && percent < 100)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The slippage is a percentage, from 0 to less than 100");
    return static_cast<uint64_t>(std::llround(percent * 100));
}

uint64_t LessSlippage(uint64_t amount, uint64_t slippage)
{
    return static_cast<uint64_t>(static_cast<unsigned __int128>(amount) * (10000 - slippage) / 10000);
}

/** An output of a transaction to make: a token it carries, or (none) a result of the operation. */
struct Out {
    CTxDestination dest;
    std::optional<Token> token;
};

/**
 * Make, sign and send a BitAssets transaction: the outputs `outs` first, in this order, then the
 * marker (burning `chn_in`, the CHN the operation takes in), then the change.
 */
CTransactionRef Send(CWallet& wallet, std::optional<bitassets::Operation> operation, const std::vector<Out>& outs, const std::vector<COutPoint>& inputs, CAmount chn_in = 0)
{
    Marker marker;
    marker.operation = std::move(operation);
    std::vector<CRecipient> recipients;
    for (const Out& out : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(recipients.size()), out.token});
        recipients.push_back({out.dest, 0, /*fSubtractFeeFromAmount=*/false});
    }
    recipients.push_back({CNoDestination{bitassets::MarkerScript(marker)}, chn_in, false});

    CCoinControl coin_control;
    for (const COutPoint& input : inputs) coin_control.Select(input);
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/recipients.size(), coin_control, /*sign=*/true)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    const CTransactionRef tx{res->tx};
    wallet.CommitTransaction(tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    if (!wallet.chain().isInMempool(tx->GetHash())) {
        wallet.AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The transaction was not accepted into the mempool (see the debug log for why)");
    }
    return tx;
}

Token AssetToken(const AssetId& id, uint64_t amount)
{
    Token token;
    token.kind = Token::Kind::ASSET;
    token.id = id;
    token.amount = amount;
    return token;
}

Token UnitToken(Token::Kind kind, const uint256& id)
{
    Token token;
    token.kind = kind;
    token.id = id;
    return token;
}

/**
 * What an operation takes in of an asset: the CHN to burn, or the asset coins to spend, whose
 * change goes back to the wallet.
 */
void PayIn(CWallet& wallet, const AssetInfo& asset, uint64_t amount, std::vector<COutPoint>& inputs, std::vector<Out>& outs, CAmount& chn_in)
{
    if (asset.id.IsNull()) {
        if (amount > static_cast<uint64_t>(MAX_MONEY)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too much CHN");
        chn_in += static_cast<CAmount>(amount);
        return;
    }
    const auto [selected, total]{Select(wallet, Token::Kind::ASSET, asset.id, amount, asset.label)};
    inputs.insert(inputs.end(), selected.begin(), selected.end());
    if (total > amount) outs.push_back({ChangeDestination(wallet), AssetToken(asset.id, total - amount)});
}

/** Where the results of an operation go: a result output for each that is not CHN. */
void ResultOuts(CWallet& wallet, const std::vector<AssetId>& results, std::vector<Out>& outs)
{
    for (const AssetId& asset : results) {
        if (!asset.IsNull()) outs.push_back({ChangeDestination(wallet), std::nullopt});
    }
}

CScript ChnTo(CWallet& wallet, const std::vector<AssetId>& results)
{
    const bool chn{std::any_of(results.begin(), results.end(), [](const AssetId& a) { return a.IsNull(); })};
    return chn ? GetScriptForDestination(NewDestination(wallet, "bitassets payout")) : CScript{};
}

/** The reservation of this wallet for a name, and its nonce. */
std::optional<std::pair<COutPoint, uint256>> FindReservation(CWallet& wallet, const AssetId& name)
{
    for (const OwnedToken& owned : OwnedTokens(wallet)) {
        if (owned.token.kind != Token::Kind::RESERVATION || !std::holds_alternative<WitnessV0KeyHash>(owned.dest)) continue;
        const auto commitment{wallet.chain().getBitAssetsReservation(Txid::FromUint256(owned.token.id))};
        if (!commitment) continue;
        const uint256 nonce{ReservationNonce(wallet, owned.dest, name)};
        if (bitassets::ReservationCommitment(name, nonce) == *commitment) return std::make_pair(owned.outpoint, nonce);
    }
    return std::nullopt;
}

UniValue TxResult(const CTransactionRef& tx)
{
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
}

const RPCResult TXID_RESULT{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The transaction"}}};

uint8_t DecimalsOf(CWallet& wallet, const AssetId& asset)
{
    if (asset.IsNull()) return bitassets::CHN_DECIMALS;
    const auto record{wallet.chain().getBitAsset(asset)};
    return record ? record->decimals : 0;
}

std::string LabelOfAsset(CWallet& wallet, const AssetId& asset)
{
    if (asset.IsNull()) return "CHN";
    const auto record{wallet.chain().getBitAsset(asset)};
    return bitassets::AssetLabel(asset, record ? &*record : nullptr);
}

} // namespace

RPCMethod reserveasset()
{
    return RPCMethod{
        "reserveasset",
        "Reserve the name of an asset: the first of two steps to register it. The reservation commits to the name\n"
        "without showing it, so that nobody can see it coming and take it first; registerasset registers it once\n"
        "the reservation is in a block." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, strprintf("The name: 1 to %u printable characters", bitassets::MAX_NAME_TEXT_SIZE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "asset", "The hash of the name, by which the chain will know the asset"},
        }},
        RPCExamples{HelpExampleCli("reserveasset", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const std::string name{request.params[0].get_str()};
    if (!bitassets::IsAssetName(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A name is 1 to %u printable characters, without a space at either end", bitassets::MAX_NAME_TEXT_SIZE));
    if (ToUpper(name) == "CHN") throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is the coin of the chain");
    const AssetId asset{bitassets::HashName(name)};
    if (pwallet->chain().getBitAsset(asset)) throw JSONRPCError(RPC_INVALID_PARAMETER, "An asset of this name is registered");
    const CTxDestination dest{NewDestination(*pwallet, ReservationLabel(name))};
    Token reservation{UnitToken(Token::Kind::RESERVATION, uint256{})};
    const CTransactionRef tx{Send(*pwallet, bitassets::Reserve{bitassets::ReservationCommitment(asset, ReservationNonce(*pwallet, dest, asset))}, {{dest, reservation}}, {})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("asset", asset.GetHex());
    return result;
},
    };
}

RPCMethod registerasset()
{
    return RPCMethod{
        "registerasset",
        "Register an asset reserved with reserveasset, once the reservation is in a block: create its initial supply\n"
        "and its control coin, which mints more and changes its data." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name"},
            {"supply", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The initial supply, in whole assets (0 for none yet)"},
            {"decimals", RPCArg::Type::NUM, RPCArg::Default{0}, strprintf("The decimals it is shown with, at most %u: with 2, 1 is 100 units", bitassets::MAX_DECIMALS)},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "Its data", bitassets::DataArgs()},
            {"public", RPCArg::Type::BOOL, RPCArg::Default{true}, "Publish the name, so that it is listed and found by name. Otherwise only its hash is on the chain"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "asset", "The hash of the name"},
        }},
        RPCExamples{HelpExampleCli("registerasset", "\"GOLD\" 1000000 2 '{\"info\": \"One gram of gold\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const std::string name{request.params[0].get_str()};
    const AssetId asset{bitassets::HashName(name)};
    if (pwallet->chain().getBitAsset(asset)) throw JSONRPCError(RPC_INVALID_PARAMETER, "An asset of this name is registered");
    const int decimals{request.params[2].isNull() ? 0 : request.params[2].getInt<int>()};
    if (decimals < 0 || decimals > bitassets::MAX_DECIMALS) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The decimals are from 0 to %u", bitassets::MAX_DECIMALS));
    const uint64_t supply{bitassets::ParseUnits(request.params[1], decimals, /*allow_zero=*/true)};
    const auto reservation{FindReservation(*pwallet, asset)};
    if (!reservation) throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet has no reservation of the name in a block (see reserveasset)");
    bitassets::Register reg;
    reg.name = asset;
    reg.nonce = reservation->second;
    reg.supply = supply;
    reg.decimals = decimals;
    if (!request.params[3].isNull()) reg.data = bitassets::ParseData(request.params[3]);
    if (request.params[4].isNull() || request.params[4].get_bool()) reg.text = name;
    std::vector<Out> outs{{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset)}};
    if (supply > 0) outs.push_back({ChangeDestination(*pwallet), AssetToken(asset, supply)});
    const CTransactionRef tx{Send(*pwallet, reg, outs, {reservation->first})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("asset", asset.GetHex());
    return result;
},
    };
}

RPCMethod releaseassetreservation()
{
    return RPCMethod{
        "releaseassetreservation",
        "Give up a reservation of this wallet: for a name another registered first, or one no longer wanted." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name reserved"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("releaseassetreservation", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetId asset{bitassets::HashName(request.params[0].get_str())};
    const auto reservation{FindReservation(*pwallet, asset)};
    if (!reservation) throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet has no reservation of the name in a block");
    const auto token{pwallet->chain().getBitAssetsToken(reservation->first)};
    bitassets::Burn burn;
    burn.tokens.push_back(*token);
    return TxResult(Send(*pwallet, burn, {}, {reservation->first}));
},
    };
}

RPCMethod listmyassets()
{
    return RPCMethod{
        "listmyassets",
        "What this wallet holds, in blocks: asset coins, control coins, reservations, pool shares and auction receipts.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "assets", "Assets held or controlled", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "asset", "Its hash"},
                {RPCResult::Type::STR, "label", "Its name, or \"0x\" and its hash"},
                {RPCResult::Type::NUM, "decimals", ""},
                {RPCResult::Type::NUM, "balance", "What the wallet holds in blocks: what it can spend"},
                {RPCResult::Type::NUM, "pending", "What comes to it by transactions waiting for a block (its change included)"},
                {RPCResult::Type::NUM, "outputs", "In how many outputs"},
                {RPCResult::Type::BOOL, "control", "Whether the wallet holds its control coin"},
                {RPCResult::Type::BOOL, "control_pending", "Whether the control coin comes back to it by a transaction waiting for a block"},
                {RPCResult::Type::NUM, "supply", "How much there is"},
                {RPCResult::Type::BOOL, "fixed", "Whether its supply is fixed for good"},
            }}}},
            {RPCResult::Type::ARR, "reservations", "Names reserved", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The reservation"},
                {RPCResult::Type::STR, "name", /*optional=*/true, "The name, if reserved with this wallet"},
                {RPCResult::Type::BOOL, "taken", "Whether an asset of this name is registered (by another): the reservation can only be released"},
            }}}},
            {RPCResult::Type::ARR, "liquidity", "Shares of pools", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "pool", "The pool"},
                {RPCResult::Type::STR, "asset_a", ""},
                {RPCResult::Type::STR, "asset_b", ""},
                {RPCResult::Type::NUM, "shares", "The shares held"},
                {RPCResult::Type::NUM, "percent", "Of the pool"},
                {RPCResult::Type::NUM, "value_a", "What they would take out of the first asset"},
                {RPCResult::Type::NUM, "value_b", "And of the second"},
            }}}},
            {RPCResult::Type::ARR, "receipts", "Auctions this wallet collects", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "auction", ""},
            }}}},
        }},
        RPCExamples{HelpExampleCli("listmyassets", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    struct Holding {
        uint64_t balance{0};
        //! Coming in by transactions not in a block yet (change included).
        uint64_t pending{0};
        int outputs{0};
        bool control{false};
        //! Moving by a transaction not in a block yet (a mint, a change of data, a transfer).
        bool control_pending{false};
    };
    std::map<AssetId, Holding> holdings;
    std::map<uint256, uint64_t> shares;
    UniValue reservations(UniValue::VARR), receipts(UniValue::VARR);
    for (const OwnedToken& owned : OwnedTokens(*pwallet)) {
        switch (owned.token.kind) {
        case Token::Kind::ASSET:
            holdings[owned.token.id].balance += owned.token.amount;
            ++holdings[owned.token.id].outputs;
            break;
        case Token::Kind::CONTROL: holdings[owned.token.id].control = true; break;
        case Token::Kind::LP: shares[owned.token.id] += owned.token.amount; break;
        case Token::Kind::RECEIPT: {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("auction", owned.token.id.GetHex());
            receipts.push_back(std::move(obj));
            break;
        }
        case Token::Kind::RESERVATION: {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", owned.token.id.GetHex());
            const auto label{LabelOf(*pwallet, owned.dest)};
            bool taken{false};
            if (label && label->starts_with("bitasset:")) {
                const std::string name{label->substr(9)};
                obj.pushKV("name", name);
                taken = pwallet->chain().getBitAsset(bitassets::HashName(name)).has_value();
            }
            obj.pushKV("taken", taken);
            reservations.push_back(std::move(obj));
            break;
        }
        }
    }
    // What transactions of the wallet not in a block yet bring it: their markers say what each output carries.
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            if (wtx.isAbandoned() || pwallet->GetTxDepthInMainChain(wtx) != 0) continue;
            const CTransaction& tx{*wtx.GetTx()};
            const auto marker{bitassets::GetMarker(tx)};
            if (!marker) continue;
            for (const auto& out : marker->outputs) {
                if (!out.token || out.n >= tx.vout.size() || !pwallet->IsMine(tx.vout[out.n])) continue;
                if (pwallet->IsSpent(COutPoint{txid, out.n})) continue;
                if (out.token->kind == Token::Kind::ASSET) holdings[out.token->id].pending += out.token->amount;
                if (out.token->kind == Token::Kind::CONTROL) holdings[out.token->id].control_pending = true;
            }
        }
    }
    UniValue assets(UniValue::VARR);
    for (const auto& [id, holding] : holdings) {
        const auto record{pwallet->chain().getBitAsset(id)};
        const uint8_t decimals{record ? record->decimals : uint8_t{0}};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("asset", id.GetHex());
        obj.pushKV("label", bitassets::AssetLabel(id, record ? &*record : nullptr));
        obj.pushKV("decimals", decimals);
        obj.pushKV("balance", bitassets::AmountToJSON(holding.balance, decimals));
        obj.pushKV("pending", bitassets::AmountToJSON(holding.pending, decimals));
        obj.pushKV("outputs", holding.outputs);
        obj.pushKV("control", holding.control || holding.control_pending);
        obj.pushKV("control_pending", holding.control_pending);
        obj.pushKV("supply", bitassets::AmountToJSON(record ? record->supply : 0, decimals));
        obj.pushKV("fixed", record && record->fixed);
        assets.push_back(std::move(obj));
    }
    UniValue liquidity(UniValue::VARR);
    for (const auto& [id, held] : shares) {
        const auto pool{pwallet->chain().getBitAssetsPool(id)};
        if (!pool) continue;
        const auto [v0, v1]{bitassets::amm::Withdraw(*pool, held)};
        // A pool with CHN in it shows CHN second.
        const bool flip{pool->asset0.IsNull()};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("pool", id.GetHex());
        obj.pushKV("asset_a", LabelOfAsset(*pwallet, flip ? pool->asset1 : pool->asset0));
        obj.pushKV("asset_b", LabelOfAsset(*pwallet, flip ? pool->asset0 : pool->asset1));
        obj.pushKV("shares", held);
        UniValue percent;
        percent.setNumStr(strprintf("%.4f", pool->shares ? 100.0 * held / pool->shares : 0.0));
        obj.pushKV("percent", percent);
        obj.pushKV("value_a", bitassets::AmountToJSON(flip ? v1 : v0, DecimalsOf(*pwallet, flip ? pool->asset1 : pool->asset0)));
        obj.pushKV("value_b", bitassets::AmountToJSON(flip ? v0 : v1, DecimalsOf(*pwallet, flip ? pool->asset0 : pool->asset1)));
        liquidity.push_back(std::move(obj));
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("assets", std::move(assets));
    result.pushKV("reservations", std::move(reservations));
    result.pushKV("liquidity", std::move(liquidity));
    result.pushKV("receipts", std::move(receipts));
    return result;
},
    };
}

RPCMethod sendasset()
{
    return RPCMethod{
        "sendasset",
        "Send an asset to an address." + HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Where to"},
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much, in whole assets"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("sendasset", "\"address\" \"GOLD\" 12.5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const CTxDestination dest{DecodeDestination(request.params[0].get_str())};
    if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    const AssetInfo asset{ParseAsset(*pwallet, request.params[1])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is sent with sendtoaddress");
    const uint64_t amount{bitassets::ParseUnits(request.params[2], asset.decimals)};
    const auto [inputs, total]{Select(*pwallet, Token::Kind::ASSET, asset.id, amount, asset.label)};
    std::vector<Out> outs{{dest, AssetToken(asset.id, amount)}};
    if (total > amount) outs.push_back({ChangeDestination(*pwallet), AssetToken(asset.id, total - amount)});
    return TxResult(Send(*pwallet, std::nullopt, outs, inputs));
},
    };
}

RPCMethod mintasset()
{
    return RPCMethod{
        "mintasset",
        "Mint more of an asset this wallet controls." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much, in whole assets"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Where the new coins go (default: this wallet)"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("mintasset", "\"GOLD\" 1000")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is not minted");
    if (asset.record->fixed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The supply of this asset is fixed: its control coin was burned");
    const uint64_t amount{bitassets::ParseUnits(request.params[1], asset.decimals)};
    CTxDestination dest{ChangeDestination(*pwallet)};
    if (!request.params[2].isNull()) {
        dest = DecodeDestination(request.params[2].get_str());
        if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    return TxResult(Send(*pwallet, bitassets::Mint{asset.id, amount}, {{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset.id)}, {dest, AssetToken(asset.id, amount)}}, inputs));
},
    };
}

RPCMethod burnasset()
{
    return RPCMethod{
        "burnasset",
        "Destroy some of an asset this wallet holds: the supply goes down by as much." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much, in whole assets"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("burnasset", "\"GOLD\" 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is not burned this way");
    const uint64_t amount{bitassets::ParseUnits(request.params[1], asset.decimals)};
    const auto [inputs, total]{Select(*pwallet, Token::Kind::ASSET, asset.id, amount, asset.label)};
    std::vector<Out> outs;
    if (total > amount) outs.push_back({ChangeDestination(*pwallet), AssetToken(asset.id, total - amount)});
    bitassets::Burn burn;
    burn.tokens.push_back(AssetToken(asset.id, amount));
    return TxResult(Send(*pwallet, burn, outs, inputs));
},
    };
}

RPCMethod updateasset()
{
    return RPCMethod{
        "updateasset",
        "Change the data of an asset this wallet controls. A field left out is kept, a field set to null is deleted." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::NO, "The fields to change", bitassets::DataArgs()},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("updateasset", "\"GOLD\" '{\"info\": \"One gram of gold, held in Zurich\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN has no data");
    if (asset.record->fixed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The data of this asset is fixed: its control coin was burned");
    const bitassets::AssetUpdates updates{bitassets::ParseUpdates(request.params[1])};
    if (updates.Empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Nothing to change");
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    return TxResult(Send(*pwallet, bitassets::UpdateAsset{asset.id, updates}, {{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset.id)}}, inputs));
},
    };
}

RPCMethod transferassetcontrol()
{
    return RPCMethod{
        "transferassetcontrol",
        "Give the control coin of an asset to another address: its holder mints and changes the data." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Where to"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("transferassetcontrol", "\"GOLD\" \"address\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN has no control coin");
    const CTxDestination dest{DecodeDestination(request.params[1].get_str())};
    if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    return TxResult(Send(*pwallet, std::nullopt, {{dest, UnitToken(Token::Kind::CONTROL, asset.id)}}, inputs));
},
    };
}

RPCMethod fixassetsupply()
{
    return RPCMethod{
        "fixassetsupply",
        "Burn the control coin of an asset: nobody can mint more of it or change its data, ever." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"confirm", RPCArg::Type::BOOL, RPCArg::Optional::NO, "true: this cannot be undone"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("fixassetsupply", "\"GOLD\" true")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    if (!request.params[1].get_bool()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not confirmed");
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN has no control coin");
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    bitassets::Burn burn;
    burn.tokens.push_back(UnitToken(Token::Kind::CONTROL, asset.id));
    return TxResult(Send(*pwallet, burn, {}, inputs));
},
    };
}

RPCMethod swapasset()
{
    return RPCMethod{
        "swapasset",
        "Trade in a pool: pay in an amount of one asset, get at least the quoted amount of another, less the slippage.\n"
        "CHN that comes out is paid by the coinbase of the block." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + std::string{bitassets::ASSET_ARG_HELP}},
            {"amount_in", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
            {"asset_out", RPCArg::Type::STR, RPCArg::Optional::NO, "What comes out"},
            {"slippage", RPCArg::Type::NUM, RPCArg::Default{0.5}, "How much less than the quote, in percent, the trade may give if the pool moves first"},
            {"max_impact", RPCArg::Type::NUM, RPCArg::Default{10}, "The worst price impact accepted, in percent: a trade too large for the pool, which would pay far more than the pool's price, is refused (100: anything)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::NUM, "quote", "What it gives if nothing moves the pool first"},
            {RPCResult::Type::NUM, "min_out", "The least it gives"},
        }},
        RPCExamples{HelpExampleCli("swapasset", "\"CHN\" 1 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo in{ParseAsset(*pwallet, request.params[0])};
    const AssetInfo out{ParseAsset(*pwallet, request.params[2])};
    if (in.id == out.id) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
    const uint64_t amount_in{bitassets::ParseUnits(request.params[1], in.decimals)};
    const uint64_t slippage{Slippage(request.params[3], 0.5)};
    const auto pool{pwallet->chain().getBitAssetsPool(bitassets::PoolId(in.id, out.id))};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets");
    const bool zero_in{pool->asset0 == in.id};
    const uint64_t quote{bitassets::amm::SwapOut(zero_in ? pool->reserve0 : pool->reserve1, zero_in ? pool->reserve1 : pool->reserve0, amount_in)};
    if (quote == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it buys nothing");
    const double max_impact{request.params[4].isNull() ? 10.0 : request.params[4].get_real()};
    if (!(max_impact >= 0 && max_impact <= 100)) throw JSONRPCError(RPC_INVALID_PARAMETER, "max_impact is a percentage, from 0 to 100");
    // A pool nobody provides liquidity to holds only dust: whatever goes in buys almost nothing.
    if (bitassets::amm::Abandoned(*pool) && max_impact < 100) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Nobody provides liquidity to this pool any more: it holds only the minimum it keeps for good, and a trade would get almost nothing (add liquidity first)");
    }
    const double impact{bitassets::amm::PriceImpact(zero_in ? pool->reserve0 : pool->reserve1, zero_in ? pool->reserve1 : pool->reserve0, amount_in, quote)};
    if (impact > max_impact) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The trade is too large for this pool: its price would be %.2f%% worse than the pool's (at most %.2f%% accepted; see max_impact)", impact, max_impact));
    }
    const uint64_t min_out{std::max<uint64_t>(1, LessSlippage(quote, slippage))};
    std::vector<COutPoint> inputs;
    std::vector<Out> outs;
    CAmount chn_in{0};
    PayIn(*pwallet, in, amount_in, inputs, outs, chn_in);
    ResultOuts(*pwallet, {out.id}, outs);
    const CTransactionRef tx{Send(*pwallet, bitassets::Swap{in.id, amount_in, out.id, min_out, ChnTo(*pwallet, {out.id})}, outs, inputs, chn_in)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("quote", bitassets::AmountToJSON(quote, out.decimals));
    result.pushKV("min_out", bitassets::AmountToJSON(min_out, out.decimals));
    return result;
},
    };
}

RPCMethod addliquidity()
{
    return RPCMethod{
        "addliquidity",
        "Put two assets in their pool, for shares of it; this makes the pool if there is none, at the price the amounts\n"
        "set. Into a pool there is, the second amount is worked out from the first at the pool's price, unless given." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount_a", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much of it"},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount_b", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "How much of it: needed for a new pool"},
            {"slippage", RPCArg::Type::NUM, RPCArg::Default{1}, "How many fewer shares than quoted, in percent, it may give if the pool moves first"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::NUM, "amount_a", "What goes in of the first asset"},
            {RPCResult::Type::NUM, "amount_b", "And of the second"},
            {RPCResult::Type::NUM, "shares", "The shares it gives if nothing moves the pool first"},
        }},
        RPCExamples{HelpExampleCli("addliquidity", "\"GOLD\" 100 \"CHN\" 5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo a{ParseAsset(*pwallet, request.params[0])};
    const AssetInfo b{ParseAsset(*pwallet, request.params[2])};
    if (a.id == b.id) throw JSONRPCError(RPC_INVALID_PARAMETER, "Two different assets make a pool");
    const uint64_t amount_a{bitassets::ParseUnits(request.params[1], a.decimals)};
    const uint64_t slippage{Slippage(request.params[4], 1)};
    const auto pool{pwallet->chain().getBitAssetsPool(bitassets::PoolId(a.id, b.id))};
    uint64_t amount_b;
    if (!request.params[3].isNull()) {
        amount_b = bitassets::ParseUnits(request.params[3], b.decimals);
    } else {
        if (!pool || pool->shares == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool yet: give both amounts, which set its price");
        const bool a_first{pool->asset0 == a.id};
        const uint64_t ra{a_first ? pool->reserve0 : pool->reserve1}, rb{a_first ? pool->reserve1 : pool->reserve0};
        // At the pool's price, rounded up so that the first amount is the one that counts.
        const unsigned __int128 b_needed{(static_cast<unsigned __int128>(amount_a) * rb + ra - 1) / ra};
        if (b_needed == 0 || b_needed > bitassets::MAX_AMOUNT) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little or too much for this pool");
        amount_b = static_cast<uint64_t>(b_needed);
    }
    bitassets::Pool current;
    if (pool) current = *pool;
    const bool a_first{pool ? pool->asset0 == a.id : a.id < b.id};
    uint64_t shares{bitassets::amm::SharesFor(current, a_first ? amount_a : amount_b, a_first ? amount_b : amount_a)};
    if (current.shares == 0) shares = shares > bitassets::MIN_LIQUIDITY ? shares - bitassets::MIN_LIQUIDITY : 0;
    if (shares == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it makes no shares (a new pool needs the product of the amounts, in units, above a million)");
    // Opening (or reopening an abandoned) pool takes a deposit worth trading against.
    if (bitassets::amm::Abandoned(current)) {
        const uint64_t chn{a.id.IsNull() ? amount_a : b.id.IsNull() ? amount_b : bitassets::MIN_OPEN_CHN};
        if (shares < bitassets::MIN_OPEN_SHARES || chn < bitassets::MIN_OPEN_CHN) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little to open a pool: it takes at least %s CHN on the CHN side, and amounts whose product, in units, is above %u", FormatMoney(bitassets::MIN_OPEN_CHN), (bitassets::MIN_OPEN_SHARES + bitassets::MIN_LIQUIDITY) * (bitassets::MIN_OPEN_SHARES + bitassets::MIN_LIQUIDITY)));
        }
    }
    std::vector<COutPoint> inputs;
    std::vector<Out> outs;
    CAmount chn_in{0};
    PayIn(*pwallet, a, amount_a, inputs, outs, chn_in);
    PayIn(*pwallet, b, amount_b, inputs, outs, chn_in);
    outs.push_back({ChangeDestination(*pwallet), std::nullopt});
    const CTransactionRef tx{Send(*pwallet, bitassets::AddLiquidity{a.id, b.id, amount_a, amount_b, std::max<uint64_t>(1, LessSlippage(shares, slippage))}, outs, inputs, chn_in)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount_a", bitassets::AmountToJSON(amount_a, a.decimals));
    result.pushKV("amount_b", bitassets::AmountToJSON(amount_b, b.decimals));
    result.pushKV("shares", shares);
    return result;
},
    };
}

RPCMethod removeliquidity()
{
    return RPCMethod{
        "removeliquidity",
        "Give back shares of a pool this wallet holds, for its two assets." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"percent", RPCArg::Type::NUM, RPCArg::Default{100}, "How much of the shares held, in percent"},
            {"slippage", RPCArg::Type::NUM, RPCArg::Default{1}, "How much less than quoted, in percent, it may give if the pool moves first"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::NUM, "shares", "The shares given back"},
            {RPCResult::Type::NUM, "amount_a", "What they take out of the first asset, if nothing moves the pool first"},
            {RPCResult::Type::NUM, "amount_b", "And of the second"},
        }},
        RPCExamples{HelpExampleCli("removeliquidity", "\"GOLD\" \"CHN\" 50")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo a{ParseAsset(*pwallet, request.params[0])};
    const AssetInfo b{ParseAsset(*pwallet, request.params[1])};
    const double percent{request.params[2].isNull() ? 100.0 : request.params[2].get_real()};
    if (!(percent > 0 && percent <= 100)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The percentage is above 0, at most 100");
    const uint64_t slippage{Slippage(request.params[3], 1)};
    const uint256 id{bitassets::PoolId(a.id, b.id)};
    const auto pool{pwallet->chain().getBitAssetsPool(id)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets");
    uint64_t held{0};
    for (const OwnedToken& t : OwnedTokens(*pwallet)) {
        if (t.token.kind == Token::Kind::LP && t.token.id == id) held += t.token.amount;
    }
    if (held == 0) NotHeld(*pwallet, Token::Kind::LP, id, "shares of this pool");
    const uint64_t shares{percent >= 100 ? held : static_cast<uint64_t>(static_cast<long double>(held) * percent / 100)};
    if (shares == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "That is no share at all");
    const auto [out0, out1]{bitassets::amm::Withdraw(*pool, shares)};
    const bool a_first{pool->asset0 == a.id};
    const uint64_t out_a{a_first ? out0 : out1}, out_b{a_first ? out1 : out0};
    if (out_a == 0 || out_b == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too few shares: they take nothing out of one of the assets");
    const auto [inputs, total]{Select(*pwallet, Token::Kind::LP, id, shares, "shares of this pool")};
    std::vector<Out> outs;
    if (total > shares) {
        Token change{UnitToken(Token::Kind::LP, id)};
        change.amount = total - shares;
        outs.push_back({ChangeDestination(*pwallet), change});
    }
    ResultOuts(*pwallet, {a.id, b.id}, outs);
    bitassets::RemoveLiquidity remove{a.id, b.id, shares, std::max<uint64_t>(1, LessSlippage(out_a, slippage)), std::max<uint64_t>(1, LessSlippage(out_b, slippage)), ChnTo(*pwallet, {a.id, b.id})};
    const CTransactionRef tx{Send(*pwallet, remove, outs, inputs)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("shares", shares);
    result.pushKV("amount_a", bitassets::AmountToJSON(out_a, a.decimals));
    result.pushKV("amount_b", bitassets::AmountToJSON(out_b, b.decimals));
    return result;
},
    };
}

RPCMethod createauction()
{
    return RPCMethod{
        "createauction",
        "Sell an amount of an asset by Dutch auction: the price of all of it starts high and falls block by block, in a\n"
        "straight line, to the end price; bids buy at the price of their block. The receipt, which this wallet keeps,\n"
        "collects what it brings in and what is left, once it ends or sells out (or at once, to cancel it, before any bid)." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, "What it sells. " + std::string{bitassets::ASSET_ARG_HELP}},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
            {"quote", RPCArg::Type::STR, RPCArg::Optional::NO, "What it sells for"},
            {"start_price", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The price of all of it at the start, in what it sells for"},
            {"end_price", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The price of all of it at the end: the least it sells for"},
            {"duration", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks it takes bids"},
            {"start_in", RPCArg::Type::NUM, RPCArg::Default{1}, "When it starts, in blocks after the one that takes the transaction: 1 is the block after that"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("createauction", "\"GOLD\" 100 \"CHN\" 50 10 1440")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo base{ParseAsset(*pwallet, request.params[0])};
    const AssetInfo quote{ParseAsset(*pwallet, request.params[2])};
    if (base.id == quote.id) throw JSONRPCError(RPC_INVALID_PARAMETER, "It sells one asset for another");
    bitassets::CreateAuction create;
    create.base = base.id;
    create.base_amount = bitassets::ParseUnits(request.params[1], base.decimals);
    create.quote = quote.id;
    create.start_price = bitassets::ParseUnits(request.params[3], quote.decimals);
    create.end_price = bitassets::ParseUnits(request.params[4], quote.decimals);
    if (create.end_price > create.start_price) throw JSONRPCError(RPC_INVALID_PARAMETER, "The price falls: the end price is at most the start price");
    create.duration = request.params[5].getInt<int>();
    if (create.duration < 1 || create.duration > bitassets::MAX_AUCTION_DURATION) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The duration is 1 to %u blocks", bitassets::MAX_AUCTION_DURATION));
    const int start_in{request.params[6].isNull() ? 1 : request.params[6].getInt<int>()};
    if (start_in < 1) throw JSONRPCError(RPC_INVALID_PARAMETER, "It starts a block after the one that takes the transaction, at the earliest");
    // An auction cannot start before the block that makes it: a block of margin, should the transaction wait one to be mined.
    create.start_height = pwallet->chain().getBitAssetsHeight() + start_in;
    std::vector<COutPoint> inputs;
    std::vector<Out> outs{{ChangeDestination(*pwallet), UnitToken(Token::Kind::RECEIPT, uint256{})}};
    CAmount chn_in{0};
    PayIn(*pwallet, base, create.base_amount, inputs, outs, chn_in);
    return TxResult(Send(*pwallet, create, outs, inputs, chn_in));
},
    };
}

RPCMethod bidauction()
{
    return RPCMethod{
        "bidauction",
        "Bid on an auction: pay an amount of what it sells for, get what that buys at the price of the block that\n"
        "takes the bid (at least what it buys in the next block: the price only falls)." + HELP_REQUIRING_PASSPHRASE,
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "What to pay"},
            {"buy_all", RPCArg::Type::BOOL, RPCArg::Default{false}, "Pay what buys all that is left, at the price of the next block, instead"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::NUM, "pays", "What it pays"},
            {RPCResult::Type::NUM, "buys", "The least it buys"},
        }},
        RPCExamples{HelpExampleCli("bidauction", "\"txid\" 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    const auto auction{pwallet->chain().getBitAssetsAuction(id)};
    if (!auction) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    const int height{pwallet->chain().getBitAssetsHeight()};
    if (!auction->OpenAt(height)) {
        if (height < auction->start_height) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The auction starts at height %d", auction->start_height));
        throw JSONRPCError(RPC_INVALID_PARAMETER, "The auction takes no more bids");
    }
    const uint8_t dq{DecimalsOf(*pwallet, auction->quote)}, db{DecimalsOf(*pwallet, auction->base)};
    const uint64_t price{auction->PriceAt(height)};
    uint64_t amount;
    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        // The most that buys no more than what is left.
        const unsigned __int128 most{((static_cast<unsigned __int128>(auction->remaining) + 1) * price - 1) / auction->base_amount};
        amount = static_cast<uint64_t>(std::min<unsigned __int128>(most, bitassets::MAX_AMOUNT));
    } else {
        if (request.params[1].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Give an amount, or buy_all");
        amount = bitassets::ParseUnits(request.params[1], dq);
    }
    const uint64_t buys{auction->BuysAt(height, amount)};
    if (buys == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it buys nothing at the price of the next block");
    if (buys > auction->remaining) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too much: it buys more than is left (see buy_all)");
    const AssetInfo quote{auction->quote, std::nullopt, dq, LabelOfAsset(*pwallet, auction->quote)};
    std::vector<COutPoint> inputs;
    std::vector<Out> outs;
    CAmount chn_in{0};
    PayIn(*pwallet, quote, amount, inputs, outs, chn_in);
    ResultOuts(*pwallet, {auction->base}, outs);
    const CTransactionRef tx{Send(*pwallet, bitassets::Bid{id, amount, buys, ChnTo(*pwallet, {auction->base})}, outs, inputs, chn_in)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("pays", bitassets::AmountToJSON(amount, dq));
    result.pushKV("buys", bitassets::AmountToJSON(buys, db));
    return result;
},
    };
}

RPCMethod collectauction()
{
    return RPCMethod{
        "collectauction",
        "Close an auction this wallet holds the receipt of: take what it brought in and what is left of what it sold.\n"
        "It can be collected once it has ended or sold out, or before any bid, which cancels it." + HELP_REQUIRING_PASSPHRASE,
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("collectauction", "\"txid\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    const auto auction{pwallet->chain().getBitAssetsAuction(id)};
    if (!auction) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    if (auction->closed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The auction was collected already");
    if (!auction->CollectableAt(pwallet->chain().getBitAssetsHeight())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The auction is running, with bids: it can be collected after height %d, or once sold out", auction->EndHeight()));
    }
    const auto [inputs, total]{Select(*pwallet, Token::Kind::RECEIPT, id.ToUint256(), 1, "the receipt of this auction")};
    std::vector<AssetId> results;
    if (auction->remaining > 0) results.push_back(auction->base);
    if (auction->proceeds > 0) results.push_back(auction->quote);
    std::vector<Out> outs;
    ResultOuts(*pwallet, results, outs);
    return TxResult(Send(*pwallet, bitassets::Collect{id, ChnTo(*pwallet, results)}, outs, inputs));
},
    };
}

RPCMethod releaseasset()
{
    return RPCMethod{
        "releaseasset",
        "Retire a dead asset: its supply fixed, every unit of it in pools nobody provides liquidity to. The asset and\n"
        "its pools go and its name is free again; what the pools hold of other assets is burned, and the CHN they hold\n"
        "is paid to mainchain miners, as the fee of a withdrawal in the next bundle. Anyone may do it; this wallet pays\n"
        "the transaction fee." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("releaseasset", "\"PEBBLES\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is not retired");
    if (!pwallet->chain().getBitAssetReleasable(asset.id)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This asset is not dead: its supply has to be fixed, nobody may hold any of it, and its pools may have no liquidity providers (see getasset)");
    }
    // A release of it waiting already: once mined, the asset is gone, and another could never be.
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            if (wtx.isAbandoned() || pwallet->GetTxDepthInMainChain(wtx) != 0) continue;
            const auto marker{bitassets::GetMarker(*wtx.GetTx())};
            const auto* release{marker && marker->operation ? std::get_if<bitassets::ReleaseAsset>(&*marker->operation) : nullptr};
            if (release && release->asset == asset.id) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("%s is being retired already: transaction %s is waiting for a block", asset.label, txid.GetHex()));
            }
        }
    }
    return TxResult(Send(*pwallet, bitassets::ReleaseAsset{asset.id}, {}, {}));
},
    };
}

RPCMethod listassetactivity()
{
    return RPCMethod{
        "listassetactivity",
        "The BitAssets transactions of this wallet, newest first.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{50}, "How many"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM_TIME, "time", "When the wallet saw it"},
            {RPCResult::Type::NUM, "confirmations", ""},
            {RPCResult::Type::STR, "operation", "What it does"},
            {RPCResult::Type::STR, "summary", "In words"},
        }}}},
        RPCExamples{HelpExampleCli("listassetactivity", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const int count{request.params[0].isNull() ? 50 : request.params[0].getInt<int>()};
    struct Entry {
        int64_t time;
        Txid txid;
        int confirmations;
        Marker marker;
    };
    std::vector<Entry> entries;
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            if (wtx.isAbandoned()) continue;
            const auto marker{bitassets::GetMarker(*wtx.GetTx())};
            if (!marker) continue;
            entries.push_back({wtx.GetTxTime(), txid, pwallet->GetTxDepthInMainChain(wtx), *marker});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
    const auto amount{[&](const AssetId& asset, uint64_t units) { return bitassets::FormatUnits(units, DecimalsOf(*pwallet, asset)) + " " + LabelOfAsset(*pwallet, asset); }};
    UniValue result(UniValue::VARR);
    for (const Entry& e : entries) {
        if (result.size() >= static_cast<size_t>(count)) break;
        std::string operation{"transfer"}, summary;
        if (e.marker.operation) {
            std::visit([&](const auto& op) {
                using T = std::decay_t<decltype(op)>;
                if constexpr (std::is_same_v<T, bitassets::Reserve>) {
                    operation = "reserve";
                    summary = "Reserved a name";
                } else if constexpr (std::is_same_v<T, bitassets::Register>) {
                    operation = "register";
                    summary = strprintf("Registered %s with %s", LabelOfAsset(*pwallet, op.name), amount(op.name, op.supply));
                } else if constexpr (std::is_same_v<T, bitassets::Mint>) {
                    operation = "mint";
                    summary = "Minted " + amount(op.asset, op.amount);
                } else if constexpr (std::is_same_v<T, bitassets::UpdateAsset>) {
                    operation = "update";
                    summary = "Changed the data of " + LabelOfAsset(*pwallet, op.asset);
                } else if constexpr (std::is_same_v<T, bitassets::Burn>) {
                    operation = "burn";
                    std::vector<std::string> parts;
                    for (const Token& t : op.tokens) {
                        if (t.kind == Token::Kind::ASSET) parts.push_back(amount(t.id, t.amount));
                        if (t.kind == Token::Kind::CONTROL) parts.push_back("the control coin of " + LabelOfAsset(*pwallet, t.id));
                        if (t.kind == Token::Kind::RESERVATION) parts.push_back("a reservation");
                    }
                    summary = "Burned " + util::Join(parts, ", ");
                } else if constexpr (std::is_same_v<T, bitassets::Swap>) {
                    operation = "swap";
                    summary = strprintf("Swapped %s for at least %s", amount(op.asset_in, op.amount_in), amount(op.asset_out, op.min_out));
                } else if constexpr (std::is_same_v<T, bitassets::AddLiquidity>) {
                    operation = "add liquidity";
                    summary = strprintf("Put %s and %s in their pool", amount(op.asset_a, op.amount_a), amount(op.asset_b, op.amount_b));
                } else if constexpr (std::is_same_v<T, bitassets::RemoveLiquidity>) {
                    operation = "remove liquidity";
                    summary = strprintf("Took %s and %s out of their pool, at least", amount(op.asset_a, op.min_a), amount(op.asset_b, op.min_b));
                } else if constexpr (std::is_same_v<T, bitassets::CreateAuction>) {
                    operation = "auction";
                    summary = strprintf("Put %s up for auction, from %s down to %s", amount(op.base, op.base_amount), amount(op.quote, op.start_price), amount(op.quote, op.end_price));
                } else if constexpr (std::is_same_v<T, bitassets::Bid>) {
                    operation = "bid";
                    const auto auction{pwallet->chain().getBitAssetsAuction(op.auction)};
                    summary = auction ? strprintf("Bid %s for at least %s", amount(auction->quote, op.quote_amount), amount(auction->base, op.min_base)) : "Bid on an auction";
                } else if constexpr (std::is_same_v<T, bitassets::Collect>) {
                    operation = "collect";
                    summary = "Collected an auction";
                } else if constexpr (std::is_same_v<T, bitassets::ReleaseAsset>) {
                    operation = "release";
                    summary = "Retired a dead asset (its pools' CHN go to mainchain miners)";
                }
            }, *e.marker.operation);
        } else {
            std::vector<std::string> parts;
            for (const auto& out : e.marker.outputs) {
                if (out.token && out.token->kind == Token::Kind::ASSET) parts.push_back(amount(out.token->id, out.token->amount));
                if (out.token && out.token->kind == Token::Kind::CONTROL) parts.push_back("the control coin of " + LabelOfAsset(*pwallet, out.token->id));
            }
            summary = "Moved " + util::Join(parts, ", ");
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", e.txid.GetHex());
        obj.pushKV("time", e.time);
        obj.pushKV("confirmations", e.confirmations);
        obj.pushKV("operation", operation);
        obj.pushKV("summary", summary);
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

} // namespace wallet

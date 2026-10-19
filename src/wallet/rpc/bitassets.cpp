// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <addresstype.h>
#include <bitassets/logo.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <coins.h>
#include <core_io.h>
#include <crypto/sha256.h>
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
    // By its number too, as the node's commands take it.
    info.id = bitassets::ParseAssetArg(value, [&](uint32_t seq) { return wallet.chain().getBitAssetOfSeq(seq); });
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
CTransactionRef Send(CWallet& wallet, std::optional<bitassets::Operation> operation, const std::vector<Out>& outs, const std::vector<COutPoint>& inputs, CAmount chn_in = 0,
                     const std::optional<CFeeRate>& fee_rate = std::nullopt)
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
    if (fee_rate) coin_control.m_feerate = *fee_rate;
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/recipients.size(), coin_control, /*sign=*/true)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    const CTransactionRef tx{res->tx};
    wallet.CommitTransaction(tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    // Not a failure if a block took it already, or if the wallet does not broadcast (-walletbroadcast=0).
    if (CommittedTransactionRefused(wallet, tx)) {
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

struct OwnReservation {
    COutPoint outpoint;
    Txid id;
    uint256 nonce;
};

/** Whether a reservation of the wallet, carried by `dest`, is of `name`; if so, its nonce. */
std::optional<uint256> ReservationOf(const CWallet& wallet, const CTxDestination& dest, const uint256& commitment, const AssetId& name)
{
    if (!std::holds_alternative<WitnessV0KeyHash>(dest)) return std::nullopt;
    const uint256 nonce{ReservationNonce(wallet, dest, name)};
    // Bound to the script of its output.
    if (bitassets::ReservationCommitment(name, nonce, GetScriptForDestination(dest)) == commitment) return nonce;
    return std::nullopt;
}

/** The reservation of this wallet for a name, and its nonce. */
std::optional<OwnReservation> FindReservation(CWallet& wallet, const AssetId& name)
{
    for (const OwnedToken& owned : OwnedTokens(wallet)) {
        if (owned.token.kind != Token::Kind::RESERVATION || !std::holds_alternative<WitnessV0KeyHash>(owned.dest)) continue;
        const Txid id{Txid::FromUint256(owned.token.id)};
        const auto commitment{wallet.chain().getBitAssetsReservation(id)};
        if (!commitment) continue;
        if (const auto nonce{ReservationOf(wallet, owned.dest, *commitment, name)}) return OwnReservation{owned.outpoint, id, *nonce};
    }
    return std::nullopt;
}

/**
 * A registration of `name` by this wallet that the mempool dropped (the rules refused it at a later tip,
 * say: it was stale), made abandoned so that its reservation can be registered again. Not with
 * -walletbroadcast=0, where a transaction out of the mempool is one the user keeps to send.
 */
void AbandonDroppedRegistration(CWallet& wallet, const AssetId& name)
{
    if (!wallet.GetBroadcastTransactions()) return;
    LOCK(wallet.cs_wallet);
    for (auto& [txid, wtx] : wallet.mapWallet) {
        if (wallet.GetTxDepthInMainChain(wtx) != 0 || wtx.isAbandoned() || wtx.InMempool()) continue;
        const auto marker{bitassets::GetMarker(*wtx.GetTx())};
        if (!marker || !marker->operation) continue;
        const auto* reg{std::get_if<bitassets::Register>(&*marker->operation)};
        if (reg && reg->name == name) wallet.AbandonTransaction(wtx);
    }
}

/**
 * The first height from which a block may take the registration of a reservation made in the block
 * at `made`, it and every block after it: a reservation is revealed only `reveal_depth` blocks after
 * the block that made it.
 */
int RevealHeight(CWallet& wallet, int made)
{
    const int64_t deep{int64_t{made} + wallet.chain().getBitAssetsRevealDepth()};
    return static_cast<int>(std::max<int64_t>(wallet.chain().getBitAssetsHeight(), deep));
}

/**
 * A logo given to an RPC: base64 of a PNG or JPEG the rules take (bitassets/logo.h), checked here so that
 * the wallet never pays for a transaction the rules refuse. An empty string, if `allow_empty`: none.
 */
std::vector<unsigned char> ParseLogo(const UniValue& value, bool allow_empty)
{
    const std::string& text{value.get_str()};
    // Base64 of the largest logo, and no more: nothing larger is decoded.
    if (text.size() > (bitassets::MAX_LOGO_BYTES + 2) / 3 * 4) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this logo (bad-ba-logo-oversize): a logo is at most %u bytes", bitassets::MAX_LOGO_BYTES));
    const auto bytes{DecodeBase64(text)};
    if (!bytes) throw JSONRPCError(RPC_INVALID_PARAMETER, "The logo is not base64");
    if (bytes->empty()) {
        if (!allow_empty) throw JSONRPCError(RPC_INVALID_PARAMETER, "The logo is empty");
        return {};
    }
    std::string reason;
    if (!bitassets::CheckLogo(*bytes, reason)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this logo (%s): a PNG or a JPEG of exactly %ux%u pixels, at most %u bytes, "
                                                            "without metadata or chunks other than the image's (see doc/bitassets.md, Logos)",
                                                            reason, bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES));
    }
    return *bytes;
}

/** The logos of assets are in the rules from the next block, or a logo is refused here. */
void NeedLogos(CWallet& wallet)
{
    if (!wallet.chain().bitAssetsLogosActive()) throw JSONRPCError(RPC_WALLET_ERROR, "The logos of assets are not in the rules of the chain yet (logo_height in getbitassetsinfo)");
}

/**
 * The name of a new asset as the wallet reserves and registers it: from ticker_height a ticker, the
 * letters of what is given uppercased ("gold" is GOLD); before it, as given.
 */
std::string NewAssetName(CWallet& wallet, const std::string& given)
{
    if (!wallet.chain().bitAssetsTickersActive()) return given;
    const std::string name{ToUpper(given)};
    if (!bitassets::IsTicker(name)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The name of a new asset is a ticker (from ticker_height in getbitassetsinfo): %u to %u characters, "
                                                            "letters A to Z and digits 0 to 9 only (\"%s\" is not one). A longer name goes in the display name",
                                                            bitassets::MIN_TICKER_SIZE, bitassets::MAX_TICKER_SIZE, name));
    }
    return name;
}

/** Tickers and display names are in the rules from the next block, or a display name is refused here. */
void NeedTickers(CWallet& wallet)
{
    if (!wallet.chain().bitAssetsTickersActive()) throw JSONRPCError(RPC_WALLET_ERROR, "Display names of assets are not in the rules of the chain yet (ticker_height in getbitassetsinfo)");
}

/** A display name given to an RPC, checked as the rules check it; "" if `allow_empty` (none). */
std::string ParseDisplayName(const UniValue& value, bool allow_empty)
{
    const std::string& text{value.get_str()};
    if (text.empty()) {
        if (!allow_empty) throw JSONRPCError(RPC_INVALID_PARAMETER, "The display name is empty");
        return text;
    }
    if (const auto why{bitassets::WhyNotDisplayName(text)}) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this display name (bad-ba-display-name): %s. A display name is 1 to %u bytes of UTF-8 in NFC, "
                                                            "printable, without a space at either end", *why, bitassets::MAX_DISPLAY_NAME_SIZE));
    }
    return text;
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

//! bitassets::NameMimicsNumber: refused by the wallet, not relayed by the mempool.
constexpr const char* NAME_MIMICS_NUMBER{"The name holds '#', '(', ')' or four digits, a dash and four digits: shown with its number, it would read as the number of another asset"};

RPCMethod reserveasset()
{
    return RPCMethod{
        "reserveasset",
        "Reserve the name of an asset: the first of two steps to register it. The reservation commits to the name\n"
        "without showing it, so that nobody can see it coming and take it first; registerasset registers it once\n"
        "the reservation is in a block." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, strprintf("The name: 1 to %u printable characters; from ticker_height (getbitassetsinfo) a ticker, %u to %u letters A-Z and digits 0-9 (letters are uppercased)",
                                                                          bitassets::MAX_NAME_TEXT_SIZE, bitassets::MIN_TICKER_SIZE, bitassets::MAX_TICKER_SIZE)},
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
    const std::string name{NewAssetName(*pwallet, request.params[0].get_str())};
    if (!bitassets::IsAssetName(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A name is 1 to %u printable characters, without a space at either end", bitassets::MAX_NAME_TEXT_SIZE));
    if (bitassets::ReadsAsAnotherAsset(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name reads as another asset (CHN, a number such as 1739-0029, or \"0x\" and a hash)");
    if (bitassets::NameMimicsNumber(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, NAME_MIMICS_NUMBER);
    const AssetId asset{bitassets::HashName(name)};
    if (pwallet->chain().getBitAsset(asset)) throw JSONRPCError(RPC_INVALID_PARAMETER, "An asset of this name is registered");
    const CTxDestination dest{NewDestination(*pwallet, ReservationLabel(name))};
    Token reservation{UnitToken(Token::Kind::RESERVATION, uint256{})};
    const uint256 nonce{ReservationNonce(*pwallet, dest, asset)};
    // The commitment binds the reservation to the script of its output: a copy of it, made by
    // someone who saw it, can never reveal the name before this one.
    const uint256 commitment{bitassets::ReservationCommitment(asset, nonce, GetScriptForDestination(dest))};
    const CTransactionRef tx{Send(*pwallet, bitassets::Reserve{commitment}, {{dest, reservation}}, {})};
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
        "Register an asset reserved with reserveasset, once the reservation is deep enough: create its initial supply\n"
        "and its control coin, which mints more and changes its data. A registration reveals a reservation only in a\n"
        "block some blocks after the one that made it (revealdepth in getbitassetsinfo; listmyassets says from which\n"
        "block each reservation registers)." + HELP_REQUIRING_PASSPHRASE,
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name"},
            {"supply", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The initial supply, in whole assets (0 for none yet)"},
            {"decimals", RPCArg::Type::NUM, RPCArg::Default{0}, strprintf("The decimals it is shown with, at most %u: with 2, 1 is 100 units", bitassets::MAX_DECIMALS)},
            {"data", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "Its data", bitassets::DataArgs()},
            {"public", RPCArg::Type::BOOL, RPCArg::Default{true}, "Publish the name, so that it is listed and found by name. Otherwise only its hash is on the chain"},
            {"logo", RPCArg::Type::STR, RPCArg::Optional::OMITTED, strprintf("Its logo, in base64: a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata (see setassetlogo)", bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES)},
            {"display_name", RPCArg::Type::STR, RPCArg::Optional::OMITTED, strprintf("From ticker_height: its display name, shown with the ticker; not unique (see setassetdisplayname). 1 to %u bytes of UTF-8", bitassets::MAX_DISPLAY_NAME_SIZE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "asset", "The hash of the name"},
            {RPCResult::Type::STR, "name", "The name (the ticker) registered"},
        }},
        RPCExamples{HelpExampleCli("registerasset", "\"GOLD\" 1000000 2 '{\"info\": \"One gram of gold\"}'") +
                    HelpExampleCli("-named registerasset", "name=GOLD supply=1000 display_name=\"Gold Bar Token, 1 g\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const bool tickers{pwallet->chain().bitAssetsTickersActive()};
    const std::string name{NewAssetName(*pwallet, request.params[0].get_str())};
    const AssetId asset{bitassets::HashName(name)};
    if (pwallet->chain().getBitAsset(asset)) throw JSONRPCError(RPC_INVALID_PARAMETER, "An asset of this name is registered");
    if (tickers && !request.params[4].isNull() && !request.params[4].get_bool()) throw JSONRPCError(RPC_INVALID_PARAMETER, "From ticker_height the name of a new asset (its ticker) is published: public cannot be false");
    if (bitassets::ReadsAsAnotherAsset(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name reads as another asset (CHN, a number such as 1739-0029, or \"0x\" and a hash)");
    const int decimals{request.params[2].isNull() ? 0 : request.params[2].getInt<int>()};
    if (decimals < 0 || decimals > bitassets::MAX_DECIMALS) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The decimals are from 0 to %u", bitassets::MAX_DECIMALS));
    const uint64_t supply{bitassets::ParseUnits(request.params[1], decimals, /*allow_zero=*/true)};
    // A registration sent before that the mempool dropped holds the reservation: abandoned, it frees it.
    AbandonDroppedRegistration(*pwallet, asset);
    const auto reservation{FindReservation(*pwallet, asset)};
    if (!reservation) throw JSONRPCError(RPC_INVALID_PARAMETER, "This wallet has no reservation of the name in a block (see reserveasset)");
    // Not before the rules let it be revealed: the registration would not be taken.
    if (const auto made{pwallet->chain().getBitAssetsReservationHeight(reservation->id)}) {
        const int next{pwallet->chain().getBitAssetsHeight()};
        const int from{RevealHeight(*pwallet, *made)};
        if (from > next) {
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("The reservation is too recent: a registration reveals it only in a block %d blocks after the one that made it (%d), so that whoever makes a block cannot take the name first. Try again from block %d, in %d blocks",
                                                           pwallet->chain().getBitAssetsRevealDepth(), *made, from, from - next));
        }
    }
    bitassets::Register reg;
    reg.name = asset;
    reg.nonce = reservation->nonce;
    reg.supply = supply;
    reg.decimals = decimals;
    if (!request.params[3].isNull()) reg.data = bitassets::ParseData(request.params[3]);
    if (!request.params[5].isNull() && !request.params[5].get_str().empty()) {
        NeedLogos(*pwallet);
        reg.logo = ParseLogo(request.params[5], /*allow_empty=*/false);
    }
    if (!request.params[6].isNull() && !request.params[6].get_str().empty()) {
        NeedTickers(*pwallet);
        reg.display_name = ParseDisplayName(request.params[6], /*allow_empty=*/false);
    }
    if (request.params[4].isNull() || request.params[4].get_bool()) {
        // Published, it would read as another asset's number where names are shown with theirs.
        if (bitassets::NameMimicsNumber(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, std::string{NAME_MIMICS_NUMBER} + " (register it unpublished: public false)");
        reg.text = name;
    }
    std::vector<Out> outs{{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset)}};
    if (supply > 0) outs.push_back({ChangeDestination(*pwallet), AssetToken(asset, supply)});
    const CTransactionRef tx{Send(*pwallet, reg, outs, {reservation->outpoint})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("asset", asset.GetHex());
    result.pushKV("name", name);
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
    const auto token{pwallet->chain().getBitAssetsToken(reservation->outpoint)};
    bitassets::Burn burn;
    burn.tokens.push_back(*token);
    return TxResult(Send(*pwallet, burn, {}, {reservation->outpoint}));
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
                {RPCResult::Type::NUM, "height", /*optional=*/true, "The block that made it"},
                {RPCResult::Type::NUM, "registers_from", /*optional=*/true, "The first block that may take its registration (see registerasset), if reserved with this wallet"},
                {RPCResult::Type::NUM, "wait", /*optional=*/true, "How many blocks until then: 0 if it can be registered now"},
            }}}},
            {RPCResult::Type::ARR, "liquidity", "Shares of pools", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "pool", "The pool"},
                {RPCResult::Type::STR, "asset_a", ""},
                {RPCResult::Type::STR, "asset_b", ""},
                {RPCResult::Type::STR_HEX, "asset_a_id", "The hash of the first asset (null for CHN)"},
                {RPCResult::Type::STR_HEX, "asset_b_id", "The hash of the second"},
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
            const Txid id{Txid::FromUint256(owned.token.id)};
            const auto made{pwallet->chain().getBitAssetsReservationHeight(id)};
            std::optional<int> from;
            if (label && label->starts_with("bitasset:")) {
                const std::string name{label->substr(9)};
                obj.pushKV("name", name);
                const AssetId asset{bitassets::HashName(name)};
                taken = pwallet->chain().getBitAsset(asset).has_value();
                const auto commitment{pwallet->chain().getBitAssetsReservation(id)};
                const auto mine{commitment ? ReservationOf(*pwallet, owned.dest, *commitment, asset) : std::nullopt};
                if (made && mine) from = RevealHeight(*pwallet, *made);
            }
            obj.pushKV("taken", taken);
            if (made) obj.pushKV("height", *made);
            if (from) {
                obj.pushKV("registers_from", *from);
                obj.pushKV("wait", std::max(0, *from - pwallet->chain().getBitAssetsHeight()));
            }
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
        obj.pushKV("asset_a_id", (flip ? pool->asset1 : pool->asset0).GetHex());
        obj.pushKV("asset_b_id", (flip ? pool->asset0 : pool->asset1).GetHex());
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

RPCMethod searchmyassets()
{
    return RPCMethod{
        "searchmyassets",
        "searchassets among the assets this wallet holds or controls, in blocks: the same arguments, filters, sorts, cursor and result,\n"
        "by the node's search index. Bounded by the wallet's own transactions.",
        bitassets::SearchAssetsArgs(),
        bitassets::SearchAssetsResult(),
        RPCExamples{HelpExampleCli("searchmyassets", "") + HelpExampleCli("-rpcwallet=w searchmyassets", "\"gold\" '{}' supply")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    // Its asset coins and control coins, as the chain has them now.
    std::set<AssetId> mine;
    for (const OwnedToken& t : OwnedTokens(const_cast<CWallet&>(*pwallet))) {
        if (t.token.kind == Token::Kind::ASSET || t.token.kind == Token::Kind::CONTROL) mine.insert(t.token.id);
    }
    UniValue params(UniValue::VARR);
    for (size_t i{0}; i < 5; ++i) params.push_back(i < request.params.size() ? request.params[i] : UniValue{});
    return pwallet->chain().searchBitAssets(params, std::vector<AssetId>(mine.begin(), mine.end()));
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

RPCMethod setassetlogo()
{
    return RPCMethod{
        "setassetlogo",
        strprintf("Set the logo of an asset this wallet controls, or remove it (an empty logo). The logo goes whole into the transaction:\n"
                  "a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata (no text, Exif, ICC profile or comment) and\n"
                  "nothing after its end; the rules check its structure, and refuse anything else. It replaces the logo before.", bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES) +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"logo", RPCArg::Type::STR, RPCArg::Optional::NO, "The logo, in base64; \"\" removes it"},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
            {
                {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"the wallet's fee rate"}, "The fee rate, in " + CURRENCY_ATOM + "/vB"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR_HEX, "hash", /*optional=*/true, "SHA-256 of the logo (none when removed)"},
            {RPCResult::Type::NUM, "size", "Its size in bytes (0 when removed)"},
            {RPCResult::Type::STR_AMOUNT, "fee", "The network fee paid"},
        }},
        RPCExamples{HelpExampleCli("setassetlogo", "\"GOLD\" \"iVBORw0KGgo...\"") + HelpExampleCli("setassetlogo", "\"GOLD\" \"\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN has no logo here");
    if (asset.record->fixed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The data of this asset is fixed: its control coin was burned");
    NeedLogos(*pwallet);
    const std::vector<unsigned char> logo{ParseLogo(request.params[1], /*allow_empty=*/true)};
    if (logo.empty() && !pwallet->chain().getBitAssetLogo(asset.id)) throw JSONRPCError(RPC_INVALID_PARAMETER, "This asset has no logo to remove");
    std::optional<CFeeRate> fee_rate;
    if (!request.params[2].isNull()) {
        const UniValue& rate{request.params[2].find_value("fee_rate")};
        if (!rate.isNull()) fee_rate = CFeeRate{AmountFromValue(rate, /*decimals=*/3)};
    }
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    const CTransactionRef tx{Send(*pwallet, bitassets::SetLogo{asset.id, logo}, {{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset.id)}}, inputs, 0, fee_rate)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    if (!logo.empty()) {
        uint256 hash;
        CSHA256().Write(logo.data(), logo.size()).Finalize(hash.data());
        result.pushKV("hash", HexStr(hash));
    }
    result.pushKV("size", static_cast<uint64_t>(logo.size()));
    CAmount fee{0};
    {
        LOCK(pwallet->cs_wallet);
        CAmount in{0}, out{tx->GetValueOut()};
        for (const CTxIn& txin : tx->vin) {
            if (const CWalletTx* prev{pwallet->GetWalletTx(txin.prevout.hash)}) in += prev->GetTx()->vout[txin.prevout.n].nValue;
        }
        fee = in - out;
    }
    result.pushKV("fee", ValueFromAmount(fee));
    return result;
},
    };
}

RPCMethod setassetdisplayname()
{
    return RPCMethod{
        "setassetdisplayname",
        strprintf("Set the display name of an asset this wallet controls, or remove it (an empty one). The display name is shown with\n"
                  "the asset's ticker (its name, which stays as it is) and found by searchassets; it is not unique: other assets may have\n"
                  "the same. 1 to %u bytes of UTF-8 in NFC, printable, no space at either end, no control, format (bidirectional,\n"
                  "zero-width) or combining character. It replaces the one before. From ticker_height (getbitassetsinfo).", bitassets::MAX_DISPLAY_NAME_SIZE) +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"display_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The display name; \"\" removes it"},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
            {
                {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"the wallet's fee rate"}, "The fee rate, in " + CURRENCY_ATOM + "/vB"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR, "display_name", /*optional=*/true, "The display name set (none when removed)"},
            {RPCResult::Type::STR_AMOUNT, "fee", "The network fee paid"},
        }},
        RPCExamples{HelpExampleCli("setassetdisplayname", "\"GOLD\" \"Gold Bar Token, 1 g\"") + HelpExampleCli("setassetdisplayname", "\"GOLD\" \"\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo asset{ParseAsset(*pwallet, request.params[0])};
    if (asset.id.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN has no display name here");
    if (asset.record->fixed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The data of this asset is fixed: its control coin was burned");
    NeedTickers(*pwallet);
    const std::string display_name{ParseDisplayName(request.params[1], /*allow_empty=*/true)};
    if (display_name.empty() && !pwallet->chain().getBitAssetDisplayName(asset.id)) throw JSONRPCError(RPC_INVALID_PARAMETER, "This asset has no display name to remove");
    std::optional<CFeeRate> fee_rate;
    if (!request.params[2].isNull()) {
        const UniValue& rate{request.params[2].find_value("fee_rate")};
        if (!rate.isNull()) fee_rate = CFeeRate{AmountFromValue(rate, /*decimals=*/3)};
    }
    const auto [inputs, total]{Select(*pwallet, Token::Kind::CONTROL, asset.id, 1, "the control coin of " + asset.label)};
    const CTransactionRef tx{Send(*pwallet, bitassets::SetDisplayName{asset.id, display_name}, {{ChangeDestination(*pwallet), UnitToken(Token::Kind::CONTROL, asset.id)}}, inputs, 0, fee_rate)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    if (!display_name.empty()) result.pushKV("display_name", display_name);
    CAmount fee{0};
    {
        LOCK(pwallet->cs_wallet);
        CAmount in{0}, out{tx->GetValueOut()};
        for (const CTxIn& txin : tx->vin) {
            if (const CWalletTx* prev{pwallet->GetWalletTx(txin.prevout.hash)}) in += prev->GetTx()->vout[txin.prevout.n].nValue;
        }
        fee = in - out;
    }
    result.pushKV("fee", ValueFromAmount(fee));
    return result;
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
            {"min_out", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The least that should come out, as the caller showed it (its own quote less the slippage): if the pool moved since, so that the quote now, less the slippage, is below it, the trade is refused rather than signed for less"},
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
    if (out.id.IsNull() && quote < bitassets::MIN_CHN_PAYOUT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little: the least CHN paid out is %s", FormatMoney(bitassets::MIN_CHN_PAYOUT)));
    }
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
    // The least the caller confirmed: never signed for less (the pool may have moved since its quote).
    if (!request.params[5].isNull()) {
        const uint64_t confirmed{bitassets::ParseUnits(request.params[5], out.decimals)};
        if (min_out < confirmed) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The pool moved: the trade now gives at least %s %s, less than the %s %s asked for (min_out). Quote it again",
                                                                bitassets::FormatUnits(min_out, out.decimals), out.label, bitassets::FormatUnits(confirmed, out.decimals), out.label));
        }
    }
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
        "set. Into a pool there is, the second amount is worked out from the first at the pool's price, unless given.\n"
        "A pool nobody provides liquidity to any more (abandoned) is reopened as a new pool is made: both amounts are\n"
        "needed, and they set its price (what it holds is the dust it keeps for good, at a price anyone could set).\n"
        "The amounts given for an abandoned pool are what it reopens with: the dust it\n"
        "holds counts toward them, and the rest goes in. Should someone else reopen or move the pool first, the\n"
        "deposit goes in at the pool's price then, what its price does not take comes back, and it is refused if it\n"
        "gives fewer shares than quoted, less the slippage." + HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount_a", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much of it"},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"amount_b", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "How much of it: needed for a new pool, or an abandoned one. Into a pool someone provides liquidity to, the amounts are the most to put in: only what the pool's price takes goes in"},
            {"slippage", RPCArg::Type::NUM, RPCArg::Default{1}, "How many fewer shares than quoted, in percent, it may give if the pool moves first"},
            {"sets_price", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "What the caller expects: true, that the amounts set the price (a new pool, or an abandoned one), and refused if someone provides liquidity to the pool by now; false, that they go in at the pool's price, and refused if they would set it. Left out: either"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::NUM, "amount_a", "What goes in of the first asset if nothing moves the pool first (at the pool's price, into a pool someone provides liquidity to; less the dust, into an abandoned one)"},
            {RPCResult::Type::NUM, "amount_b", "And of the second"},
            {RPCResult::Type::NUM, "shares", "The shares it gives if nothing moves the pool first"},
            {RPCResult::Type::BOOL, "sets_price", "Whether the amounts set the pool's price: a new pool, or an abandoned one reopened"},
        }},
        RPCExamples{HelpExampleCli("addliquidity", "\"GOLD\" 100 \"CHN\" 5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request)};
    if (!pwallet) return UniValue::VNULL;
    const AssetInfo a{ParseAsset(*pwallet, request.params[0])};
    const AssetInfo b{ParseAsset(*pwallet, request.params[2])};
    if (a.id == b.id) throw JSONRPCError(RPC_INVALID_PARAMETER, "Two different assets make a pool");
    uint64_t amount_a{bitassets::ParseUnits(request.params[1], a.decimals)};
    const uint64_t slippage{Slippage(request.params[4], 1)};
    const auto pool{pwallet->chain().getBitAssetsPool(bitassets::PoolId(a.id, b.id))};
    // A new pool, or one nobody provides liquidity to: the amounts set its price. What an abandoned
    // pool holds is the dust every pool keeps for good, at whatever price the last trade or the last
    // provider left it: anyone can set it, and nobody should deposit at it unawares.
    const bool sets_price{!pool || bitassets::amm::Abandoned(*pool)};
    // What the caller showed: a deposit that was to set the price never goes in at someone else's.
    if (!request.params[5].isNull() && request.params[5].get_bool() != sets_price) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, sets_price ? "Nobody provides liquidity to this pool (any more): the amounts would set its price, which was not expected (sets_price)"
                                                             : "Someone opened this pool meanwhile: the amounts would go in at its price, not set it (sets_price). Look at its price first");
    }
    uint64_t amount_b;
    if (!request.params[3].isNull()) {
        amount_b = bitassets::ParseUnits(request.params[3], b.decimals);
    } else {
        if (!pool || pool->shares == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool yet: give both amounts, which set its price");
        if (sets_price) throw JSONRPCError(RPC_INVALID_PARAMETER, "Nobody provides liquidity to this pool any more: its price is what anyone left it at. Give both amounts, which set its price, as for a new pool");
        const bool a_first{pool->asset0 == a.id};
        const uint64_t ra{a_first ? pool->reserve0 : pool->reserve1}, rb{a_first ? pool->reserve1 : pool->reserve0};
        // At the pool's price, rounded up so that the first amount is the one that counts.
        const unsigned __int128 b_needed{(static_cast<unsigned __int128>(amount_a) * rb + ra - 1) / ra};
        if (b_needed == 0 || b_needed > bitassets::MAX_AMOUNT) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little or too much for this pool");
        amount_b = static_cast<uint64_t>(b_needed);
    }
    bitassets::Pool current;
    if (pool) {
        current = *pool;
    } else {
        current.asset0 = std::min(a.id, b.id);
        current.asset1 = std::max(a.id, b.id);
    }
    const bool a_first{current.asset0 == a.id};
    // What the transaction offers, what goes in of it if nothing moves the pool first, and the shares.
    uint64_t offer_a{amount_a}, offer_b{amount_b};
    uint64_t shares{0};
    // A pool everyone left holds dust, which the deposit that reopens it merges in: the amounts
    // given are what it reopens with, at their price, and the dust counts toward them.
    if (pool && sets_price) {
        const uint64_t dust_a{a_first ? current.reserve0 : current.reserve1}, dust_b{a_first ? current.reserve1 : current.reserve0};
        if (amount_a <= dust_a || amount_b <= dust_b) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("This pool holds %s %s and %s %s, left by its last providers, which count toward what it reopens with: give more than that of each",
                                                                bitassets::FormatUnits(dust_a, a.decimals), a.label, bitassets::FormatUnits(dust_b, b.decimals), b.label));
        }
        offer_a = amount_a - dust_a;
        offer_b = amount_b - dust_b;
    }
    const auto deposit{bitassets::amm::Provide(current, a_first ? offer_a : offer_b, a_first ? offer_b : offer_a)};
    if (deposit) {
        shares = deposit->shares;
        // Into a pool someone provides liquidity to, only what its price takes is offered: nothing
        // comes back unless the pool moves first (then the excess does, by the rules).
        if (!deposit->opens) {
            offer_a = a_first ? deposit->take0 : deposit->take1;
            offer_b = a_first ? deposit->take1 : deposit->take0;
        }
    }
    if (shares == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it makes no shares (a new pool needs the product of the amounts, in units, above a million)");
    // Opening (or reopening an abandoned) pool takes a deposit worth trading against: what the pool
    // holds after it (its dust merged in) is what the amounts give.
    if (bitassets::amm::Abandoned(current)) {
        const uint64_t chn{a.id.IsNull() ? amount_a : b.id.IsNull() ? amount_b : bitassets::MIN_OPEN_CHN};
        if (shares < bitassets::MIN_OPEN_SHARES || chn < bitassets::MIN_OPEN_CHN) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little to open a pool: it takes at least %s CHN on the CHN side, and amounts whose product, in units, is above %u", FormatMoney(bitassets::MIN_OPEN_CHN), (bitassets::MIN_OPEN_SHARES + bitassets::MIN_LIQUIDITY) * (bitassets::MIN_OPEN_SHARES + bitassets::MIN_LIQUIDITY)));
        }
    }
    std::vector<COutPoint> inputs;
    std::vector<Out> outs;
    CAmount chn_in{0};
    PayIn(*pwallet, a, offer_a, inputs, outs, chn_in);
    PayIn(*pwallet, b, offer_b, inputs, outs, chn_in);
    // The shares, then what comes back (asset coins, or CHN the coinbase pays to its address),
    // should the pool move first.
    outs.push_back({ChangeDestination(*pwallet), std::nullopt});
    outs.push_back({ChangeDestination(*pwallet), std::nullopt});
    const CTransactionRef tx{Send(*pwallet, bitassets::AddLiquidity{a.id, b.id, offer_a, offer_b, std::max<uint64_t>(1, LessSlippage(shares, slippage))}, outs, inputs, chn_in)};
    amount_a = offer_a;
    amount_b = offer_b;
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount_a", bitassets::AmountToJSON(amount_a, a.decimals));
    result.pushKV("amount_b", bitassets::AmountToJSON(amount_b, b.decimals));
    result.pushKV("shares", shares);
    result.pushKV("sets_price", sets_price);
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
    // A side that rounds to nothing, or to CHN dust (which stays in the pool), is left out.
    const auto [out0, out1]{bitassets::amm::WithdrawPaid(*pool, shares)};
    const bool a_first{pool->asset0 == a.id};
    const uint64_t out_a{a_first ? out0 : out1}, out_b{a_first ? out1 : out0};
    if (out_a == 0 && out_b == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too few shares: they take nothing out of the pool");
    const auto [inputs, total]{Select(*pwallet, Token::Kind::LP, id, shares, "shares of this pool")};
    std::vector<Out> outs;
    if (total > shares) {
        Token change{UnitToken(Token::Kind::LP, id)};
        change.amount = total - shares;
        outs.push_back({ChangeDestination(*pwallet), change});
    }
    std::vector<AssetId> results;
    if (out_a > 0) results.push_back(a.id);
    if (out_b > 0) results.push_back(b.id);
    ResultOuts(*pwallet, results, outs);
    // A side left out asks for nothing; one that gives something, for at least 1 (it must not be left out).
    const auto least{[&](uint64_t out) { return out == 0 ? uint64_t{0} : std::max<uint64_t>(1, LessSlippage(out, slippage)); }};
    bitassets::RemoveLiquidity remove{a.id, b.id, shares, least(out_a), least(out_b), ChnTo(*pwallet, results)};
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
            {"start_in", RPCArg::Type::NUM, RPCArg::Default{1}, strprintf("When it starts, in blocks after the one that takes the transaction: 1 is the block after that; at most %u", bitassets::MAX_AUCTION_DELAY)},
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
    if (create.base.IsNull() && create.base_amount < bitassets::MIN_CHN_PAYOUT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little: an auction sells %s CHN at least (the least CHN paid out)", FormatMoney(bitassets::MIN_CHN_PAYOUT)));
    }
    create.duration = request.params[5].getInt<int>();
    if (create.duration < 1 || create.duration > bitassets::MAX_AUCTION_DURATION) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The duration is 1 to %u blocks", bitassets::MAX_AUCTION_DURATION));
    const int start_in{request.params[6].isNull() ? 1 : request.params[6].getInt<int>()};
    if (start_in < 1) throw JSONRPCError(RPC_INVALID_PARAMETER, "It starts a block after the one that takes the transaction, at the earliest");
    if (start_in > bitassets::MAX_AUCTION_DELAY) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("It starts within %u blocks", bitassets::MAX_AUCTION_DELAY));
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
        "takes the bid (at least what it buys in the next block: the price only falls). A bid that buys all that is left\n"
        "pays what that costs in the block that takes it, and gets the rest back." + HELP_REQUIRING_PASSPHRASE,
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "What to pay; with buy_all, the most to pay (refused if all that is left costs more)"},
            {"buy_all", RPCArg::Type::BOOL, RPCArg::Default{false}, "Pay what buys all that is left at the price of the next block, and no more (cost_of_remaining in getauction); taken in a later block, it pays what all that is left costs then, and the rest comes back"},
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
    // An asset of it retired since, or registered again: not what the auction trades.
    for (const auto& [asset, registration] : {std::pair{auction->base, auction->base_registration}, std::pair{auction->quote, auction->quote_registration}}) {
        if (asset.IsNull()) continue;
        const auto record{pwallet->chain().getBitAsset(asset)};
        if (!record || record->registration != registration) throw JSONRPCError(RPC_INVALID_PARAMETER, "An asset of this auction was retired since it was made: it takes no bids");
    }
    const uint8_t dq{DecimalsOf(*pwallet, auction->quote)}, db{DecimalsOf(*pwallet, auction->base)};
    // The least that buys all that is left: what the window shows, and the most a bid ever pays.
    const uint64_t cost{auction->CostOfRemaining(height)};
    uint64_t amount;
    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        // Not the most that buys no more than what is left: that overpays by up to the price of a
        // unit (with 0 decimals, nearly twice the price for the last one).
        amount = cost;
        if (amount == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Nothing is left to buy");
        if (!request.params[1].isNull() && amount > bitassets::ParseUnits(request.params[1], dq)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("All that is left costs %s now, more than the most given", bitassets::FormatUnits(amount, dq)));
        }
    } else {
        if (request.params[1].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Give an amount, or buy_all");
        amount = bitassets::ParseUnits(request.params[1], dq);
    }
    uint64_t buys{auction->BuysAt(height, amount)};
    if (buys == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it buys nothing at the price of the next block");
    // An amount that buys all that is left pays what that costs, not more. Bought in a later block,
    // where the price is lower, it still buys all that is left, and the rest comes back (the bid says
    // where: below).
    if (buys >= auction->remaining) {
        amount = std::min(amount, cost);
        buys = auction->remaining;
    }
    if (auction->base.IsNull() && buys < bitassets::MIN_CHN_PAYOUT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little: the least CHN paid out is %s", FormatMoney(bitassets::MIN_CHN_PAYOUT)));
    }
    if (auction->base.IsNull() && buys < auction->remaining && auction->remaining - buys < bitassets::MIN_CHN_PAYOUT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("It would leave less than %s CHN, which nobody could buy: buy all that is left, or less", FormatMoney(bitassets::MIN_CHN_PAYOUT)));
    }
    if (auction->quote.IsNull() && amount < bitassets::MIN_CHN_PAYOUT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Too little: a bid pays %s CHN at least (the least CHN paid out)", FormatMoney(bitassets::MIN_CHN_PAYOUT)));
    }
    const AssetInfo quote{auction->quote, std::nullopt, dq, LabelOfAsset(*pwallet, auction->quote)};
    std::vector<COutPoint> inputs;
    std::vector<Out> outs;
    CAmount chn_in{0};
    PayIn(*pwallet, quote, amount, inputs, outs, chn_in);
    // What it buys, then what comes back of what it pays, should it buy all that is left in a block
    // where that costs less (by the coinbase, for CHN).
    ResultOuts(*pwallet, {auction->base, auction->quote}, outs);
    const CTransactionRef tx{Send(*pwallet, bitassets::Bid{id, amount, buys, ChnTo(*pwallet, {auction->base, auction->quote})}, outs, inputs, chn_in)};
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
    // CHN dust is not paid out (it is burned).
    const auto paid{[&](const AssetId& asset, uint64_t amount) { return amount > 0 && !(asset.IsNull() && amount < bitassets::MIN_CHN_PAYOUT); }};
    std::vector<AssetId> results;
    if (paid(auction->base, auction->remaining)) results.push_back(auction->base);
    if (paid(auction->quote, auction->proceeds)) results.push_back(auction->quote);
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

RPCMethod listassettransactions()
{
    const std::vector<RPCResult> party{
        {RPCResult::Type::STR, "address", "An address, \"unknown\" where the outputs a transaction spent cannot be read (a pruned block)"},
        {RPCResult::Type::BOOL, "mine", "Whether it is this wallet's"},
    };
    return RPCMethod{
        "listassettransactions",
        "The asset transactions of this wallet, newest first, one entry per asset each moves: what kind of move it is, the\n"
        "asset and amount (signed), the network fee the wallet paid, and who the other side is: the addresses an asset was\n"
        "sent to, or the addresses that paid a transaction the wallet received an asset by (the outputs it spent, from the\n"
        "undo data of its block or the mempool: no index; \"unknown\" when they cannot be read).",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many entries, at most 1000"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
            {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only the entries of this asset (" + std::string{bitassets::ASSET_ARG_HELP} + ")"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM_TIME, "time", "When the wallet saw it"},
            {RPCResult::Type::NUM, "confirmations", ""},
            {RPCResult::Type::STR_HEX, "blockhash", /*optional=*/true, ""},
            {RPCResult::Type::NUM, "blockheight", /*optional=*/true, ""},
            {RPCResult::Type::STR, "category", "send, receive, register, mint, burn, swap, pool_add, pool_remove, auction, bid, collect, logo, display_name, update, control, reserve, release"},
            {RPCResult::Type::STR_HEX, "asset", /*optional=*/true, "The asset (none for a reservation)"},
            {RPCResult::Type::STR, "label", /*optional=*/true, "Its label"},
            {RPCResult::Type::STR, "seq", /*optional=*/true, "Its number"},
            {RPCResult::Type::NUM, "decimals", /*optional=*/true, ""},
            {RPCResult::Type::NUM, "amount", /*optional=*/true, "How much of it moved, in whole assets (exact)"},
            {RPCResult::Type::NUM, "sign", "+1 into the wallet, -1 out of it, 0 neither (a change of its data, a logo)"},
            {RPCResult::Type::BOOL, "at_least", /*optional=*/true, "The amount is the least the transaction asked for (a result not read)"},
            {RPCResult::Type::BOOL, "at_most", /*optional=*/true, "The amount is the most it offered (a deposit into a pool)"},
            {RPCResult::Type::STR_AMOUNT, "fee", /*optional=*/true, "The network fee, when the wallet paid all of it"},
            {RPCResult::Type::ARR, "to", "Where the asset went (outputs of others first)", {{RPCResult::Type::OBJ, "", "", party}}},
            {RPCResult::Type::ARR, "from", "Who paid the transaction, for what the wallet received", {{RPCResult::Type::OBJ, "", "", party}}},
            {RPCResult::Type::BOOL, "abandoned", ""},
        }}}},
        RPCExamples{HelpExampleCli("listassettransactions", "") + HelpExampleCli("listassettransactions", "20 0 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    // It reads, and needs no key: a locked wallet answers too.
    const std::shared_ptr<CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    const int count{request.params[0].isNull() ? 100 : request.params[0].getInt<int>()};
    const int skip{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (count < 0 || count > 1000 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count: 0 to 1000; skip: 0 or more");
    std::optional<AssetId> only;
    if (!request.params[2].isNull()) only = ParseAsset(*pwallet, request.params[2]).id;

    struct Party {
        std::string address;
        bool mine;
    };
    struct Row {
        int64_t time;
        Txid txid;
        int confirmations;
        std::optional<std::pair<uint256, int>> block;
        std::string category;
        std::optional<AssetId> asset;
        uint64_t amount{0};
        bool has_amount{false};
        int sign{0};
        bool at_least{false}, at_most{false};
        std::optional<CAmount> fee;
        std::vector<Party> to, from;
        bool abandoned;
    };
    struct Seen {
        CTransactionRef tx;
        int64_t time;
        int confirmations;
        std::optional<std::pair<uint256, int>> block;
        bool any_mine, all_mine;
        CAmount debit;
        std::vector<bool> owned;
        bool abandoned;
        Marker marker;
    };
    std::vector<Seen> seen;
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            const CTransactionRef tx{wtx.GetTx()};
            auto marker{bitassets::GetMarker(*tx)};
            if (!marker) continue;
            const int depth{pwallet->GetTxDepthInMainChain(wtx)};
            if (depth < 0) continue; // conflicted
            Seen s{tx, wtx.GetTxTime(), depth, std::nullopt, false, AllInputsMine(*pwallet, *tx), 0, {}, wtx.isAbandoned(), std::move(*marker)};
            if (const auto* conf{wtx.state<TxStateConfirmed>()}) s.block = std::make_pair(conf->confirmed_block_hash, conf->confirmed_block_height);
            for (const CTxIn& in : tx->vin) s.any_mine |= InputIsMine(*pwallet, in);
            if (s.all_mine) s.debit = CachedTxGetDebit(*pwallet, wtx, /*avoid_reuse=*/false);
            for (const CTxOut& out : tx->vout) s.owned.push_back(pwallet->IsMine(out));
            seen.push_back(std::move(s));
        }
    }
    const auto address{[&](const CScript& script) {
        CTxDestination dest;
        return ExtractDestination(script, dest) ? EncodeDestination(dest) : std::string{"script " + HexStr(script).substr(0, 16)};
    }};
    std::vector<Row> rows;
    for (const Seen& s : seen) {
        const CTransaction& tx{*s.tx};
        const Txid txid{tx.GetHash()};
        // What each output carries: the marker says it for the outputs it names a token for; a result output, the state
        // says while it is not spent.
        std::map<uint32_t, Token> tokens;
        for (const bitassets::MarkerOutput& out : s.marker.outputs) {
            if (out.token) {
                Token t{*out.token};
                if (t.id.IsNull()) t.id = txid.ToUint256();
                tokens[out.n] = t;
            } else if (out.n < tx.vout.size() && s.owned[out.n]) {
                if (const auto t{pwallet->chain().getBitAssetsToken(COutPoint{txid, out.n})}) tokens[out.n] = *t;
            }
        }
        const auto base{[&](std::string category) {
            Row r{s.time, txid, s.confirmations, s.block, std::move(category)};
            r.abandoned = s.abandoned;
            if (s.all_mine) r.fee = s.debit - tx.GetValueOut();
            return r;
        }};
        const auto add{[&](std::string category, const AssetId& asset, std::optional<uint64_t> amount, int sign, bool at_least = false, bool at_most = false) {
            if (asset.IsNull()) return; // CHN: the transactions of the wallet say it
            Row r{base(std::move(category))};
            r.asset = asset;
            r.sign = sign;
            if (amount) {
                r.amount = *amount;
                r.has_amount = true;
            }
            r.at_least = at_least;
            r.at_most = at_most;
            for (const auto& [n, t] : tokens) {
                if (t.id != asset || (t.kind != Token::Kind::ASSET && t.kind != Token::Kind::CONTROL)) continue;
                const Party p{address(tx.vout[n].scriptPubKey), s.owned[n]};
                // Change back to the wallet is no destination of a send.
                if (sign < 0 && p.mine) continue;
                // What came in: to the wallet's outputs (the sender's change is theirs).
                if (sign > 0 && !p.mine && !s.any_mine) continue;
                if (std::none_of(r.to.begin(), r.to.end(), [&](const Party& q) { return q.address == p.address; })) r.to.push_back(p);
            }
            std::stable_partition(r.to.begin(), r.to.end(), [](const Party& p) { return !p.mine; });
            rows.push_back(std::move(r));
        }};
        const auto received{[&](const AssetId& asset, Token::Kind kind) {
            uint64_t sum{0};
            for (const auto& [n, t] : tokens) {
                if (t.id == asset && t.kind == kind && s.owned[n]) sum += t.amount;
            }
            return sum;
        }};
        const auto sent{[&](const AssetId& asset, Token::Kind kind) {
            uint64_t sum{0};
            for (const auto& [n, t] : tokens) {
                if (t.id == asset && t.kind == kind && !s.owned[n]) sum += t.amount;
            }
            return sum;
        }};
        std::set<AssetId> assets;
        for (const auto& [n, t] : tokens) {
            if (t.kind == Token::Kind::ASSET || t.kind == Token::Kind::CONTROL) assets.insert(t.id);
        }
        const size_t first{rows.size()};
        if (!s.any_mine) {
            // From someone else: what came to the wallet's outputs.
            for (const AssetId& asset : assets) {
                if (const uint64_t got{received(asset, Token::Kind::ASSET)}) add("receive", asset, got, +1);
                if (received(asset, Token::Kind::CONTROL)) add("control", asset, std::nullopt, +1);
            }
        } else if (!s.marker.operation) {
            for (const AssetId& asset : assets) {
                if (const uint64_t gone{sent(asset, Token::Kind::ASSET)}) add("send", asset, gone, -1);
                if (sent(asset, Token::Kind::CONTROL)) add("control", asset, std::nullopt, -1);
            }
        } else {
            std::visit([&](const auto& op) {
                using T = std::decay_t<decltype(op)>;
                if constexpr (std::is_same_v<T, bitassets::Reserve>) {
                    rows.push_back(base("reserve"));
                } else if constexpr (std::is_same_v<T, bitassets::Register>) {
                    add("register", op.name, op.supply, +1);
                } else if constexpr (std::is_same_v<T, bitassets::Mint>) {
                    add("mint", op.asset, op.amount, +1);
                } else if constexpr (std::is_same_v<T, bitassets::UpdateAsset>) {
                    add("update", op.asset, std::nullopt, 0);
                } else if constexpr (std::is_same_v<T, bitassets::SetLogo>) {
                    add("logo", op.asset, std::nullopt, 0);
                } else if constexpr (std::is_same_v<T, bitassets::SetDisplayName>) {
                    add("display_name", op.asset, std::nullopt, 0);
                } else if constexpr (std::is_same_v<T, bitassets::ReleaseAsset>) {
                    add("release", op.asset, std::nullopt, 0);
                } else if constexpr (std::is_same_v<T, bitassets::Burn>) {
                    for (const Token& t : op.tokens) {
                        if (t.kind == Token::Kind::ASSET) add("burn", t.id, t.amount, -1);
                        if (t.kind == Token::Kind::CONTROL) add("control", t.id, std::nullopt, -1);
                    }
                } else if constexpr (std::is_same_v<T, bitassets::Swap>) {
                    add("swap", op.asset_in, op.amount_in, -1);
                    const uint64_t got{received(op.asset_out, Token::Kind::ASSET)};
                    add("swap", op.asset_out, got ? got : op.min_out, +1, /*at_least=*/!got);
                } else if constexpr (std::is_same_v<T, bitassets::AddLiquidity>) {
                    add("pool_add", op.asset_a, op.amount_a, -1, false, /*at_most=*/true);
                    add("pool_add", op.asset_b, op.amount_b, -1, false, /*at_most=*/true);
                } else if constexpr (std::is_same_v<T, bitassets::RemoveLiquidity>) {
                    const uint64_t a{received(op.asset_a, Token::Kind::ASSET)}, b{received(op.asset_b, Token::Kind::ASSET)};
                    add("pool_remove", op.asset_a, a ? a : op.min_a, +1, !a);
                    add("pool_remove", op.asset_b, b ? b : op.min_b, +1, !b);
                } else if constexpr (std::is_same_v<T, bitassets::CreateAuction>) {
                    add("auction", op.base, op.base_amount, -1);
                } else if constexpr (std::is_same_v<T, bitassets::Bid>) {
                    if (const auto auction{pwallet->chain().getBitAssetsAuction(op.auction)}) {
                        add("bid", auction->quote, op.quote_amount, -1);
                        const uint64_t got{received(auction->base, Token::Kind::ASSET)};
                        add("bid", auction->base, got ? got : op.min_base, +1, !got);
                    }
                } else if constexpr (std::is_same_v<T, bitassets::Collect>) {
                    for (const AssetId& asset : assets) {
                        if (const uint64_t got{received(asset, Token::Kind::ASSET)}) add("collect", asset, got, +1);
                    }
                    if (rows.size() == first) rows.push_back(base("collect"));
                } else {
                    // An operation added later (Layers' own): what moved, as a transfer.
                    for (const AssetId& asset : assets) {
                        if (const uint64_t gone{sent(asset, Token::Kind::ASSET)}) add("send", asset, gone, -1);
                        if (const uint64_t got{received(asset, Token::Kind::ASSET)}) add("receive", asset, got, +1);
                    }
                }
            }, *s.marker.operation);
            // Units sent to others besides what the operation does (a mint to another address is its "to").
        }
        if (rows.size() == first) continue;
        // Who paid what the wallet received from someone else: the outputs the transaction spent.
        if (!s.any_mine) {
            std::vector<Party> from;
            std::optional<std::vector<CTxOut>> spent;
            if (s.block) {
                spent = pwallet->chain().getSpentOutputs(s.block->first, txid);
            } else {
                std::map<COutPoint, Coin> coins;
                for (const CTxIn& in : tx.vin) coins[in.prevout];
                pwallet->chain().findCoins(coins);
                std::vector<CTxOut> outs;
                for (const CTxIn& in : tx.vin) {
                    if (coins[in.prevout].IsSpent()) break;
                    outs.push_back(coins[in.prevout].out);
                }
                if (outs.size() == tx.vin.size()) spent = std::move(outs);
            }
            if (spent) {
                LOCK(pwallet->cs_wallet);
                for (const CTxOut& out : *spent) {
                    const Party p{address(out.scriptPubKey), pwallet->IsMine(out)};
                    if (std::none_of(from.begin(), from.end(), [&](const Party& q) { return q.address == p.address; })) from.push_back(p);
                }
            } else {
                from.push_back({"unknown", false});
            }
            for (size_t i{first}; i < rows.size(); ++i) rows[i].from = from;
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { if (a.time != b.time) return a.time > b.time;
        // Seen in the same second: the newer is the less deep.
        if (a.confirmations != b.confirmations) return a.confirmations < b.confirmations;
        return a.txid < b.txid; });
    UniValue result(UniValue::VARR);
    int skipped{0};
    for (const Row& r : rows) {
        if (only && (!r.asset || *r.asset != *only)) continue;
        if (skipped++ < skip) continue;
        if (result.size() >= static_cast<size_t>(count)) break;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", r.txid.GetHex());
        obj.pushKV("time", r.time);
        obj.pushKV("confirmations", r.confirmations);
        if (r.block) {
            obj.pushKV("blockhash", r.block->first.GetHex());
            obj.pushKV("blockheight", r.block->second);
        }
        obj.pushKV("category", r.category);
        if (r.asset) {
            const auto record{pwallet->chain().getBitAsset(*r.asset)};
            const uint8_t decimals{record ? record->decimals : uint8_t{0}};
            obj.pushKV("asset", r.asset->GetHex());
            obj.pushKV("label", bitassets::AssetLabel(*r.asset, record ? &*record : nullptr));
            if (record) obj.pushKV("seq", bitassets::FormatSeq(record->seq));
            obj.pushKV("decimals", decimals);
            if (r.has_amount) obj.pushKV("amount", bitassets::AmountToJSON(r.amount, decimals));
        }
        obj.pushKV("sign", r.sign);
        if (r.at_least) obj.pushKV("at_least", true);
        if (r.at_most) obj.pushKV("at_most", true);
        if (r.fee) obj.pushKV("fee", ValueFromAmount(*r.fee));
        const auto parties{[](const std::vector<Party>& list) {
            UniValue arr(UniValue::VARR);
            for (const Party& p : list) {
                UniValue o(UniValue::VOBJ);
                o.pushKV("address", p.address);
                o.pushKV("mine", p.mine);
                arr.push_back(std::move(o));
            }
            return arr;
        }};
        obj.pushKV("to", parties(r.to));
        obj.pushKV("from", parties(r.from));
        obj.pushKV("abandoned", r.abandoned);
        result.push_back(std::move(obj));
    }
    return result;
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
                    summary = strprintf("Put %s and %s (at most) in their pool", amount(op.asset_a, op.amount_a), amount(op.asset_b, op.amount_b));
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
                } else if constexpr (std::is_same_v<T, bitassets::SetLogo>) {
                    operation = "logo";
                    summary = op.logo.empty() ? "Removed the logo of " + LabelOfAsset(*pwallet, op.asset)
                                              : strprintf("Set the logo of %s (%u bytes)", LabelOfAsset(*pwallet, op.asset), op.logo.size());
                } else if constexpr (std::is_same_v<T, bitassets::SetDisplayName>) {
                    operation = "display name";
                    summary = op.display_name.empty() ? "Removed the display name of " + LabelOfAsset(*pwallet, op.asset)
                                                      : strprintf("Set the display name of %s to \"%s\"", LabelOfAsset(*pwallet, op.asset), op.display_name);
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

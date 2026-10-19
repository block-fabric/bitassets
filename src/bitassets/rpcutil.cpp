// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/rpcutil.h>

#include <netaddress.h>
#include <netbase.h>
#include <pubkey.h>
#include <rpc/protocol.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <cstring>

namespace bitassets {

std::vector<RPCArg> DataArgs()
{
    return {
        {"info", RPCArg::Type::STR, RPCArg::Optional::OMITTED, strprintf("What the asset is: a description, a link (at most %u bytes)", MAX_INFO_SIZE)},
        {"commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A hash (32 bytes, hex) of data kept elsewhere, such as terms or a prospectus"},
        {"ipv4", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An IPv4 address of the issuer, with a port or not"},
        {"ipv6", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An IPv6 address of the issuer, with a port or not"},
        {"encryptionkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A public key (33 bytes, hex) to encrypt messages to the issuer to"},
        {"signingkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "An x-only public key (32 bytes, hex) the issuer signs with"},
    };
}

std::vector<RPCResult> DataResults()
{
    return {
        {RPCResult::Type::STR, "info", /*optional=*/true, "What the asset is"},
        {RPCResult::Type::STR_HEX, "commitment", /*optional=*/true, "A hash of data kept elsewhere"},
        {RPCResult::Type::STR, "ipv4", /*optional=*/true, "An IPv4 address of the issuer"},
        {RPCResult::Type::STR, "ipv6", /*optional=*/true, "An IPv6 address of the issuer"},
        {RPCResult::Type::STR_HEX, "encryptionkey", /*optional=*/true, "A key to encrypt to the issuer to"},
        {RPCResult::Type::STR_HEX, "signingkey", /*optional=*/true, "A key the issuer signs with"},
    };
}

namespace {
std::string FormatSocket(const SocketV4& socket)
{
    const std::string address{strprintf("%u.%u.%u.%u", socket.ip[0], socket.ip[1], socket.ip[2], socket.ip[3])};
    return socket.port == 0 ? address : strprintf("%s:%u", address, socket.port);
}

std::string FormatSocket(const SocketV6& socket)
{
    in6_addr addr;
    std::memcpy(&addr, socket.ip.data(), 16);
    if (socket.port == 0) return CNetAddr{addr}.ToStringAddr();
    return CService{CNetAddr{addr}, socket.port}.ToStringAddrPort();
}

uint256 ParseCommitment(const UniValue& value)
{
    const auto hash{uint256::FromHex(value.get_str())};
    if (!hash) throw JSONRPCError(RPC_INVALID_PARAMETER, "commitment: 32 bytes in hex");
    return *hash;
}

SocketV4 ParseV4(const UniValue& value)
{
    const auto service{Lookup(value.get_str(), 0, /*fAllowLookup=*/false)};
    in_addr addr;
    if (!service || !service->IsIPv4() || !service->GetInAddr(&addr)) throw JSONRPCError(RPC_INVALID_PARAMETER, "ipv4: an IPv4 address, such as \"203.0.113.7\", with a port or not");
    SocketV4 socket;
    std::memcpy(socket.ip.data(), &addr, 4);
    socket.port = service->GetPort();
    return socket;
}

SocketV6 ParseV6(const UniValue& value)
{
    const auto service{Lookup(value.get_str(), 0, /*fAllowLookup=*/false)};
    in6_addr addr;
    if (!service || !service->IsIPv6() || !service->GetIn6Addr(&addr)) throw JSONRPCError(RPC_INVALID_PARAMETER, "ipv6: an IPv6 address, such as \"2001:db8::7\", or in brackets with a port");
    SocketV6 socket;
    std::memcpy(socket.ip.data(), &addr, 16);
    socket.port = service->GetPort();
    return socket;
}

EncryptionKey ParseEncryptionKey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHex(value.get_str())};
    if (bytes.size() != 33 || !CPubKey{bytes}.IsFullyValid()) throw JSONRPCError(RPC_INVALID_PARAMETER, "encryptionkey: a compressed public key, 33 bytes in hex");
    EncryptionKey key;
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

SigningKey ParseSigningKey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHex(value.get_str())};
    if (bytes.size() != 32 || !XOnlyPubKey{bytes}.IsFullyValid()) throw JSONRPCError(RPC_INVALID_PARAMETER, "signingkey: an x-only public key, 32 bytes in hex");
    return uint256{std::span{bytes}};
}

std::string ParseInfo(const UniValue& value)
{
    const std::string info{value.get_str()};
    if (info.size() > MAX_INFO_SIZE) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("info: at most %u bytes", MAX_INFO_SIZE));
    return info;
}

template <typename T, typename F>
void ReadUpdate(const UniValue& obj, const std::string& key, Update<T>& update, F parse)
{
    if (!obj.exists(key)) return;
    if (obj[key].isNull()) {
        update.kind = UpdateKind::DELETE;
        return;
    }
    update.kind = UpdateKind::SET;
    update.value = parse(obj[key]);
}

const std::vector<std::string> FIELDS{"info", "commitment", "ipv4", "ipv6", "encryptionkey", "signingkey"};
} // namespace

AssetUpdates ParseUpdates(const UniValue& value)
{
    if (!value.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The data of an asset is an object");
    for (const std::string& key : value.getKeys()) {
        if (std::find(FIELDS.begin(), FIELDS.end(), key) == FIELDS.end()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown field of asset data: " + key);
    }
    AssetUpdates updates;
    ReadUpdate(value, "info", updates.info, ParseInfo);
    ReadUpdate(value, "commitment", updates.commitment, ParseCommitment);
    ReadUpdate(value, "ipv4", updates.ipv4, ParseV4);
    ReadUpdate(value, "ipv6", updates.ipv6, ParseV6);
    ReadUpdate(value, "encryptionkey", updates.encryption_key, ParseEncryptionKey);
    ReadUpdate(value, "signingkey", updates.signing_key, ParseSigningKey);
    return updates;
}

AssetData ParseData(const UniValue& value)
{
    const AssetUpdates updates{ParseUpdates(value)};
    AssetData data;
    if (updates.info.kind == UpdateKind::SET) data.info = updates.info.value;
    if (updates.commitment.kind == UpdateKind::SET) data.commitment = updates.commitment.value;
    if (updates.ipv4.kind == UpdateKind::SET) data.ipv4 = updates.ipv4.value;
    if (updates.ipv6.kind == UpdateKind::SET) data.ipv6 = updates.ipv6.value;
    if (updates.encryption_key.kind == UpdateKind::SET) data.encryption_key = updates.encryption_key.value;
    if (updates.signing_key.kind == UpdateKind::SET) data.signing_key = updates.signing_key.value;
    return data;
}

UniValue DataToJSON(const AssetData& data)
{
    UniValue obj(UniValue::VOBJ);
    if (data.info) obj.pushKV("info", *data.info);
    if (data.commitment) obj.pushKV("commitment", data.commitment->GetHex());
    if (data.ipv4) obj.pushKV("ipv4", FormatSocket(*data.ipv4));
    if (data.ipv6) obj.pushKV("ipv6", FormatSocket(*data.ipv6));
    if (data.encryption_key) obj.pushKV("encryptionkey", HexStr(*data.encryption_key));
    if (data.signing_key) obj.pushKV("signingkey", HexStr(*data.signing_key));
    return obj;
}

AssetId ParseAssetArg(const UniValue& value, const std::function<std::optional<AssetId>(uint32_t)>& lookup)
{
    const std::string text{value.get_str()};
    if (text.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The asset is empty");
    if (ToUpper(text) == "CHN") return CHN;
    if (text.size() == 66 && text.starts_with("0x")) {
        if (const auto hash{uint256::FromHex(text.substr(2))}) return *hash;
    }
    if (lookup) {
        // Text that reads as a number names the asset of that number, or nothing: never the asset
        // whose name it is, which may be a private look-alike (a number of an asset retired since,
        // or not given yet, would otherwise resolve to it).
        if (const auto seq{ParseSeq(text)}) {
            if (const auto asset{lookup(*seq)}) return *asset;
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset has the number %s (it may have been retired)", text));
        }
        // Shaped as a number that is none ("0000-1739-0029"): an error, never the asset of that name.
        if (ShapedAsNumber(text)) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not the number of an asset (a number of three groups does not start with 0000)", text));
    }
    return HashName(text);
}

std::string AssetLabel(const AssetId& asset, const AssetRecord* record)
{
    if (asset.IsNull()) return "CHN";
    if (record && !record->text.empty()) return record->text;
    return "0x" + asset.GetHex();
}

std::string FormatUnits(uint64_t units, uint8_t decimals)
{
    if (decimals == 0) return strprintf("%u", units);
    uint64_t scale{1};
    for (uint8_t i{0}; i < decimals; ++i) scale *= 10;
    return strprintf("%u.%0*u", units / scale, int{decimals}, units % scale);
}

UniValue AmountToJSON(uint64_t units, uint8_t decimals)
{
    UniValue value;
    value.setNumStr(FormatUnits(units, decimals));
    return value;
}

uint64_t ParseUnits(const UniValue& value, uint8_t decimals, bool allow_zero)
{
    if (!value.isNum() && !value.isStr()) throw JSONRPCError(RPC_TYPE_ERROR, "An amount is a number or a string");
    int64_t units;
    if (!ParseFixedPoint(value.getValStr(), decimals, &units) || units < 0) {
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Invalid amount %s: a positive number with at most %u decimals", value.getValStr(), decimals));
    }
    if (units == 0 && !allow_zero) throw JSONRPCError(RPC_TYPE_ERROR, "The amount is zero");
    return static_cast<uint64_t>(units);
}

std::vector<RPCArg> SearchAssetsArgs()
{
    return {
        {"query", RPCArg::Type::STR, RPCArg::Default{""}, "Text, \"#\" and a number, a number, or a hash"},
        {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
        {
            {"min_supply", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "At least this supply, in whole units"},
            {"has_pool", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only those with a pool someone provides liquidity to"},
            {"has_logo", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only those with a logo"},
            {"active_within_blocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only those touched in the last this many blocks"},
            {"exclude_dead", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Leave out those not touched in the last this many blocks; true: in the volume window (about a day)"},
            {"created_after", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only those registered after this height"},
            {"exclude_burned", RPCArg::Type::BOOL, RPCArg::Default{false}, "Leave out those with no supply"},
            {"exclude_retired", RPCArg::Type::BOOL, RPCArg::Default{true}, "Leave out the retired ones"},
            {"exclude_keywords", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave out those with any of these words in their names (whole words, any case)",
                {{"keyword", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
            {"assets", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Only these (names, numbers or hashes; at most 5000)",
                {{"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
        }},
        {"sort", RPCArg::Type::STR, RPCArg::Default{"relevance"}, "relevance (a text: the whole name, then its start, then a word's start; no text: as activity), "
            "name, newest, oldest, supply, activity (last touched first), volume (traded in the window; those not traded are left out), pool_depth (those with a pool with CHN only)"},
        {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next_cursor\" of the page before, with the same query, filters and sort"},
        {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, "How many at most, from 1 to 200"},
    };
}

RPCResult SearchAssetsResult()
{
    return RPCResult{RPCResult::Type::OBJ, "", "",
    {
        {RPCResult::Type::ARR, "assets", "", {{RPCResult::Type::OBJ, "", "These and, unless retired, the other fields of getasset (registration, data, releasable, control, logo)", {
            {RPCResult::Type::STR_HEX, "asset", "The hash of its name"},
            {RPCResult::Type::STR, "name", /*optional=*/true, "Its name, if public"},
            {RPCResult::Type::STR, "label", "Its name, or \"0x\" and its hash"},
            {RPCResult::Type::STR, "seq", "Its number"},
            {RPCResult::Type::NUM, "decimals", ""},
            {RPCResult::Type::NUM, "supply", "How much there is"},
            {RPCResult::Type::NUM, "minted", "All ever minted"},
            {RPCResult::Type::NUM, "burned", "All ever burned"},
            {RPCResult::Type::BOOL, "fixed", "Whether its supply is fixed for good"},
            {RPCResult::Type::BOOL, "retired", "Whether it was retired (its name may be another asset's now)"},
            {RPCResult::Type::BOOL, "has_logo", ""},
            {RPCResult::Type::BOOL, "has_pool", "Whether it has a pool someone provides liquidity to"},
            {RPCResult::Type::NUM, "pools", "How many"},
            {RPCResult::Type::NUM, "pool_depth", "The CHN in its pool with CHN (0 if none)"},
            {RPCResult::Type::NUM, "volume", "CHN traded in its pools with CHN and its auctions for CHN over the volume window"},
            {RPCResult::Type::NUM, "created", "The height of the block that registered it"},
            {RPCResult::Type::NUM, "registered", "The same"},
            {RPCResult::Type::NUM, "last_activity", "The height of the last block that touched it"},
            {RPCResult::Type::STR, "match", /*optional=*/true, "How the text matched: \"exact\", \"prefix\" or \"word\""},
        }, RPCResultOptions{.skip_type_check = true}}}},
        {RPCResult::Type::ANY, "next_cursor", "The cursor of the next page (a string); null when there is no more (a full page always has one: the next may be empty)"},
        {RPCResult::Type::BOOL, "partial", "The search stopped at its budget before the page was full: go on with next_cursor"},
        {RPCResult::Type::NUM, "scanned", "Index entries looked at"},
        {RPCResult::Type::NUM, "height", "The height of the tip the page is of"},
    }};
}

} // namespace bitassets

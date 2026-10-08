// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_RPCUTIL_H
#define BITCOIN_BITASSETS_RPCUTIL_H

#include <bitassets/state.h>
#include <rpc/util.h>

#include <univalue.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

/** What the commands of the node and of the wallet share: how assets, amounts and asset data are written in JSON. */
namespace bitassets {

/** The fields of the data of an asset, as arguments and as a result. */
std::vector<RPCArg> DataArgs();
std::vector<RPCResult> DataResults();
AssetData ParseData(const UniValue& value);
/** A field left out is kept, null deletes it, a value sets it. */
AssetUpdates ParseUpdates(const UniValue& value);
UniValue DataToJSON(const AssetData& data);

/** How an asset is named: "CHN", its name, "0x" and the hex of its hash, or its number ("1739-0029"). */
inline constexpr const char* ASSET_ARG_HELP{"The asset: \"CHN\", its name, \"0x\" and its hash, or its number"};
/**
 * The asset an argument names. `lookup` gives the asset of a number, if it can; with it, text that
 * reads as a number names that asset or none (an error), never the asset of that name. A name is
 * taken as given (its hash), whether it is registered or not.
 */
AssetId ParseAssetArg(const UniValue& value, const std::function<std::optional<AssetId>(uint32_t)>& lookup = {});
/** How an asset is shown: "CHN", its name if public, or "0x" and its hash. */
std::string AssetLabel(const AssetId& asset, const AssetRecord* record);

/** Units of an asset with `decimals` decimals, as a JSON number: 1.5 for 150 units with 2 decimals. */
UniValue AmountToJSON(uint64_t units, uint8_t decimals);
std::string FormatUnits(uint64_t units, uint8_t decimals);
/** Units from a JSON number or string in whole assets, with at most `decimals` decimals. */
uint64_t ParseUnits(const UniValue& value, uint8_t decimals, bool allow_zero = false);
/** The decimals of CHN. */
inline constexpr uint8_t CHN_DECIMALS{8};

} // namespace bitassets

#endif // BITCOIN_BITASSETS_RPCUTIL_H

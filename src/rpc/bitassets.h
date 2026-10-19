// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_BITASSETS_H
#define BITCOIN_RPC_BITASSETS_H

#include <bitassets/state.h>

#include <univalue.h>

#include <vector>

class ChainstateManager;

/**
 * searchassets (its five arguments in `params`) among `only`: the wallet's assets, for the wallet's
 * searchmyassets (interfaces::Chain::searchBitAssets). The node's commands never read a wallet.
 */
UniValue SearchBitAssetsJSON(ChainstateManager& chainman, const UniValue& params, const std::vector<bitassets::AssetId>& only);

#endif // BITCOIN_RPC_BITASSETS_H

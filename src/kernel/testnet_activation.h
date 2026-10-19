// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_TESTNET_ACTIVATION_H
#define BITCOIN_KERNEL_TESTNET_ACTIVATION_H

#include <limits>

/**
 * BitAssets on the Chains test network: the height of the Chains testnet block that activated BitAssets in
 * slot 4 (Consensus::SidechainParams::main_activation_height of the test network).
 *
 * The test network was reset on 17/Oct/2026 (new genesis blocks for Chains and every sidechain, every
 * rule from block 0). Slot 4 activates again on the new Chains testnet; until then this is 0, which
 * the node accepts on a test network with a warning in its log (CheckSidechainActivation) and which
 * leaves nothing of the mainchain out. Once the slot is active, set it to the "activationheight" that
 * `chains-cli -testnet getsidechain 4` shows, rebuild, and start the nodes of the test network
 * only then: a node started before keeps the record of the mainchain it began (from height 0, not from
 * the activation; harmless, but start new datadirs with the height set).
 */
inline constexpr int TESTNET_MAIN_ACTIVATION_HEIGHT{67};

/**
 * The logos of assets on the test network (Consensus::SidechainParams::logo_height; bitassets/logo.h,
 * doc/bitassets.md "Logos"): a marker that sets a logo is valid from this BitAssets block on. A
 * placeholder (never) until the deploy: set it a few blocks above the tip then, rebuild, and upgrade
 * every node of the test network before the chain reaches it (a node of before would refuse the blocks).
 */
inline constexpr int TESTNET_LOGO_HEIGHT{439};

/**
 * Tickers and display names on the test network (Consensus::SidechainParams::ticker_height;
 * bitassets::CheckTickerActivation, doc/bitassets.md "Tickers and display names"): from this BitAssets
 * block on, the name of a new asset is a ticker and a marker may set a display name. A placeholder
 * (never) until the deploy: set it a few blocks above the tip then, rebuild, and upgrade every node of
 * the test network before the chain reaches it (a node of before would refuse the blocks that set a
 * display name, and take registrations that are not tickers).
 */
inline constexpr int TESTNET_TICKER_HEIGHT{std::numeric_limits<int>::max()};

#endif // BITCOIN_KERNEL_TESTNET_ACTIVATION_H

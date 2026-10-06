// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_INHERITED_PARAMS_H
#define BITCOIN_TEST_UTIL_INHERITED_PARAMS_H

#include <consensus/params.h>
#include <uint256.h>

/**
 * A sidechain turns off rules that it inherits from the mainchain: its proof
 * of work is a formality that is never retargeted, and its blocks have no
 * subsidy. The code of those rules is still there, and its tests still test
 * it, with the parameters of a network of this chain changed back to what
 * that code is for.
 */
inline Consensus::Params WithInheritedRules(Consensus::Params params)
{
    params.sidechain.enabled = false;
    params.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    params.fPowNoRetargeting = false;
    return params;
}

#endif // BITCOIN_TEST_UTIL_INHERITED_PARAMS_H

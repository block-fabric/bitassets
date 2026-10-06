// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <cstdlib>

arith_uint256 CalculateASERT(const arith_uint256& ref_target, int64_t spacing, int64_t time_diff, int64_t height_diff, const arith_uint256& pow_limit, int64_t half_life) noexcept
{
    assert(ref_target > 0 && ref_target <= pow_limit);
    assert(height_diff >= 0);
    // The multiplication by 65536 below must not overflow.
    assert(std::llabs(time_diff - spacing * height_diff) < (int64_t{1} << (63 - 16)));

    // exponent = (time_diff - spacing * (height_diff + 1)) / half_life, in 16.16 fixed point.
    const int64_t exponent{((time_diff - spacing * (height_diff + 1)) * 65536) / half_life};

    // Split into an integer number of doublings and a fractional part in [0, 1).
    // The shift of a negative value is arithmetic, i.e. it rounds towards negative infinity.
    int64_t shifts{exponent >> 16};
    const uint16_t frac{static_cast<uint16_t>(exponent)};
    assert(exponent == (shifts * 65536) + frac);

    // factor = 65536 * 2^(frac / 65536), approximated by a cubic polynomial with an error below 0.013%.
    const uint32_t factor{65536 + static_cast<uint32_t>((
        195766423245049ULL * frac +
        971821376ULL * frac * frac +
        5127ULL * frac * frac * frac +
        (1ULL << 47)) >> 48)};

    // pow_limit leaves at least 32 leading zero bits, so this cannot overflow 256 bits.
    arith_uint256 next_target{ref_target * factor};

    // Apply the integer doublings together with the 2^16 scale of the factor.
    shifts -= 16;
    if (shifts <= 0) {
        next_target >>= -shifts;
    } else {
        const arith_uint256 shifted{next_target << shifts};
        if ((shifted >> shifts) != next_target) {
            // Overflowed 256 bits.
            next_target = pow_limit;
        } else {
            next_target = shifted;
        }
    }

    if (next_target == 0) return arith_uint256{1};
    if (next_target > pow_limit) return pow_limit;
    return next_target;
}

/**
 * aserti3 retarget with a bootstrap period.
 *
 * Blocks below the anchor height are mined at the proof of work limit. The
 * anchor's target is the limit scaled by how long those blocks took compared
 * to the target spacing, so that aserti3 starts from the hash rate that is
 * actually present. The span is measured from block 1, which keeps the time
 * between the creation of the genesis block and the launch of the network
 * out of the calculation. Every later block is retargeted by aserti3 relative
 * to the anchor.
 */
static unsigned int GetNextASERTWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params)
{
    const arith_uint256 pow_limit{UintToArith256(params.powLimit)};
    const unsigned int pow_limit_bits{pow_limit.GetCompact()};
    const int anchor_height{params.asert_anchor_height};
    assert(anchor_height >= 3);
    const int next_height{pindexLast->nHeight + 1};

    // Bootstrap period.
    if (next_height < anchor_height) return pow_limit_bits;

    // The anchor: calibrate from the bootstrap blocks 1..anchor_height-1.
    if (next_height == anchor_height) {
        const CBlockIndex* first{pindexLast->GetAncestor(1)};
        assert(first != nullptr);
        const int64_t expected_timespan{(pindexLast->nHeight - first->nHeight) * params.nPowTargetSpacing};
        const int64_t actual_timespan{std::max<int64_t>(pindexLast->GetBlockTime() - first->GetBlockTime(), 1)};
        // Bootstrap blocks slower than the target spacing cannot lower the difficulty below the limit.
        if (actual_timespan >= expected_timespan) return pow_limit_bits;
        // pow_limit has at least 32 leading zero bits and actual_timespan fits in 32 bits here.
        arith_uint256 target{pow_limit};
        target *= static_cast<uint32_t>(actual_timespan);
        target /= static_cast<uint32_t>(expected_timespan);
        if (target == 0) target = arith_uint256{1};
        return target.GetCompact();
    }

    // Special difficulty rule for test networks: a block more than two target
    // spacings after its parent may be a min-difficulty block.
    if (params.fPowAllowMinDifficultyBlocks &&
        pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2) {
        return pow_limit_bits;
    }

    const CBlockIndex* anchor{pindexLast->GetAncestor(anchor_height)};
    assert(anchor != nullptr && anchor->pprev != nullptr);

    arith_uint256 ref_target;
    ref_target.SetCompact(anchor->nBits);

    const int64_t time_diff{pindexLast->GetBlockTime() - anchor->pprev->GetBlockTime()};
    const int64_t height_diff{pindexLast->nHeight - anchor->nHeight};

    return CalculateASERT(ref_target, params.nPowTargetSpacing, time_diff, height_diff, pow_limit, params.asert_half_life).GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    if (params.asert_half_life > 0 && !params.fPowNoRetargeting) {
        return GetNextASERTWorkRequired(pindexLast, pblock, params);
    }
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then it MUST be a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4
    if (params.enforce_BIP94) {
        // Here we use the first block of the difficulty period. This way
        // the real difficulty is always preserved in the first block as
        // it is not allowed to use the min-difficulty exception.
        int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
        const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) return true;

    // aserti3 retargets every block by a factor that depends on block
    // timestamps, which are not available here.
    if (params.asert_half_life > 0 && !params.fPowNoRetargeting) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        // Calculate the smallest difficulty value possible:
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Round and then compare this new calculated value to what is
        // observed.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

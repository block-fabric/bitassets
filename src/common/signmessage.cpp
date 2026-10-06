// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <common/signmessage.h>

#include <addresstype.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <pubkey.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/**
 * Text used to signify that a signed message follows and to prevent
 * inadvertently signing a transaction.
 */
// The same on the mainchain and on all its sidechains, whose addresses differ.
const std::string MESSAGE_MAGIC = "Chains Signed Message:\n";

MessageVerificationResult MessageVerify(
    const std::string& address,
    const std::string& signature,
    const std::string& message)
{
    CTxDestination destination = DecodeDestination(address);
    if (!IsValidDestination(destination)) {
        return MessageVerificationResult::ERR_INVALID_ADDRESS;
    }

    // An address stands for one key if it is the hash of that key, which is the
    // case of P2PKH addresses and of P2WPKH addresses.
    const PKHash* pkhash{std::get_if<PKHash>(&destination)};
    const WitnessV0KeyHash* witness_keyhash{std::get_if<WitnessV0KeyHash>(&destination)};
    // A P2TR address is a key itself. It signs with a Schnorr signature, from
    // which no key can be recovered; the signature is checked against the address.
    if (const WitnessV1Taproot* taproot{std::get_if<WitnessV1Taproot>(&destination)}) {
        const auto signature_bytes{DecodeBase64(signature)};
        if (!signature_bytes) return MessageVerificationResult::ERR_MALFORMED_SIGNATURE;
        if (signature_bytes->size() != 64) return MessageVerificationResult::ERR_NOT_SIGNED;
        const XOnlyPubKey output_key{*taproot};
        return output_key.VerifySchnorr(MessageHash(message), *signature_bytes) ? MessageVerificationResult::OK : MessageVerificationResult::ERR_NOT_SIGNED;
    }
    if (!pkhash && !witness_keyhash) {
        return MessageVerificationResult::ERR_ADDRESS_NO_KEY;
    }

    auto signature_bytes = DecodeBase64(signature);
    if (!signature_bytes) {
        return MessageVerificationResult::ERR_MALFORMED_SIGNATURE;
    }

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(MessageHash(message), *signature_bytes)) {
        return MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED;
    }

    if (pkhash ? !(PKHash(pubkey) == *pkhash) : !(pubkey.IsCompressed() && WitnessV0KeyHash(pubkey) == *witness_keyhash)) {
        return MessageVerificationResult::ERR_NOT_SIGNED;
    }

    return MessageVerificationResult::OK;
}

bool MessageSign(
    const CKey& privkey,
    const std::string& message,
    std::string& signature)
{
    std::vector<unsigned char> signature_bytes;

    if (!privkey.SignCompact(MessageHash(message), signature_bytes)) {
        return false;
    }

    signature = EncodeBase64(signature_bytes);

    return true;
}

uint256 MessageHash(const std::string& message)
{
    HashWriter hasher{};
    hasher << MESSAGE_MAGIC << message;

    return hasher.GetHash();
}

std::string SigningResultString(const SigningResult res)
{
    switch (res) {
        case SigningResult::OK:
            return "No error";
        case SigningResult::PRIVATE_KEY_NOT_AVAILABLE:
            return "Private key not available";
        case SigningResult::SIGNING_FAILED:
            return "Sign failed";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

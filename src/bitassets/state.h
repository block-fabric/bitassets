// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#ifndef BITCOIN_BITASSETS_STATE_H
#define BITCOIN_BITASSETS_STATE_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

/**
 * Assets (BitAssets, after the sidechain of that name by LayerTwo Labs).
 *
 *  - An **asset** is registered under a name, in two steps as BitNames are (a
 *    **reservation** commits to the name under a secret nonce, a later
 *    **registration** reveals it), and is known on the chain by the SHA-256
 *    hash of the name. The registration creates its initial supply and its
 *    **control** coin: whoever holds the control coin mints more and changes
 *    the asset's data. Burning the control coin fixes the supply for good.
 *  - Asset coins, control coins, reservations, liquidity (LP) shares and
 *    auction receipts are **tokens**, carried by outputs of no value that a
 *    **marker** output (an OP_RETURN) lists, each with what it carries. Every
 *    token is accounted for: what a transaction spends, plus what its
 *    operation creates, is exactly what it outputs, plus what its operation
 *    takes in. Nothing is burned by accident.
 *  - **Pools** (a constant-product market maker, 0.3% to the liquidity
 *    providers) trade any two assets, CHN included. **Dutch auctions** sell an
 *    amount of one asset for another at a price that falls block by block.
 *  - CHN a transaction puts into a pool or an auction is burned by the marker
 *    (its value); CHN a pool or an auction pays out is paid by the coinbase of
 *    the block, as deposits are. Asset coins a pool or an auction pays out go
 *    to the **result** outputs the marker lists, in the amount the state
 *    gives when the block is connected, never less than the transaction asks.
 */
namespace bitassets {

/** An asset: the hash of its name. The null hash is CHN, the coin of the chain. */
using AssetId = uint256;
inline const AssetId CHN{};

/** OP_RETURN, then a push of these bytes and what follows. */
inline constexpr unsigned char TAG[4]{0x42, 0x41, 0x53, 0x54}; // "BAST"
inline constexpr uint8_t MARKER_VERSION{0};

/** Most outputs one transaction lists. */
inline constexpr size_t MAX_MARKER_OUTPUTS{64};
/** Longest name of an asset that a registration publishes. */
inline constexpr size_t MAX_NAME_TEXT_SIZE{64};
/** Longest display name of an asset, in bytes of UTF-8 (IsDisplayName). */
inline constexpr size_t MAX_DISPLAY_NAME_SIZE{64};
/** Shortest and longest ticker: the name a registration publishes from SidechainParams::ticker_height (IsTicker). */
inline constexpr size_t MIN_TICKER_SIZE{2};
inline constexpr size_t MAX_TICKER_SIZE{12};
/** Longest description of an asset. */
inline constexpr size_t MAX_INFO_SIZE{512};
/** Most decimals an asset is shown with. */
inline constexpr uint8_t MAX_DECIMALS{12};
/** The most units of an asset there can be, and the most any amount can be. */
inline constexpr uint64_t MAX_AMOUNT{static_cast<uint64_t>(std::numeric_limits<int64_t>::max())};
/** Liquidity shares a new pool keeps for good, so that a pool can never be emptied (and its price made anything). */
inline constexpr uint64_t MIN_LIQUIDITY{1000};
/** Shares the deposit that opens a pool (or reopens an abandoned one) must give, MIN_LIQUIDITY aside. */
inline constexpr uint64_t MIN_OPEN_SHARES{100'000};
/** CHN the deposit that opens a pool with CHN must put in: 0.01 CHN. */
inline constexpr uint64_t MIN_OPEN_CHN{1'000'000};
/** The pools keep 3 of every 1000 units traded, for the liquidity providers. */
inline constexpr uint64_t SWAP_FEE_PER_MILLE{3};
/** Longest an auction may run, in blocks. */
inline constexpr int32_t MAX_AUCTION_DURATION{1'000'000};
/** Latest an auction may start, in blocks after the one that makes it: about a year. */
inline constexpr int32_t MAX_AUCTION_DELAY{52'560};
/**
 * Least CHN a pool or an auction pays out: 0.0005 CHN. A payout takes a
 * place in the coinbase's queue, which pays a bounded number per block (half of MAX_PAYOUTS_PER_BLOCK
 * for the sidechain's own): small payouts would delay everyone's. Filling the queue for a block takes
 * 500 swaps of this much each (0.25 CHN turned over, 0.3% of it to the pools each way, and the fees),
 * not 500 of 1000 satoshis (0.000005 CHN); a payout of it is five times the least withdrawal, and an
 * output worth spending at any fee rate below about 700 sat/vB. Less from a swap or a bid is refused;
 * less taken out of a pool, or coming back from a deposit, stays in it, for its providers. An auction
 * selling CHN sells this much at least, and a bid leaves no less of it (or none); a bid in CHN pays
 * this much at least: whatever an auction holds in CHN can be paid out. (Less from collecting an
 * auction is not paid: the CHN were burned when they went in.)
 */
inline constexpr uint64_t MIN_CHN_PAYOUT{50'000};
/**
 * Most CHN payouts the transactions of a block may give rise to (swaps, bids, liquidity taken out,
 * auctions collected); a transaction that would go beyond is not valid in that block, and waits for
 * the next (the miner leaves it out, and fills the block around it). The half of a block that the
 * queue of these payouts is sure to get (sidechain::State::TakePayouts): transactions cannot make
 * that queue grow, so a flood of small swaps delays nobody's payout by more than a block, and refunds
 * and deposits, which have the other half, not at all. A transaction has one action and so at most
 * two payouts; 500 is about one in eight of the transactions a full block holds, many more than
 * pay out CHN in ordinary use.
 */
inline constexpr size_t MAX_TX_PAYOUTS_PER_BLOCK{500};
/**
 * Relay policy, not consensus: a transaction pays at least the minimum relay fee of this many virtual
 * bytes for each CHN payout and each refund of a withdrawal it would give rise to, or the mempool
 * refuses it ("ba-payouts-too-dense"). A payout takes one of the MAX_TX_PAYOUTS_PER_BLOCK, and a refund
 * one of the sidechain::MAX_REFUNDS_PER_BLOCK, of a block, which are scarcer than its weight: without
 * this, a transaction of 40 refunds takes 40 of the 100 for the minimum fee of its 4700 vbytes (470
 * sat at 0.1 sat/vB); it now pays 3000. The 600 of a block then cost the minimum fee of 450 000
 * vbytes, about half a block of transactions. What a wallet makes is never refused: it pays 1 sat/vB
 * at least (-mintxfee, ten times the minimum relay fee), a refund output alone takes 116 vbytes, and a
 * transaction has one action, which pays out once at most, in 150 vbytes or more.
 */
inline constexpr int64_t PAYOUT_POLICY_VSIZE{750};
/** Most tokens one burn names. */
inline constexpr size_t MAX_BURNS{16};

/** Serializes a std::optional as a flag and, if set, the value. */
struct OptionalFormatter {
    template <typename Stream, typename T>
    void Ser(Stream& s, const std::optional<T>& v)
    {
        s << v.has_value();
        if (v) s << *v;
    }
    template <typename Stream, typename T>
    void Unser(Stream& s, std::optional<T>& v)
    {
        bool has_value;
        s >> has_value;
        v.reset();
        if (has_value) s >> v.emplace();
    }
};
#define BA_OPT(x) Using<OptionalFormatter>(x)

struct SocketV4 {
    std::array<uint8_t, 4> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV4, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV4&, const SocketV4&) = default;
};

struct SocketV6 {
    std::array<uint8_t, 16> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV6, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV6&, const SocketV6&) = default;
};

/** A compressed secp256k1 public key, to encrypt messages to the issuer. */
using EncryptionKey = std::array<uint8_t, 33>;
/** An x-only secp256k1 public key (BIP340) the issuer signs with. */
using SigningKey = uint256;

/** The data of an asset, which its controller changes. Every field may be absent. */
struct AssetData {
    std::optional<uint256> commitment;
    std::optional<SocketV4> ipv4;
    std::optional<SocketV6> ipv6;
    std::optional<EncryptionKey> encryption_key;
    std::optional<SigningKey> signing_key;
    //! What the asset is: a description, a link.
    std::optional<std::string> info;

    SERIALIZE_METHODS(AssetData, obj) { READWRITE(BA_OPT(obj.commitment), BA_OPT(obj.ipv4), BA_OPT(obj.ipv6), BA_OPT(obj.encryption_key), BA_OPT(obj.signing_key), BA_OPT(obj.info)); }
    friend bool operator==(const AssetData&, const AssetData&) = default;
};

// windows.h (winnt.h) defines DELETE, an access right, as a macro; it would break the enumerator below.
#if defined(WIN32) && defined(DELETE)
#undef DELETE
#endif

/** How an update treats one field: delete it, leave it, or set it. */
enum class UpdateKind : uint8_t { DELETE = 0, RETAIN = 1, SET = 2 };

template <typename T>
struct Update {
    UpdateKind kind{UpdateKind::RETAIN};
    std::optional<T> value;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(kind);
        if (kind == UpdateKind::SET) s << *value;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t k;
        s >> k;
        if (k > static_cast<uint8_t>(UpdateKind::SET)) throw std::ios_base::failure("unknown update kind");
        kind = static_cast<UpdateKind>(k);
        value.reset();
        if (kind == UpdateKind::SET) s >> value.emplace();
    }
    friend bool operator==(const Update&, const Update&) = default;
};

struct AssetUpdates {
    Update<uint256> commitment;
    Update<SocketV4> ipv4;
    Update<SocketV6> ipv6;
    Update<EncryptionKey> encryption_key;
    Update<SigningKey> signing_key;
    Update<std::string> info;

    SERIALIZE_METHODS(AssetUpdates, obj) { READWRITE(obj.commitment, obj.ipv4, obj.ipv6, obj.encryption_key, obj.signing_key, obj.info); }
    friend bool operator==(const AssetUpdates&, const AssetUpdates&) = default;
    bool Empty() const;
};

/** What an output carries. */
struct Token {
    enum class Kind : uint8_t {
        ASSET = 0,       //!< `amount` units of the asset `id`
        CONTROL = 1,     //!< the control coin of the asset `id`
        RESERVATION = 2, //!< the reservation made by the transaction `id`
        LP = 3,          //!< `amount` shares of the pool `id`
        RECEIPT = 4,     //!< the right to what the auction `id` (the transaction that made it) brings in
    };
    Kind kind{Kind::ASSET};
    uint256 id;
    //! For ASSET and LP; 1 for the others.
    uint64_t amount{1};

    SERIALIZE_METHODS(Token, obj)
    {
        uint8_t k{static_cast<uint8_t>(obj.kind)};
        READWRITE(k, obj.id, VARINT(obj.amount));
        SER_READ(obj, if (k > static_cast<uint8_t>(Kind::RECEIPT)) throw std::ios_base::failure("unknown token kind"));
        SER_READ(obj, obj.kind = static_cast<Kind>(k));
    }
    friend bool operator==(const Token&, const Token&) = default;
    /** Whether an amount goes with the kind. */
    static bool Divisible(Kind kind) { return kind == Kind::ASSET || kind == Kind::LP; }
};

/** An output the marker lists: what it carries, or that it takes a result of the operation. */
struct MarkerOutput {
    uint32_t n{0};
    //! Nothing: the output takes the next result of the operation.
    std::optional<Token> token;
    friend bool operator==(const MarkerOutput&, const MarkerOutput&) = default;
};

//
// What a transaction does, beyond moving tokens.
//

struct Reserve {
    static constexpr uint8_t KIND{1};
    //! ReservationCommitment(name, nonce, script of the reservation's output).
    uint256 commitment;
    SERIALIZE_METHODS(Reserve, obj) { READWRITE(obj.commitment); }
    friend bool operator==(const Reserve&, const Reserve&) = default;
};

/** Register an asset: spends its reservation, creates its control coin and its initial supply. */
struct Register {
    static constexpr uint8_t KIND{2};
    AssetId name;
    uint256 nonce;
    uint64_t supply{0};
    uint8_t decimals{0};
    AssetData data;
    //! The name, if the registration makes it public.
    std::optional<std::string> text;
    //! Its logo (bitassets/logo.h), if any: from SidechainParams::logo_height, a registration with one
    //! is a marker of the kind REGISTER_WITH_LOGO_KIND (these fields, then the logo).
    std::vector<unsigned char> logo;
    //! Its display name (IsDisplayName), if any: from SidechainParams::ticker_height, a registration with
    //! one is a marker of the kind REGISTER_WITH_DISPLAY_NAME_KIND (these fields, the logo, empty or not, then it).
    std::string display_name;
    SERIALIZE_METHODS(Register, obj) { READWRITE(obj.name, obj.nonce, VARINT(obj.supply), obj.decimals, obj.data, BA_OPT(obj.text)); }
    friend bool operator==(const Register&, const Register&) = default;
};
/** The kind of the marker of a Register that carries a logo. */
inline constexpr uint8_t REGISTER_WITH_LOGO_KIND{33};
/** The kind of the marker of a Register that carries a display name (and a logo, or an empty one). */
inline constexpr uint8_t REGISTER_WITH_DISPLAY_NAME_KIND{35};

/** More of an asset, by whoever spends its control coin. */
struct Mint {
    static constexpr uint8_t KIND{3};
    AssetId asset;
    uint64_t amount{0};
    SERIALIZE_METHODS(Mint, obj) { READWRITE(obj.asset, VARINT(obj.amount)); }
    friend bool operator==(const Mint&, const Mint&) = default;
};

/** Change the data of an asset, by whoever spends its control coin. */
struct UpdateAsset {
    static constexpr uint8_t KIND{4};
    AssetId asset;
    AssetUpdates updates;
    SERIALIZE_METHODS(UpdateAsset, obj) { READWRITE(obj.asset, obj.updates); }
    friend bool operator==(const UpdateAsset&, const UpdateAsset&) = default;
};

/** Destroy tokens the transaction spends: asset coins, control coins (the supply is then fixed), reservations. */
struct Burn {
    static constexpr uint8_t KIND{5};
    std::vector<Token> tokens;
    SERIALIZE_METHODS(Burn, obj) { READWRITE(obj.tokens); }
    friend bool operator==(const Burn&, const Burn&) = default;
};

/** Trade `amount_in` of one asset for at least `min_out` of another, in their pool. Result: what comes out. */
struct Swap {
    static constexpr uint8_t KIND{6};
    AssetId asset_in;
    uint64_t amount_in{0};
    AssetId asset_out;
    uint64_t min_out{0};
    //! Where the coinbase pays the CHN a result is in.
    CScript chn_to;
    SERIALIZE_METHODS(Swap, obj) { READWRITE(obj.asset_in, VARINT(obj.amount_in), obj.asset_out, VARINT(obj.min_out), obj.chn_to); }
    friend bool operator==(const Swap&, const Swap&) = default;
};

/**
 * Put two assets in their pool (making it, if there is none). Results: the shares, at least
 * `min_shares`, then what goes back.
 *
 * The amounts are the most that goes in (amm::Provide), and the marker lists two result outputs. Into a pool someone provides
 * liquidity to, the side that gives fewer shares goes in whole, and of the other only what those
 * shares are worth at the pool's price: the rest goes back, as asset coins to the second result
 * output, or, for CHN, paid by the coinbase to its script (CHN under MIN_CHN_PAYOUT stays in the
 * pool). A new pool, or one nobody provides liquidity to any more, is opened by the amounts as a
 * new pool is made, what it holds merged in, and nothing goes back: the second output carries nothing.
 */
struct AddLiquidity {
    static constexpr uint8_t KIND{7};
    AssetId asset_a;
    AssetId asset_b;
    uint64_t amount_a{0};
    uint64_t amount_b{0};
    uint64_t min_shares{0};
    SERIALIZE_METHODS(AddLiquidity, obj) { READWRITE(obj.asset_a, obj.asset_b, VARINT(obj.amount_a), VARINT(obj.amount_b), VARINT(obj.min_shares)); }
    friend bool operator==(const AddLiquidity&, const AddLiquidity&) = default;
};

/** Give shares of a pool back. Results: its two assets, in the order named, at least the least amounts named. */
struct RemoveLiquidity {
    static constexpr uint8_t KIND{8};
    AssetId asset_a;
    AssetId asset_b;
    uint64_t shares{0};
    uint64_t min_a{0};
    uint64_t min_b{0};
    CScript chn_to;
    SERIALIZE_METHODS(RemoveLiquidity, obj) { READWRITE(obj.asset_a, obj.asset_b, VARINT(obj.shares), VARINT(obj.min_a), VARINT(obj.min_b), obj.chn_to); }
    friend bool operator==(const RemoveLiquidity&, const RemoveLiquidity&) = default;
};

/**
 * Sell `base_amount` of an asset for another, by Dutch auction: the price of all of it starts at
 * `start_price` at `start_height` and falls in a straight line to `end_price` over `duration`
 * blocks; a bid buys at the price of its block. The transaction outputs the receipt, which
 * collects what the auction brings in.
 */
struct CreateAuction {
    static constexpr uint8_t KIND{9};
    AssetId base;
    uint64_t base_amount{0};
    AssetId quote;
    uint64_t start_price{0};
    uint64_t end_price{0};
    int32_t start_height{0};
    int32_t duration{0};
    SERIALIZE_METHODS(CreateAuction, obj) { READWRITE(obj.base, VARINT(obj.base_amount), obj.quote, VARINT(obj.start_price), VARINT(obj.end_price), obj.start_height, obj.duration); }
    friend bool operator==(const CreateAuction&, const CreateAuction&) = default;
};

/**
 * Pay `quote_amount` into an auction. Result: what it buys, at least `min_base`. One that would buy
 * more than is left buys all of it, for what that costs, if it can be given the rest back: in CHN,
 * by the coinbase to `chn_to`; in an asset, to one more result output, after that of what it buys
 * (which carries nothing if nothing comes back). Otherwise it is refused.
 */
struct Bid {
    static constexpr uint8_t KIND{10};
    Txid auction;
    uint64_t quote_amount{0};
    uint64_t min_base{0};
    CScript chn_to;
    SERIALIZE_METHODS(Bid, obj) { READWRITE(obj.auction, VARINT(obj.quote_amount), VARINT(obj.min_base), obj.chn_to); }
    friend bool operator==(const Bid&, const Bid&) = default;
};

/**
 * Close an auction with its receipt, once it has ended, sold out, or before any bid (to cancel it).
 * Results: what is left of what it sold, then what it brought in; amounts of nothing are left out.
 */
struct Collect {
    static constexpr uint8_t KIND{11};
    Txid auction;
    CScript chn_to;
    SERIALIZE_METHODS(Collect, obj) { READWRITE(obj.auction, obj.chn_to); }
    friend bool operator==(const Collect&, const Collect&) = default;
};

/**
 * Retire a dead asset: one whose supply is fixed and whose every unit is in pools nobody provides
 * liquidity to (nobody holds any, nothing is up for auction). The asset and its pools go, and its
 * name is free again. What its pools held of other assets is burned; the CHN they held is paid to
 * mainchain miners, as the fee of a withdrawal in the next bundle. Anyone may do it.
 */
struct ReleaseAsset {
    static constexpr uint8_t KIND{12};
    AssetId asset;
    SERIALIZE_METHODS(ReleaseAsset, obj) { READWRITE(obj.asset); }
    friend bool operator==(const ReleaseAsset&, const ReleaseAsset&) = default;
};

/**
 * Set the logo of an asset (bitassets/logo.h), or remove it (an empty one), by whoever spends its
 * control coin; it replaces the one before. Valid from SidechainParams::logo_height (kinds 32 and 33
 * stay clear of those Layers adds, 13 on).
 */
struct SetLogo {
    static constexpr uint8_t KIND{32};
    AssetId asset;
    std::vector<unsigned char> logo;
    SERIALIZE_METHODS(SetLogo, obj) { READWRITE(obj.asset, obj.logo); }
    friend bool operator==(const SetLogo&, const SetLogo&) = default;
};

/**
 * Set the display name of an asset (IsDisplayName), or remove it (an empty one), by whoever spends its
 * control coin; it replaces the one before. Display names are not unique: the name of an asset (its
 * ticker) is. Valid from SidechainParams::ticker_height.
 */
struct SetDisplayName {
    static constexpr uint8_t KIND{34};
    AssetId asset;
    std::string display_name;
    SERIALIZE_METHODS(SetDisplayName, obj) { READWRITE(obj.asset, obj.display_name); }
    friend bool operator==(const SetDisplayName&, const SetDisplayName&) = default;
};

using Operation = std::variant<Reserve, Register, Mint, UpdateAsset, Burn, Swap, AddLiquidity, RemoveLiquidity, CreateAuction, Bid, Collect, ReleaseAsset, SetLogo, SetDisplayName>;

/** Whether an operation sets a logo (SetLogo, or a Register with one): valid from SidechainParams::logo_height only. */
bool IsLogoOperation(const Operation& op);
/**
 * Whether a transaction may be in a block at a height where logos are `active` or not: one whose
 * marker sets a logo is not valid before (bad-ba-logo-inactive), as no such marker was.
 */
[[nodiscard]] bool CheckLogoActivation(const CTransaction& tx, bool active, std::string& reject_reason);

/** Whether an operation sets a display name (SetDisplayName, or a Register with one): valid from SidechainParams::ticker_height only. */
bool IsDisplayNameOperation(const Operation& op);
/**
 * The rules of tickers and display names, at a height where they are `active` or not. Before: a marker
 * that sets a display name is not valid (bad-ba-display-name-inactive), as no such marker was. From:
 * a registration publishes its name (bad-ba-ticker-private), and the name is a ticker (IsTicker;
 * bad-ba-ticker). Assets registered before keep their names, whatever they are.
 */
[[nodiscard]] bool CheckTickerActivation(const CTransaction& tx, bool active, std::string& reject_reason);

/** The marker output of a transaction. */
struct Marker {
    std::optional<Operation> operation;
    //! The outputs that carry tokens or take results, in increasing order.
    std::vector<MarkerOutput> outputs;
    friend bool operator==(const Marker&, const Marker&) = default;
};

CScript MarkerScript(const Marker& marker);
/** Whether the script claims to be a BitAssets marker, well formed or not. */
bool IsMarkerScript(const CScript& script);
std::optional<Marker> ParseMarkerScript(const CScript& script);
/** The marker of a transaction: none if it has none; the error if it has a bad one, or several. */
std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error = nullptr);

/** The hash of the name of an asset. */
AssetId HashName(const std::string& name);
/**
 * What a reservation commits to: HMAC-SHA256 with the nonce as key, of the name hash then the script
 * of the output that carries the reservation when it is made. Bound to that script, a copy of it made
 * by anyone else, with an output of their own, commits to nothing they can reveal; one with an output
 * of the maker's script is the maker's to spend.
 */
uint256 ReservationCommitment(const AssetId& name, const uint256& nonce, const CScript& script);
/** The pool of two assets: the same whatever their order. */
uint256 PoolId(const AssetId& a, const AssetId& b);
/** Whether a name can be published: 1 to MAX_NAME_TEXT_SIZE printable ASCII characters, no space at either end. */
bool IsAssetName(const std::string& name);
/**
 * Whether a name is a ticker: MIN_TICKER_SIZE to MAX_TICKER_SIZE characters, each an uppercase letter
 * A to Z or a digit 0 to 9. From SidechainParams::ticker_height, the name of every new asset is one
 * (the look-alike rules below apply too: no ticker is "CHN", "0X..." or holds what reads as a number).
 */
bool IsTicker(const std::string& name);
/**
 * Whether text can be the display name of an asset: 1 to MAX_DISPLAY_NAME_SIZE bytes of well-formed
 * UTF-8 (shortest forms, no surrogates), no space at either end, each code point one of those of
 * DISPLAY_NAME_RANGES (bitassets/displayname_table.h, generated from Unicode 15.0): assigned, graphic
 * (letters, digits, punctuation, symbols) or the space U+0020, and nothing that changes how the others
 * read: no control or format character (bidirectional overrides and isolates, zero-width spaces and
 * joiners, soft hyphen...), no other space, no default-ignorable or blank character (Hangul fillers,
 * braille blank), no combining mark, variation selector, conjoining jamo or character that composes
 * with the one before, and none that NFC changes. Text of these is in NFC, and is shown as it is. The
 * set is frozen: a later version of Unicode adds nothing to it (a soft fork could).
 */
bool IsDisplayName(const std::string& text);
/** Why text cannot be a display name (for those who tell the user); nullopt if it can. */
std::optional<std::string> WhyNotDisplayName(const std::string& text);

/**
 * Whether a name reads as another asset where assets are named (rpcutil's ParseAssetArg): CHN in any
 * case, a number ("1739-0029", "0001-1739-0029") or "0x" and anything. No asset is registered under
 * one (a private name is checked as far as it can be: CHN).
 */
bool ReadsAsAnotherAsset(const std::string& name);
/**
 * Whether a name holds what reads as the number of an asset where names are shown with their numbers
 * ("GOLD (#1739-0029)"): '#', '(' or ')', or four digits, a dash and four digits anywhere. Policy, not
 * consensus: the wallet refuses to reserve or publish one, and the mempool does not relay its
 * registration (ba-name-look-alike); a block may still hold one (a private name published by a block
 * producer), which every page shows number first ("#5555-1234 · \"GOLD (#1739-0029)\"").
 */
bool NameMimicsNumber(const std::string& name);
/** Whether text is shaped as a number ("1739-0029", "0000-1739-0029"): groups of four digits, two or three, with dashes. */
bool ShapedAsNumber(const std::string& text);

/** The number of an asset in the order of registration, shown as BitNames shows its numbers. */
std::string FormatSeq(uint32_t seq);
std::optional<uint32_t> ParseSeq(const std::string& text);

//
// The state.
//

/** A value of a field of an asset, with where it was set. */
template <typename T>
struct Stamped {
    std::optional<T> value;
    Txid txid;
    int32_t height{0};
    SERIALIZE_METHODS(Stamped, obj) { READWRITE(BA_OPT(obj.value), obj.txid, obj.height); }
    friend bool operator==(const Stamped&, const Stamped&) = default;
};

/** The fields of the data of an asset, in the order of AssetData: their numbers in the keys of the history. */
enum class DataField : uint8_t { COMMITMENT = 0, IPV4 = 1, IPV6 = 2, ENCRYPTION_KEY = 3, SIGNING_KEY = 4, INFO = 5 };
inline constexpr size_t DATA_FIELDS{6};
static_assert(DATA_FIELDS == 6, "AssetRecord serializes each count");

/**
 * An asset as it is now. Every value each field of its data has had is kept apart, one entry per
 * value (State::GetHistory), so that a change to it reads and writes a record of fixed size.
 */
/** Where a reservation was made: the script of its output then, and the height. */
struct ReservationOrigin {
    CScript script;
    int32_t height{0};
    SERIALIZE_METHODS(ReservationOrigin, obj) { READWRITE(obj.script, obj.height); }
    friend bool operator==(const ReservationOrigin&, const ReservationOrigin&) = default;
};

struct AssetRecord {
    uint32_t seq{0};
    Txid registration;
    int32_t height{0};
    //! The name, if its registration made it public.
    std::string text;
    uint8_t decimals{0};
    //! Units there are: all minted, less all burned.
    uint64_t supply{0};
    uint64_t minted{0};
    uint64_t burned{0};
    //! Whether the control coin was burned: no more can be minted, the data cannot change.
    bool fixed{false};
    //! Its data now.
    AssetData data;
    //! How many values each field has had (its registration's included): where its history ends.
    std::array<uint32_t, DATA_FIELDS> changes{};
    //! Auctions not collected that hold some of it: that sell it, or that took it in (bids paid it).
    //! Kept as auctions change (State::SetAuction), so that whether it can be retired is one look.
    uint32_t holding_auctions{0};
    //! Its pools that someone provides liquidity to (not amm::Abandoned), kept as pools change.
    uint32_t provided_pools{0};

    SERIALIZE_METHODS(AssetRecord, obj)
    {
        READWRITE(obj.seq, obj.registration, obj.height, obj.text, obj.decimals, obj.supply, obj.minted, obj.burned, obj.fixed,
                  obj.data, obj.changes[0], obj.changes[1], obj.changes[2], obj.changes[3], obj.changes[4], obj.changes[5],
                  obj.holding_auctions, obj.provided_pools);
    }
    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;

    const AssetData& Current() const { return data; }
};

/** The logo of an asset as the state records it (its bytes are kept apart: State::GetLogoBytes). */
struct LogoRecord {
    //! SHA-256 of the bytes.
    uint256 hash;
    //! LogoFormat.
    uint8_t format{0};
    uint32_t size{0};
    int32_t height{0};
    Txid txid;
    SERIALIZE_METHODS(LogoRecord, obj) { READWRITE(obj.hash, obj.format, obj.size, obj.height, obj.txid); }
    friend bool operator==(const LogoRecord&, const LogoRecord&) = default;
};

/** The display name of an asset as the state records it, with where it was set. */
struct DisplayNameRecord {
    std::string text;
    int32_t height{0};
    Txid txid;
    SERIALIZE_METHODS(DisplayNameRecord, obj) { READWRITE(obj.text, obj.height, obj.txid); }
    friend bool operator==(const DisplayNameRecord&, const DisplayNameRecord&) = default;
};

/** Every value each field of the data of an asset has had, oldest first, with where it was set. */
struct AssetHistory {
    std::vector<Stamped<uint256>> commitment;
    std::vector<Stamped<SocketV4>> ipv4;
    std::vector<Stamped<SocketV6>> ipv6;
    std::vector<Stamped<EncryptionKey>> encryption_key;
    std::vector<Stamped<SigningKey>> signing_key;
    std::vector<Stamped<std::string>> info;
    friend bool operator==(const AssetHistory&, const AssetHistory&) = default;

    /** The data at the end of the block at `height`; nullopt before `registered`, the height of the registration. */
    std::optional<AssetData> AtHeight(int registered, int height) const;
};

/** A pool of two assets, `asset0` < `asset1`. */
struct Pool {
    AssetId asset0;
    AssetId asset1;
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    //! Shares there are, MIN_LIQUIDITY of which nobody holds.
    uint64_t shares{0};
    Txid creation;
    int32_t height{0};
    //! What was traded in, of each asset, ever.
    uint64_t volume0{0};
    uint64_t volume1{0};
    uint32_t swaps{0};

    SERIALIZE_METHODS(Pool, obj) { READWRITE(obj.asset0, obj.asset1, obj.reserve0, obj.reserve1, obj.shares, obj.creation, obj.height, obj.volume0, obj.volume1, obj.swaps); }
    friend bool operator==(const Pool&, const Pool&) = default;
};

struct Auction {
    AssetId base;
    uint64_t base_amount{0};
    AssetId quote;
    uint64_t start_price{0};
    uint64_t end_price{0};
    int32_t start_height{0};
    int32_t duration{0};
    int32_t created{0};
    //! What is left to sell, and what bids paid in.
    uint64_t remaining{0};
    uint64_t proceeds{0};
    uint32_t bids{0};
    //! Collected: what was left and what came in went to the holder of the receipt.
    bool closed{false};
    //! The registrations of its two assets when it was made (null for CHN): an asset retired and
    //! registered again under its name is another asset, which the auction does not trade.
    Txid base_registration;
    Txid quote_registration;

    SERIALIZE_METHODS(Auction, obj)
    {
        READWRITE(obj.base, obj.base_amount, obj.quote, obj.start_price, obj.end_price, obj.start_height, obj.duration, obj.created,
                  obj.remaining, obj.proceeds, obj.bids, obj.closed, obj.base_registration, obj.quote_registration);
    }
    friend bool operator==(const Auction&, const Auction&) = default;

    //! In 64 bits: a start height near the top of 32 bits plus the duration would overflow.
    int64_t EndHeight() const { return int64_t{start_height} + duration - 1; }
    /** The price of all of it (base_amount) in the block at `height`, between the start and the end. */
    uint64_t PriceAt(int height) const;
    /** What a bid of `quote_amount` buys in the block at `height` (more than is left, if it is too much). */
    uint64_t BuysAt(int height, uint64_t quote_amount) const;
    /**
     * The least a bid in the block at `height` pays to buy all that is left: ceil(remaining * price
     * / base_amount), the smallest amount whose BuysAt is at least what is left. A bid of it may buy
     * more than is left (when one unit of the quote buys more than one of the base); what a bid that
     * buys more than is left pays, when it gets the rest back (Bid).
     */
    uint64_t CostOfRemaining(int height) const;
    /** Whether bids are taken in the block at `height`. */
    bool OpenAt(int height) const { return !closed && remaining > 0 && height >= start_height && height <= EndHeight(); }
    /** Whether the receipt can close it in the block at `height`. */
    bool CollectableAt(int height) const { return !closed && (height > EndHeight() || remaining == 0 || bids == 0); }
    /**
     * Whether it holds some of an asset, which keeps the asset from being retired: not collected, and
     * selling it, or having taken some of it in. One that sells something else for it and took none
     * of it in holds none.
     */
    bool Holds(const AssetId& asset) const { return !closed && (base == asset || (quote == asset && proceeds > 0)); }
};

/** Pool arithmetic, as the rules do it. */
namespace amm {
/** What `amount_in` buys from reserves `reserve_in` and `reserve_out`; 0 if nothing. */
uint64_t SwapOut(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in);
/** What has to go in to get `amount_out` out; nullopt if the pool does not hold that much. */
std::optional<uint64_t> SwapIn(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_out);
/** What a deposit does (Provide). */
struct Deposit {
    //! The shares it gives (for a pool it opens, MIN_LIQUIDITY more are kept for good).
    uint64_t shares{0};
    //! What goes into the pool of each asset; the rest of what the deposit offers goes back.
    uint64_t take0{0};
    uint64_t take1{0};
    //! Whether it opens the pool: a new one, or one nobody provides liquidity to any more (Abandoned).
    bool opens{false};
};
/**
 * A deposit of at most `amount0` and `amount1` into a pool; nullopt
 * if it gives no shares. Into a pool someone provides liquidity to, it gives the shares of the side
 * that gives fewer, which goes in whole; of the other side, what those shares are worth, rounded up
 * (the pool never loses), unless what would go back is CHN under MIN_CHN_PAYOUT (it goes in too). A
 * pool nobody provides liquidity to (a new one, or one everyone left) is opened as a new pool is
 * made, with what it holds merged in: isqrt of the product of its reserves after, MIN_LIQUIDITY of
 * them kept for good; both amounts go in. Its price is then the deposit's, its dust aside: nobody's
 * deposit can be taken in at the price of the dust, and a deposit that comes after another opened
 * the pool goes in at that one's price, its excess back, or is refused by its least shares.
 */
std::optional<Deposit> Provide(const Pool& pool, uint64_t amount0, uint64_t amount1);
/** What `shares` are worth of a pool, rounded down. */
std::pair<uint64_t, uint64_t> Withdraw(const Pool& pool, uint64_t shares);
/** What `shares` take out of a pool: CHN below MIN_CHN_PAYOUT is not taken out, it stays in the pool. */
std::pair<uint64_t, uint64_t> WithdrawPaid(const Pool& pool, uint64_t shares);
/**
 * How much worse, in percent, a trade's price is than the pool's price before it (the fee
 * included). Not a rule: what wallets warn about.
 */
double PriceImpact(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in, uint64_t amount_out);
/** Whether nobody provides liquidity to a pool any more: it holds only what it keeps for good. */
inline bool Abandoned(const Pool& pool) { return pool.shares <= MIN_LIQUIDITY; }
} // namespace amm

namespace detail {
template <typename Stream, typename Key, typename Value>
void WriteChanges(Stream& s, const std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    WriteCompactSize(s, changes.size());
    for (const auto& [key, value] : changes) {
        s << key << value.has_value();
        if (value) s << *value;
    }
}
template <typename Stream, typename Key, typename Value>
void ReadChanges(Stream& s, std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    changes.clear();
    const uint64_t count{ReadCompactSize(s)};
    for (uint64_t i{0}; i < count; ++i) {
        Key key;
        bool has_value;
        s >> key >> has_value;
        std::optional<Value> value;
        if (has_value) s >> value.emplace();
        changes.emplace_back(std::move(key), std::move(value));
    }
}
} // namespace detail


/** What an operation did, for those who look: its results, in order. */
struct Result {
    AssetId asset;
    uint64_t amount{0};
};

/**
 * The assets, on the store of the sidechain state (sidechain/store.h), one entry per key. Tables:
 * 0x30 what outputs carry, 0x31 assets, 0x32 reservations not revealed yet, 0x33 assets by number,
 * 0x34 pools, 0x35 auctions, 0x3d where reservations were made (ReservationOrigin), 0x3e the
 * history of the data of the assets (by asset, field, then position); single values 0x37 the next
 * number, 0x3f the layout of these tables (STORE_LAYOUT). Indexes, so that no rule reads a table
 * whole: 0x39 outputs by what they carry (its id, then its kind), 0x3a pools by asset, 0x3b
 * auctions not closed by asset. What a block changes,
 * the store's journal notes: its undo data.
 *
 * Logos (from SidechainParams::logo_height): 0x90 the logo of each asset that has one (LogoRecord), 0x91
 * its bytes. No rule reads the bytes: they are kept for those who show them (getassetlogo), with the
 * state, so that a pruned node has them too; only the current logo of each asset is kept, and a
 * reorganization puts the one before back through the journal.
 *
 * Display names (from SidechainParams::ticker_height): 0x92 the display name of each asset that has one
 * (DisplayNameRecord). A table of its own, so that the layout of the others (STORE_LAYOUT) is the same.
 */
class State
{
public:
    /** Read only. */
    explicit State(const sidechain::StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit State(sidechain::StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /**
     * Check a transaction against the state and, if it follows the rules, apply it. The CHN it pays
     * out are added to `payouts`, for the coinbase. A transaction that does not follow the rules,
     * or would take `payouts` (those of the block's transactions) beyond MAX_TX_PAYOUTS_PER_BLOCK,
     * changes nothing.
     * @param[out] released  CHN freed by retiring an asset (ReleaseAsset), for mainchain miners
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, std::vector<CTxOut>& payouts, std::string& reject_reason, CAmount* released = nullptr, int reveal_depth = 0);
    /** Whether an asset is dead and can be retired; if not, why (`why`). */
    bool Releasable(const AssetId& asset, std::string* why = nullptr) const;
    /**
     * Whether ApplyTx would accept the transaction now (in a block with room for its payouts);
     * `results`, if given, gets what it would pay out, and `payouts` how many CHN payouts it gives rise to.
     */
    [[nodiscard]] bool CheckTx(const CTransaction& tx, int height, std::string& reject_reason, std::vector<Result>* results = nullptr, int reveal_depth = 0, size_t* payouts = nullptr) const;
    /** What the inputs of a transaction carry, in the order of the inputs. */
    std::vector<std::pair<uint32_t, Token>> SpentTokens(const CTransaction& tx) const;

    std::optional<Token> GetToken(const COutPoint& outpoint) const;
    std::optional<AssetRecord> GetAsset(const AssetId& asset) const;
    /** Every value each field of the data of an asset has had (read whole: for those who look). */
    AssetHistory GetHistory(const AssetId& asset) const;
    /** The data of an asset at the end of the block at `height`; nullopt if not registered then (or now). */
    std::optional<AssetData> DataAt(const AssetId& asset, int height) const;
    bool HasAsset(const AssetId& asset) const;
    std::optional<uint256> GetReservation(const Txid& id) const;
    std::optional<ReservationOrigin> GetReservationOrigin(const Txid& id) const;
    std::optional<Pool> GetPool(const uint256& id) const;
    std::optional<Auction> GetAuction(const Txid& id) const;
    std::optional<AssetId> AssetOfSeq(uint32_t seq) const;
    uint32_t NextSeq() const;
    /** The pool of two assets, if there is one. */
    std::optional<Pool> FindPool(const AssetId& a, const AssetId& b) const;
    void ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const;
    void ForEachAsset(const std::function<bool(const AssetId&, const AssetRecord&)>& fn) const;
    void ForEachPool(const std::function<bool(const uint256&, const Pool&)>& fn) const;
    void ForEachAuction(const std::function<bool(const Txid&, const Auction&)>& fn) const;
    void ForEachReservation(const std::function<bool(const Txid&, const uint256&)>& fn) const;
    /** The logo of an asset, if it has one. */
    std::optional<LogoRecord> GetLogo(const AssetId& asset) const;
    /** The bytes of the logo of an asset, if it has one. */
    std::optional<std::vector<unsigned char>> GetLogoBytes(const AssetId& asset) const;
    /** The display name of an asset, if it has one. */
    std::optional<DisplayNameRecord> GetDisplayName(const AssetId& asset) const;
    /** The first output, by outpoint, that carries a token of this kind and id. */
    std::optional<COutPoint> FirstTokenOf(const uint256& id, Token::Kind kind) const;
    /** The pools of an asset, by id. */
    std::vector<std::pair<uint256, Pool>> PoolsOf(const AssetId& asset) const;
    /** The auctions not closed that sell an asset or sell for it, by id, `limit` at most. */
    std::vector<Txid> AuctionsOf(const AssetId& asset, size_t limit) const;
    /** How many pools and auctions there are (read whole: for information). */
    size_t PoolCount() const;

private:
    struct Plan;
    std::optional<Plan> MakePlan(const CTransaction& tx, int height, std::string& reject_reason, int reveal_depth) const;

    // Writes, with the indexes kept in step.
    sidechain::StoreOverlay& Writable() const;
    void SetToken(const COutPoint& outpoint, const std::optional<Token>& token);
    void SetReservation(const Txid& id, const std::optional<uint256>& commitment);
    void SetPool(const uint256& id, const std::optional<Pool>& pool);
    void SetAuction(const Txid& id, const std::optional<Auction>& auction);
    /** Adds `delta` to a count of the record of an asset (AssetRecord::holding_auctions, provided_pools), if it is registered. */
    void Count(const AssetId& asset, uint32_t AssetRecord::*count, int delta);

    const sidechain::StoreView* m_view;
    sidechain::StoreOverlay* m_overlay;
};

/**
 * The layout of the tables of the assets in the store. A store of another layout (one written by an
 * earlier version) is not read: the node derives the state again from the blocks (StoreLayoutCurrent).
 *  1: the history of an asset's data in its record; outputs indexed by id only; closed auctions indexed.
 *  2: the history apart (0x3e); outputs indexed by id then kind; closed auctions out of the index;
 *     where each reservation was made (0x3d); the registrations of an auction's assets in it.
 *  3: in each asset's record, how many auctions hold some of it and how many of its pools have
 *     providers (AssetRecord::holding_auctions, provided_pools).
 *  4: no order of reservations (0x36, 0x38) nor index of reservations by commitment (0x3c).
 *  5: withdrawal refunds wait with the deposits, not behind the payouts of the transactions; derived
 *     again, so that a node that kept refunds in the old queue does not pay differently from one that
 *     syncs from scratch.
 *  (The search index, derived data, has a layout of its own: bitassets::index::INDEX_LAYOUT.)
 */
inline constexpr uint32_t STORE_LAYOUT{5};
/** Whether the assets in a store are in the layout of this version, or there are none yet. */
bool StoreLayoutCurrent(const sidechain::StoreView& view);

/** Whether the marker of a transaction, if any, is well formed. This depends on the transaction alone. */
[[nodiscard]] bool CheckMarker(const CTransaction& tx, std::string& reject_reason);
/** The outputs a transaction's marker lists: they carry no value, and are not dust. */
std::set<uint32_t> TokenOutputs(const CTransaction& tx);
/** Whether a script is one a token can be carried by, or CHN paid to: a standard one someone can spend. */
bool Spendable(const CScript& script);

/**
 * A view that notes what is read through it: the keys read one by one, and the prefixes read in key
 * order. Whatever a check made through it found, it finds again as long as no key it read, nor any key
 * under a prefix it read, changes (the mempool checks a transaction with a marker again only then:
 * CTxMemPool::m_bitassets_reads).
 */
class RecordingView : public sidechain::StoreView
{
public:
    explicit RecordingView(const sidechain::StoreView& base) : m_base{base} {}
    std::optional<sidechain::StoreBytes> Get(std::span<const unsigned char> key) const override
    {
        m_keys.emplace(key.begin(), key.end());
        return m_base.Get(key);
    }
    std::optional<std::pair<sidechain::StoreBytes, sidechain::StoreBytes>> Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const override
    {
        m_prefixes.emplace(prefix.begin(), prefix.end());
        return m_base.Next(from, prefix);
    }
    const std::set<sidechain::StoreBytes>& Keys() const { return m_keys; }
    const std::set<sidechain::StoreBytes>& Prefixes() const { return m_prefixes; }

private:
    const sidechain::StoreView& m_base;
    mutable std::set<sidechain::StoreBytes> m_keys;
    mutable std::set<sidechain::StoreBytes> m_prefixes;
};

/**
 * Whether what a transaction's operation is checked against may depend on the height as well as on
 * the state: an auction made to start at a height, a bid, a registration (reveal depth), a collection,
 * and any operation not known to read no height (fail closed). The mempool checks those again at every
 * change of the tip.
 */
bool CheckDependsOnHeight(const CTransaction& tx);

} // namespace bitassets

#endif // BITCOIN_BITASSETS_STATE_H

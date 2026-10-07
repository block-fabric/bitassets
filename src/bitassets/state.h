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
#include <uint256.h>

#include <array>
#include <cstdint>
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
    //! ReservationCommitment(name, nonce).
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
    SERIALIZE_METHODS(Register, obj) { READWRITE(obj.name, obj.nonce, VARINT(obj.supply), obj.decimals, obj.data, BA_OPT(obj.text)); }
    friend bool operator==(const Register&, const Register&) = default;
};

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

/** Put two assets in their pool (making it, if there is none). Result: the shares, at least `min_shares`. */
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

/** Pay `quote_amount` into an auction. Result: what it buys, at least `min_base`. */
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

using Operation = std::variant<Reserve, Register, Mint, UpdateAsset, Burn, Swap, AddLiquidity, RemoveLiquidity, CreateAuction, Bid, Collect, ReleaseAsset>;

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
/** What a reservation commits to: HMAC-SHA256 with the nonce as key, of the name hash. */
uint256 ReservationCommitment(const AssetId& name, const uint256& nonce);
/** The pool of two assets: the same whatever their order. */
uint256 PoolId(const AssetId& a, const AssetId& b);
/** Whether a name can be published: 1 to MAX_NAME_TEXT_SIZE printable ASCII characters, no space at either end. */
bool IsAssetName(const std::string& name);

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
    std::vector<Stamped<uint256>> commitment;
    std::vector<Stamped<SocketV4>> ipv4;
    std::vector<Stamped<SocketV6>> ipv6;
    std::vector<Stamped<EncryptionKey>> encryption_key;
    std::vector<Stamped<SigningKey>> signing_key;
    std::vector<Stamped<std::string>> info;

    SERIALIZE_METHODS(AssetRecord, obj)
    {
        READWRITE(obj.seq, obj.registration, obj.height, obj.text, obj.decimals, obj.supply, obj.minted, obj.burned, obj.fixed,
                  obj.commitment, obj.ipv4, obj.ipv6, obj.encryption_key, obj.signing_key, obj.info);
    }
    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;

    AssetData Current() const;
    /** The data at the end of the block at `height`; nullopt before the registration. */
    std::optional<AssetData> AtHeight(int height) const;
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

    SERIALIZE_METHODS(Auction, obj)
    {
        READWRITE(obj.base, obj.base_amount, obj.quote, obj.start_price, obj.end_price, obj.start_height, obj.duration, obj.created,
                  obj.remaining, obj.proceeds, obj.bids, obj.closed);
    }
    friend bool operator==(const Auction&, const Auction&) = default;

    //! In 64 bits: a start height near the top of 32 bits plus the duration would overflow.
    int64_t EndHeight() const { return int64_t{start_height} + duration - 1; }
    /** The price of all of it (base_amount) in the block at `height`, between the start and the end. */
    uint64_t PriceAt(int height) const;
    /** What a bid of `quote_amount` buys in the block at `height` (more than is left, if it is too much). */
    uint64_t BuysAt(int height, uint64_t quote_amount) const;
    /** Whether bids are taken in the block at `height`. */
    bool OpenAt(int height) const { return !closed && remaining > 0 && height >= start_height && height <= EndHeight(); }
    /** Whether the receipt can close it in the block at `height`. */
    bool CollectableAt(int height) const { return !closed && (height > EndHeight() || remaining == 0 || bids == 0); }
};

/** Pool arithmetic, as the rules do it. */
namespace amm {
/** What `amount_in` buys from reserves `reserve_in` and `reserve_out`; 0 if nothing. */
uint64_t SwapOut(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in);
/** What has to go in to get `amount_out` out; nullopt if the pool does not hold that much. */
std::optional<uint64_t> SwapIn(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_out);
/** The shares adding `amount0` and `amount1` to a pool makes (MIN_LIQUIDITY included for a new pool); 0 if none. */
uint64_t SharesFor(const Pool& pool, uint64_t amount0, uint64_t amount1);
/** What `shares` take out of a pool. */
std::pair<uint64_t, uint64_t> Withdraw(const Pool& pool, uint64_t shares);
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

/** What a block changed in the State: the earlier value of every entry it changed, nullopt where it added one. */
struct StateUndo {
    std::optional<uint32_t> next_seq;
    std::vector<std::pair<COutPoint, std::optional<Token>>> tokens;
    std::vector<std::pair<AssetId, std::optional<AssetRecord>>> assets;
    std::vector<std::pair<Txid, std::optional<uint256>>> reservations;
    std::vector<std::pair<uint256, std::optional<Pool>>> pools;
    std::vector<std::pair<Txid, std::optional<Auction>>> auctions;
    //! The order of reservations (State::m_reservation_order) the block changed, and the next one.
    std::vector<std::pair<Txid, std::optional<uint64_t>>> reservation_orders;
    std::optional<uint64_t> next_reservation_order;

    bool empty() const { return !next_seq && tokens.empty() && assets.empty() && reservations.empty() && pools.empty() && auctions.empty() && reservation_orders.empty() && !next_reservation_order; }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        // The format from the audit's rules on: a first byte of 2, where the older format has the
        // flag of next_seq (0 or 1). The undo data is followed by more in its record, so the end of
        // the stream cannot tell the formats apart.
        s << uint8_t{2};
        s << BA_OPT(next_seq);
        detail::WriteChanges(s, tokens);
        detail::WriteChanges(s, assets);
        detail::WriteChanges(s, reservations);
        detail::WriteChanges(s, pools);
        detail::WriteChanges(s, auctions);
        detail::WriteChanges(s, reservation_orders);
        s << BA_OPT(next_reservation_order);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t first;
        s >> first;
        const bool with_orders{first == 2};
        next_seq.reset();
        if (with_orders) {
            s >> BA_OPT(next_seq);
        } else if (first != 0) {
            s >> next_seq.emplace();
        }
        detail::ReadChanges(s, tokens);
        detail::ReadChanges(s, assets);
        detail::ReadChanges(s, reservations);
        detail::ReadChanges(s, pools);
        detail::ReadChanges(s, auctions);
        // Undo data written before reservations had an order has none.
        reservation_orders.clear();
        next_reservation_order.reset();
        if (!with_orders) return;
        detail::ReadChanges(s, reservation_orders);
        s >> BA_OPT(next_reservation_order);
    }
};

/** What an operation did, for those who look: its results, in order. */
struct Result {
    AssetId asset;
    uint64_t amount{0};
};

class State
{
public:
    /**
     * Check a transaction against the state and, if it follows the rules, apply it, noting the
     * earlier values in `undo`. The CHN it pays out are added to `payouts`, for the coinbase. A
     * transaction that does not follow the rules changes nothing.
     */
    /**
     * @param[out] released  CHN freed by retiring an asset (ReleaseAsset), for mainchain miners
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, StateUndo& undo, std::vector<CTxOut>& payouts, std::string& reject_reason, int pool_rules_height = 0, CAmount* released = nullptr, int release_height = 0, int audit_height = 0);
    /** Whether an asset is dead and can be retired; if not, why (`why`). */
    bool Releasable(const AssetId& asset, std::string* why = nullptr) const;
    /** Whether ApplyTx would accept the transaction now; `results`, if given, gets what it would pay out. */
    [[nodiscard]] bool CheckTx(const CTransaction& tx, int height, std::string& reject_reason, std::vector<Result>* results = nullptr, int pool_rules_height = 0, int release_height = 0, int audit_height = 0) const;
    /** What the inputs of a transaction carry, in the order of the inputs. */
    std::vector<std::pair<uint32_t, Token>> SpentTokens(const CTransaction& tx) const;
    void Revert(const StateUndo& undo);

    const std::map<COutPoint, Token>& Tokens() const { return m_tokens; }
    const std::map<AssetId, AssetRecord>& Assets() const { return m_assets; }
    const std::map<Txid, uint256>& Reservations() const { return m_reservations; }
    const std::map<uint256, Pool>& Pools() const { return m_pools; }
    const std::map<Txid, Auction>& Auctions() const { return m_auctions; }
    std::optional<AssetId> AssetOfSeq(uint32_t seq) const;
    uint32_t NextSeq() const { return m_next_seq; }
    /** The pool of two assets, if there is one. */
    const Pool* FindPool(const AssetId& a, const AssetId& b) const;
    const std::map<Txid, uint64_t>& ReservationOrder() const { return m_reservation_order; }
    uint64_t NextReservationOrder() const { return m_next_reservation_order; }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_tokens << m_assets << m_reservations << m_seq << m_next_seq << m_pools << m_auctions << m_reservation_order << m_next_reservation_order;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_tokens >> m_assets >> m_reservations >> m_seq >> m_next_seq >> m_pools >> m_auctions;
        // A state written before reservations had an order ends here: theirs count as the oldest.
        m_reservation_order.clear();
        m_next_reservation_order = 1;
        if constexpr (requires { s.empty(); }) {
            if (s.empty()) return;
        }
        s >> m_reservation_order >> m_next_reservation_order;
    }
    friend bool operator==(const State&, const State&) = default;

private:
    struct Plan;
    std::optional<Plan> MakePlan(const CTransaction& tx, int height, std::string& reject_reason, int pool_rules_height, int release_height, int audit_height) const;

    std::map<COutPoint, Token> m_tokens;
    std::map<AssetId, AssetRecord> m_assets;
    //! Reservations not revealed yet, by the transaction that made them: their commitments.
    std::map<Txid, uint256> m_reservations;
    std::map<uint32_t, AssetId> m_seq;
    uint32_t m_next_seq{0};
    std::map<uint256, Pool> m_pools;
    std::map<Txid, Auction> m_auctions;
    //! The order in which reservations were made (from bitassets_audit_height; older ones count as 0).
    std::map<Txid, uint64_t> m_reservation_order;
    uint64_t m_next_reservation_order{1};
};

/** Whether the marker of a transaction, if any, is well formed. This depends on the transaction alone. */
[[nodiscard]] bool CheckMarker(const CTransaction& tx, std::string& reject_reason);
/** The outputs a transaction's marker lists: they carry no value, and are not dust. */
std::set<uint32_t> TokenOutputs(const CTransaction& tx);
/** Whether a script is one a token can be carried by, or CHN paid to: a standard one someone can spend. */
bool Spendable(const CScript& script);

} // namespace bitassets

#endif // BITCOIN_BITASSETS_STATE_H

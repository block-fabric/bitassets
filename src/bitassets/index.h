// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_INDEX_H
#define BITCOIN_BITASSETS_INDEX_H

#include <bitassets/state.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/**
 * The search index of the assets, their pools and their auctions: what a wallet's pages search, filter,
 * sort and page through, a page at a time, however many assets there are (millions, in time), without
 * ever reading them all.
 *
 * Derived data, not state: no rule reads it. It is kept in the store of the state, under a byte of its
 * own (sidechain::DERIVED_TABLE), written at the end of each block from what the block changed (the
 * overlay's journal: the keys of the assets, tokens, pools, auctions and logos it touched, with their
 * earlier values), so that it is flushed with the state, undone with it by a reorganisation (the
 * block's undo data has its keys too), and is always at the tip: there is nothing to sync, enable or
 * rebuild apart. StoreHash leaves it out. Always on: what it costs is small next to the state it
 * indexes (doc/bitassets.md, "The search index").
 *
 * Keys, after the table byte and a byte of their own (big-endian numbers, "~" a number inverted for a
 * descending order; seq is the number of an asset):
 *   'r' seq              -> AssetRow, one per asset ever registered (retired ones are kept, flagged)
 *   'n' 0|1 name 0 seq   -> Summary: by normalised name (1: no public name, after the named ones)
 *   't' word 0 seq       -> Summary: by each word that starts after the start of the name
 *   'm' display 0 seq    -> Summary: by normalised display name (those with one)
 *   'u' word 0 seq       -> Summary: by each word that starts after the start of the display name
 *   'o' ~seq             -> Summary: newest first
 *   's' ~supply seq      -> Summary
 *   'a' ~last seq        -> Summary: the height of the last block that touched it
 *   'v' ~volume seq      -> Summary: those traded within the window only
 *   'd' ~depth seq       -> Summary: those with a pool with CHN that someone provides for only
 *   'h' height seq       -> volume of an asset in a block of the window, until it leaves the window
 *   'p' pool             -> PoolRow;  'D' ~depth, 'V' ~volume, 'C' ~change, 'N' ~created, then pool
 *   'H' height pool      -> PoolSnap: a pool in a block of the window
 *   'E' end auction      -> auctions not closed, by their last block;  'B' ~created auction -> all
 *   'i'                  -> Info: counts;  'L' -> INDEX_LAYOUT, written with the first entry
 */
namespace bitassets::index {

/**
 * The layout of the index. A store with assets and an index of another layout, or none (one written by
 * a version without it), is not read: the node derives the sidechain state again from the blocks at
 * startup (LayoutCurrent), so that the index covers every block; but see UpgradeLayout.
 *  1: the first.
 *  2: display names: in each row (AssetRow::display, written after the rest: a row of 1 reads as one
 *     without), the flag HAS_DISPLAY, the orders 'm' and 'u'.
 */
inline constexpr uint32_t INDEX_LAYOUT{2};
/** Whether the index in a store is of this layout, or there are no assets to index yet. */
bool LayoutCurrent(const sidechain::StoreView& view);
/**
 * Bring an index of layout 1 to this layout in place, if it can be: a store written by a version without
 * display names has none (no rule let a block set one), so its index is that of layout 2 already, its
 * rows read as rows without a display name. Nothing is derived again then: the layout is written, and
 * that is all. Returns whether it did; not if the store holds display names (it was not written by such
 * a version: the index is derived again, as for any other layout).
 */
bool UpgradeLayout(sidechain::StoreOverlay& store);

/** The most a page holds. */
inline constexpr size_t MAX_PAGE{200};
/** Index entries a search looks at, at most, per call: past it, it returns what it found with a cursor to go on. */
inline constexpr size_t SCAN_BUDGET{50'000};
/** Rows a search reads in full (for a name), at most, per call. */
inline constexpr size_t ROW_BUDGET{10'000};
/**
 * The most matches of a text a search sorts in memory (by supply, say); a text that matches more is
 * searched along the index of that order, with the text as a filter.
 */
inline constexpr size_t CANDIDATE_CAP{5'000};

enum Flag : uint8_t {
    FIXED = 1,    //!< its supply is fixed for good
    RETIRED = 2,  //!< retired (ReleaseAsset): its name may be another asset's now
    HAS_LOGO = 4,
    HAS_POOL = 8, //!< a pool someone provides liquidity to
    NAMED = 16,   //!< its registration published its name
    HAS_DISPLAY = 32, //!< it has a display name
};

/** What the filters read, kept with each entry of an order so that filtering reads no row. */
struct Summary {
    uint8_t flags{0};
    uint8_t decimals{0};
    int32_t created{0};
    int32_t last{0};
    uint64_t supply{0};
    uint64_t depth{0};
    SERIALIZE_METHODS(Summary, obj) { READWRITE(obj.flags, obj.decimals, VARINT_MODE(obj.created, VarIntMode::NONNEGATIVE_SIGNED), VARINT_MODE(obj.last, VarIntMode::NONNEGATIVE_SIGNED), VARINT(obj.supply), VARINT(obj.depth)); }
    friend bool operator==(const Summary&, const Summary&) = default;
};

struct AssetRow {
    uint32_t seq{0};
    AssetId id;
    std::string text;
    uint8_t decimals{0};
    uint8_t flags{0};
    //! The height of the block that registered it, and of the last block that touched it: a transfer,
    //! a mint, a burn, a change of its data or logo, a trade, its pools, its auctions.
    int32_t created{0};
    int32_t last{0};
    uint64_t supply{0};
    uint64_t minted{0};
    uint64_t burned{0};
    //! The CHN in its pool with CHN, if someone provides liquidity to it.
    uint64_t depth{0};
    //! The CHN traded in its pools with CHN (each trade at the pool's price after it) and in its
    //! auctions for CHN, over the last SidechainParams::asset_volume_window blocks.
    uint64_t volume{0};
    //! Its pools (any pair) that someone provides liquidity to.
    uint32_t pools{0};
    //! Its display name, if any (layout 2).
    std::string display;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << seq << id << text << decimals << flags << VARINT_MODE(created, VarIntMode::NONNEGATIVE_SIGNED) << VARINT_MODE(last, VarIntMode::NONNEGATIVE_SIGNED) << VARINT(supply)
          << VARINT(minted) << VARINT(burned) << VARINT(depth) << VARINT(volume) << VARINT(pools);
        // Last, and only if there is one: a row without is written as in layout 1.
        if (!display.empty()) s << display;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> seq >> id >> text >> decimals >> flags >> VARINT_MODE(created, VarIntMode::NONNEGATIVE_SIGNED) >> VARINT_MODE(last, VarIntMode::NONNEGATIVE_SIGNED) >> VARINT(supply)
          >> VARINT(minted) >> VARINT(burned) >> VARINT(depth) >> VARINT(volume) >> VARINT(pools);
        display.clear();
        if (!s.empty()) s >> display;
    }
    friend bool operator==(const AssetRow&, const AssetRow&) = default;
    Summary Summarize() const { return {flags, decimals, created, last, supply, depth}; }
};

struct PoolRow {
    uint256 id;
    AssetId asset0;
    AssetId asset1;
    int32_t created{0};
    int32_t last{0};
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    //! The reserves at the start of the window (or when it was made, if later): what the change of price is against.
    uint64_t ref0{0};
    uint64_t ref1{0};
    uint64_t volume{0};
    bool abandoned{false};
    SERIALIZE_METHODS(PoolRow, obj)
    {
        READWRITE(obj.id, obj.asset0, obj.asset1, VARINT_MODE(obj.created, VarIntMode::NONNEGATIVE_SIGNED), VARINT_MODE(obj.last, VarIntMode::NONNEGATIVE_SIGNED), VARINT(obj.reserve0), VARINT(obj.reserve1),
                  VARINT(obj.ref0), VARINT(obj.ref1), VARINT(obj.volume), obj.abandoned);
    }
    friend bool operator==(const PoolRow&, const PoolRow&) = default;
    /** For a pool with CHN, the CHN it holds; 0 for another pair (no price in CHN to count it in). */
    uint64_t Depth() const { return asset0.IsNull() && !abandoned ? reserve0 : 0; }
    /** The change of the price of asset1 in asset0 over the window, in millionths. */
    int64_t ChangePpm() const;
};

struct PoolSnap {
    uint64_t volume{0};
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    SERIALIZE_METHODS(PoolSnap, obj) { READWRITE(VARINT(obj.volume), VARINT(obj.reserve0), VARINT(obj.reserve1)); }
};

struct Info {
    uint64_t assets{0};
    uint64_t retired{0};
    uint64_t pools{0};
    SERIALIZE_METHODS(Info, obj) { READWRITE(VARINT(obj.assets), VARINT(obj.retired), VARINT(obj.pools)); }
};

/** Text as the index compares it: ASCII lowercased, spaces at the ends dropped. */
std::string Normalize(std::string_view text);
/** The words of normalised text: runs of letters and digits, each once, in order. */
std::vector<std::string> Words(std::string_view norm);
/** The words that start after the start of normalised text, each once: what the index lists a name under ('t'). */
std::vector<std::string> LaterWords(std::string_view norm);

/**
 * Bring the index up to date with a block connected at `height`, from the journal of `store` (the
 * overlay of the block). Nothing if the overlay keeps no journal (nothing to tell what changed).
 */
void ConnectBlock(sidechain::StoreOverlay& store, int height, int volume_window);

std::optional<AssetRow> GetAssetRow(const sidechain::StoreView& view, uint32_t seq);
std::optional<PoolRow> GetPoolRow(const sidechain::StoreView& view, const uint256& id);
Info GetInfo(const sidechain::StoreView& view);
/** Write a row (nullopt: erase it), its entries in every order moved with it (for tests and benchmarks: blocks do it themselves). */
void PutAssetRow(sidechain::StoreOverlay& store, const std::optional<AssetRow>& old_row, const std::optional<AssetRow>& row);

enum class Sort : uint8_t { RELEVANCE, NAME, NEWEST, OLDEST, SUPPLY, ACTIVITY, VOLUME, POOL_DEPTH };
std::optional<Sort> ParseSort(const std::string& name);

struct Filters {
    //! In whole units of each asset.
    std::optional<double> min_supply;
    bool has_pool{false};
    bool has_logo{false};
    bool exclude_burned{false};
    bool exclude_retired{true};
    //! Touched at this height or later.
    std::optional<int> min_last;
    //! Registered after this height.
    std::optional<int> created_after;
    //! Normalised words: an asset with any of them among the words of its name or display name is left out.
    std::vector<std::string> exclude_keywords;
    //! Only these (the wallet's, say).
    std::optional<std::set<uint32_t>> only;
};

struct Request {
    //! Text matched against the names (the whole name, its start, the start of any of its words), then the display names (their start, the start of any of their words).
    std::string text;
    //! These assets only, by number (a search for a number or a hash): no text then.
    std::optional<std::vector<uint32_t>> exact;
    Sort sort{Sort::RELEVANCE};
    Filters filters;
    std::vector<unsigned char> cursor;
    size_t limit{50};
};

struct Hit {
    AssetRow row;
    //! How the text matched (RowMatch): 0 the whole name, 1 its start, 2 the start of a word, 3 the start of the display name, 4 of a word of it; -1 no text.
    int match{-1};
};

struct Page {
    std::vector<Hit> hits;
    //! Where the next page starts; empty if there is no more.
    std::vector<unsigned char> next;
    //! Index entries looked at.
    size_t scanned{0};
    //! The search stopped at its budget before the page was full: `next` goes on from there.
    bool partial{false};
};

/** How a name matches normalised text: 0 equal, 1 starts with it, 2 a word of it (not the first) starts with it; nullopt if not. */
std::optional<int> MatchTier(std::string_view norm_name, std::string_view text);
/**
 * How an asset matches normalised text: by its name (MatchTier: 0, 1, 2), else by its display name
 * (3: equal or its start, 4: a later word of it); nullopt if neither.
 */
std::optional<int> RowMatch(const AssetRow& row, std::string_view text);

/** A page of the assets that match; throws std::invalid_argument for a cursor that is not one of this search. */
Page Search(const sidechain::StoreView& view, const Request& request);

/**
 * The assets a request matches, in its order, for a search of their pools or auctions: at most
 * `max_assets`, and at most SCAN_BUDGET index entries looked at over all the pages it takes (a text that
 * few names match, among millions, does not walk the whole index). `partial` if it stopped before the end.
 */
std::vector<AssetId> MatchingAssets(const sidechain::StoreView& view, Request request, size_t max_assets, bool& partial);

enum class PoolSort : uint8_t { DEPTH, VOLUME, CHANGE, NEWEST };
std::optional<PoolSort> ParsePoolSort(const std::string& name);
struct PoolRequest {
    //! The assets whose pools are wanted (a text's matches), if any: their pools only.
    std::optional<std::vector<AssetId>> assets;
    PoolSort sort{PoolSort::DEPTH};
    bool exclude_abandoned{true};
    bool chn_only{false};
    uint64_t min_depth{0};
    std::vector<unsigned char> cursor;
    size_t limit{50};
};
struct PoolPage {
    std::vector<PoolRow> pools;
    std::vector<unsigned char> next;
    size_t scanned{0};
    bool partial{false};
};
PoolPage SearchPools(const sidechain::StoreView& view, const PoolRequest& request);

enum class AuctionSort : uint8_t { ENDING_SOON, NEWEST };
std::optional<AuctionSort> ParseAuctionSort(const std::string& name);
struct AuctionRequest {
    std::optional<std::vector<AssetId>> assets;
    AuctionSort sort{AuctionSort::ENDING_SOON};
    //! The height of the next block: what is open, upcoming or ended is as of it.
    int height{0};
    //! "open", "upcoming", "active" (open or upcoming), "ended" (not collected yet) or "any" (not closed); closed ones only with include_closed and sort NEWEST.
    std::string status{"active"};
    bool include_closed{false};
    std::vector<unsigned char> cursor;
    size_t limit{50};
};
struct AuctionPage {
    std::vector<std::pair<Txid, Auction>> auctions;
    std::vector<unsigned char> next;
    size_t scanned{0};
    bool partial{false};
};
AuctionPage SearchAuctions(const sidechain::StoreView& view, const AuctionRequest& request);

} // namespace bitassets::index

#endif // BITCOIN_BITASSETS_INDEX_H

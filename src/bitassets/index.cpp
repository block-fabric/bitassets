// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/index.h>

#include <util/check.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>

namespace bitassets::index {
namespace {

using Bytes = sidechain::StoreBytes;

const sidechain::TableId INDEX_TABLE{sidechain::DERIVED_TABLE};

// The tables of the state the index reads the keys of in a block's journal (bitassets/state.cpp).
constexpr uint8_t ST_ASSETS{0x31};
constexpr uint8_t ST_POOLS{0x34};
constexpr uint8_t ST_AUCTIONS{0x35};
constexpr uint8_t ST_TOKENS_BY_ID{0x39};
constexpr uint8_t ST_HISTORY{0x3e};
constexpr uint8_t ST_LOGOS{0x90};

constexpr uint8_t CURSOR_VERSION{1};

Bytes K(char sub) { return Bytes{sidechain::DERIVED_TABLE, static_cast<unsigned char>(sub)}; }
void Put32(Bytes& out, uint32_t v) { sidechain::KeyCodec<uint32_t>::Encode(out, v); }
void Put64(Bytes& out, uint64_t v) { sidechain::KeyCodec<uint64_t>::Encode(out, v); }
void PutHash(Bytes& out, const uint256& v) { out.insert(out.end(), v.begin(), v.end()); }
uint32_t Get32(std::span<const unsigned char> in) { return sidechain::KeyCodec<uint32_t>::Decode(in); }
uint256 GetHash(std::span<const unsigned char> in)
{
    uint256 v;
    std::copy(in.begin(), in.begin() + 32, v.begin());
    return v;
}
uint32_t Height32(int32_t h) { return static_cast<uint32_t>(std::max(h, 0)); }

/** The number of an asset at the end of a key of an order of assets. */
uint32_t SeqOfKey(std::span<const unsigned char> key)
{
    if (key.size() < 6) throw std::ios_base::failure("index key too short");
    // 'o': the number inverted; 'r': the number after the two first bytes; the others end with it.
    if (key[1] == 'o') return ~Get32(key.subspan(2));
    return Get32(key.subspan(key.size() - 4));
}

Bytes RowKey(uint32_t seq)
{
    Bytes key{K('r')};
    Put32(key, seq);
    return key;
}

Bytes NameKey(const AssetRow& row, const std::string& norm)
{
    Bytes key{K('n')};
    const bool named{(row.flags & NAMED) != 0};
    key.push_back(named ? 0 : 1);
    if (named) key.insert(key.end(), norm.begin(), norm.end());
    key.push_back(0);
    Put32(key, row.seq);
    return key;
}

/** The key of an asset in an order (NAME, NEWEST, ...); nullopt if the order leaves it out (no volume, no depth). */
std::optional<Bytes> OrderKey(Sort sort, const AssetRow& row, const std::string& norm)
{
    Bytes key;
    switch (sort) {
    case Sort::NAME: return NameKey(row, norm);
    case Sort::NEWEST: key = K('o'); Put32(key, ~row.seq); return key;
    case Sort::OLDEST: return RowKey(row.seq);
    case Sort::SUPPLY: key = K('s'); Put64(key, ~row.supply); break;
    case Sort::ACTIVITY: case Sort::RELEVANCE: key = K('a'); Put32(key, ~Height32(row.last)); break;
    case Sort::VOLUME:
        if (row.volume == 0) return std::nullopt;
        key = K('v'); Put64(key, ~row.volume); break;
    case Sort::POOL_DEPTH:
        if (row.depth == 0) return std::nullopt;
        key = K('d'); Put64(key, ~row.depth); break;
    }
    Put32(key, row.seq);
    return key;
}

/** Every entry of a row in the orders (the row itself apart), with its value. */
std::map<Bytes, Bytes> OrderEntries(const AssetRow& row)
{
    std::map<Bytes, Bytes> entries;
    const Bytes summary{sidechain::EncodeValue(row.Summarize())};
    const std::string norm{Normalize(row.text)};
    for (const Sort sort : {Sort::NAME, Sort::NEWEST, Sort::SUPPLY, Sort::ACTIVITY, Sort::VOLUME, Sort::POOL_DEPTH}) {
        if (auto key{OrderKey(sort, row, norm)}) entries.emplace(std::move(*key), summary);
    }
    if (row.flags & NAMED) {
        // The word the name starts with is found by the name's own entry, unless it is also further on.
        for (const std::string& word : LaterWords(norm)) {
            Bytes key{K('t')};
            key.insert(key.end(), word.begin(), word.end());
            key.push_back(0);
            Put32(key, row.seq);
            entries.emplace(std::move(key), summary);
        }
    }
    return entries;
}

Bytes PoolOrderKey(PoolSort sort, const PoolRow& row)
{
    Bytes key;
    switch (sort) {
    case PoolSort::DEPTH: key = K('D'); Put64(key, ~row.Depth()); break;
    case PoolSort::VOLUME: key = K('V'); Put64(key, ~row.volume); break;
    case PoolSort::CHANGE: key = K('C'); Put64(key, ~(static_cast<uint64_t>(row.ChangePpm()) + (uint64_t{1} << 63))); break;
    case PoolSort::NEWEST: key = K('N'); Put32(key, ~Height32(row.created)); break;
    }
    PutHash(key, row.id);
    return key;
}

std::set<Bytes> PoolOrderKeys(const PoolRow& row)
{
    std::set<Bytes> keys{PoolOrderKey(PoolSort::CHANGE, row), PoolOrderKey(PoolSort::NEWEST, row)};
    if (row.Depth() > 0) keys.insert(PoolOrderKey(PoolSort::DEPTH, row));
    if (row.volume > 0) keys.insert(PoolOrderKey(PoolSort::VOLUME, row));
    return keys;
}

Bytes PoolKey(const uint256& id)
{
    Bytes key{K('p')};
    PutHash(key, id);
    return key;
}

void PutPoolRow(sidechain::StoreOverlay& store, const std::optional<PoolRow>& old_row, const std::optional<PoolRow>& row)
{
    const std::set<Bytes> old_keys{old_row ? PoolOrderKeys(*old_row) : std::set<Bytes>{}};
    const std::set<Bytes> new_keys{row ? PoolOrderKeys(*row) : std::set<Bytes>{}};
    for (const Bytes& key : old_keys) {
        if (!new_keys.contains(key)) store.Erase(key);
    }
    for (const Bytes& key : new_keys) {
        if (!old_keys.contains(key)) store.Put(key, {});
    }
    const uint256& id{row ? row->id : Assert(old_row)->id};
    if (row) {
        if (!old_row || *old_row != *row) store.Put(PoolKey(id), sidechain::EncodeValue(*row));
    } else {
        store.Erase(PoolKey(id));
    }
}

Bytes AuctionEndKey(const Txid& id, const Auction& auction)
{
    Bytes key{K('E')};
    Put32(key, static_cast<uint32_t>(std::clamp<int64_t>(auction.EndHeight(), 0, std::numeric_limits<uint32_t>::max())));
    PutHash(key, id.ToUint256());
    return key;
}

Bytes AuctionNewKey(const Txid& id, const Auction& auction)
{
    Bytes key{K('B')};
    Put32(key, ~Height32(auction.created));
    PutHash(key, id.ToUint256());
    return key;
}

std::set<Bytes> AuctionKeys(const Txid& id, const Auction& auction)
{
    std::set<Bytes> keys{AuctionNewKey(id, auction)};
    if (!auction.closed) keys.insert(AuctionEndKey(id, auction));
    return keys;
}

const Bytes INFO_KEY{K('i')};
const Bytes LAYOUT_KEY{K('L')};

uint64_t Sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
uint64_t Add(uint64_t a, uint64_t b) { return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b; }
uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t c)
{
    if (c == 0) return 0;
    const unsigned __int128 r{static_cast<unsigned __int128>(a) * b / c};
    return r > std::numeric_limits<uint64_t>::max() ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(r);
}

/** Rows read once, changed in memory, written once at the end of a block. */
template <typename Id, typename Row>
struct RowCache {
    std::map<Id, std::pair<std::optional<Row>, std::optional<Row>>> rows;
    std::function<std::optional<Row>(const Id&)> read;
    std::optional<Row>& Get(const Id& id)
    {
        auto it{rows.find(id)};
        if (it == rows.end()) {
            auto row{read(id)};
            it = rows.emplace(id, std::make_pair(row, row)).first;
        }
        return it->second.second;
    }
};

} // namespace

int64_t PoolRow::ChangePpm() const
{
    if (ref0 == 0 || ref1 == 0 || reserve0 == 0 || reserve1 == 0) return 0;
    const long double ratio{(static_cast<long double>(reserve0) * ref1) / (static_cast<long double>(reserve1) * ref0)};
    const long double ppm{(ratio - 1) * 1e6L};
    return static_cast<int64_t>(std::clamp<long double>(ppm, -1e15L, 1e15L));
}

std::string Normalize(std::string_view text)
{
    size_t begin{0}, end{text.size()};
    while (begin < end && text[begin] == ' ') ++begin;
    while (end > begin && text[end - 1] == ' ') --end;
    std::string out{text.substr(begin, end - begin)};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

namespace {
bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }
} // namespace

std::vector<std::string> Words(std::string_view norm)
{
    std::vector<std::string> words;
    size_t i{0};
    while (i < norm.size()) {
        while (i < norm.size() && !IsWordChar(norm[i])) ++i;
        const size_t start{i};
        while (i < norm.size() && IsWordChar(norm[i])) ++i;
        if (i > start) {
            std::string word{norm.substr(start, i - start)};
            if (std::find(words.begin(), words.end(), word) == words.end()) words.push_back(std::move(word));
        }
    }
    return words;
}

std::vector<std::string> LaterWords(std::string_view norm)
{
    std::vector<std::string> words;
    for (size_t i{1}; i < norm.size(); ++i) {
        if (!IsWordChar(norm[i]) || IsWordChar(norm[i - 1])) continue;
        size_t end{i};
        while (end < norm.size() && IsWordChar(norm[end])) ++end;
        std::string word{norm.substr(i, end - i)};
        if (std::find(words.begin(), words.end(), word) == words.end()) words.push_back(std::move(word));
        i = end;
    }
    return words;
}

std::optional<int> MatchTier(std::string_view name, std::string_view text)
{
    if (text.empty()) return std::nullopt;
    if (name == text) return 0;
    if (name.starts_with(text)) return 1;
    // The start of a word: a letter or digit after anything else.
    for (size_t p{1}; p + text.size() <= name.size(); ++p) {
        if (IsWordChar(name[p]) && !IsWordChar(name[p - 1]) && name.substr(p).starts_with(text)) return 2;
    }
    return std::nullopt;
}

std::optional<AssetRow> GetAssetRow(const sidechain::StoreView& view, uint32_t seq)
{
    const auto bytes{view.Get(RowKey(seq))};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<AssetRow>(*bytes);
}

std::optional<PoolRow> GetPoolRow(const sidechain::StoreView& view, const uint256& id)
{
    const auto bytes{view.Get(PoolKey(id))};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<PoolRow>(*bytes);
}

bool LayoutCurrent(const sidechain::StoreView& view)
{
    if (view.Get(LAYOUT_KEY) == sidechain::EncodeValue(INDEX_LAYOUT)) return true;
    // Assets without an index of this layout: a store written by another version.
    const Bytes assets{ST_ASSETS};
    return !view.Next(assets, assets).has_value();
}

Info GetInfo(const sidechain::StoreView& view)
{
    const auto bytes{view.Get(INFO_KEY)};
    return bytes ? sidechain::DecodeValue<Info>(*bytes) : Info{};
}

void PutAssetRow(sidechain::StoreOverlay& store, const std::optional<AssetRow>& old_row, const std::optional<AssetRow>& row)
{
    const std::map<Bytes, Bytes> old_entries{old_row ? OrderEntries(*old_row) : std::map<Bytes, Bytes>{}};
    const std::map<Bytes, Bytes> new_entries{row ? OrderEntries(*row) : std::map<Bytes, Bytes>{}};
    for (const auto& [key, value] : old_entries) {
        if (!new_entries.contains(key)) store.Erase(key);
    }
    for (const auto& [key, value] : new_entries) {
        const auto it{old_entries.find(key)};
        if (it == old_entries.end() || it->second != value) store.Put(key, value);
    }
    const uint32_t seq{row ? row->seq : Assert(old_row)->seq};
    if (row) {
        if (!old_row || *old_row != *row) store.Put(RowKey(seq), sidechain::EncodeValue(*row));
    } else {
        store.Erase(RowKey(seq));
    }
}

void ConnectBlock(sidechain::StoreOverlay& store, int height, int volume_window)
{
    if (!store.Journaling()) return;
    const State state{static_cast<const sidechain::StoreView&>(store)};

    // What the block touched, with the earlier values of what it changed.
    std::set<AssetId> assets;
    std::map<AssetId, std::optional<AssetRecord>> old_records;
    std::set<uint256> pools;
    std::map<uint256, std::optional<Pool>> old_pools;
    std::map<uint256, std::optional<Auction>> old_auctions;
    for (const auto& [key, old] : store.Journal().entries) {
        if (key.size() < 33) continue;
        const uint256 id{GetHash(std::span{key}.subspan(1))};
        switch (key[0]) {
        case ST_ASSETS:
            assets.insert(id);
            old_records.emplace(id, old ? std::optional{sidechain::DecodeValue<AssetRecord>(*old)} : std::nullopt);
            break;
        case ST_TOKENS_BY_ID:
            if (key.size() < 34) break;
            if (key[33] == static_cast<uint8_t>(Token::Kind::ASSET) || key[33] == static_cast<uint8_t>(Token::Kind::CONTROL)) assets.insert(id);
            if (key[33] == static_cast<uint8_t>(Token::Kind::LP)) pools.insert(id);
            break;
        case ST_POOLS:
            pools.insert(id);
            old_pools.emplace(id, old ? std::optional{sidechain::DecodeValue<Pool>(*old)} : std::nullopt);
            break;
        case ST_AUCTIONS:
            old_auctions.emplace(id, old ? std::optional{sidechain::DecodeValue<Auction>(*old)} : std::nullopt);
            break;
        case ST_HISTORY:
        case ST_LOGOS:
            assets.insert(id);
            break;
        }
    }

    std::map<AssetId, uint64_t> asset_volume;
    std::map<uint256, std::pair<std::optional<Pool>, uint64_t>> pool_now;
    for (const uint256& id : pools) {
        const std::optional<Pool> now{state.GetPool(id)};
        const auto it{old_pools.find(id)};
        const std::optional<Pool> before{it != old_pools.end() ? it->second : now};
        uint64_t volume{0};
        for (const auto* p : {&now, &before}) {
            if (!*p) continue;
            assets.insert((*p)->asset0);
            assets.insert((*p)->asset1);
        }
        if (now && now->asset0.IsNull()) {
            // What was traded in, of each side, in CHN: the CHN, and the asset at the pool's price after.
            const bool same{before && before->asset0 == now->asset0 && before->asset1 == now->asset1};
            const uint64_t in0{Sub(now->volume0, same ? before->volume0 : 0)};
            const uint64_t in1{Sub(now->volume1, same ? before->volume1 : 0)};
            volume = Add(in0, MulDiv(in1, now->reserve0, now->reserve1));
            if (volume > 0) asset_volume[now->asset1] = Add(asset_volume[now->asset1], volume);
        }
        pool_now.emplace(id, std::make_pair(now, volume));
    }
    for (const auto& [id, before] : old_auctions) {
        const Txid txid{Txid::FromUint256(id)};
        const std::optional<Auction> now{state.GetAuction(txid)};
        for (const auto* a : {&now, &before}) {
            if (!*a) continue;
            assets.insert((*a)->base);
            assets.insert((*a)->quote);
        }
        if (now && (!before || now->bids > before->bids)) {
            if (now->quote.IsNull() && !now->base.IsNull()) {
                const uint64_t paid{Sub(now->proceeds, before ? before->proceeds : 0)};
                if (paid > 0) asset_volume[now->base] = Add(asset_volume[now->base], paid);
            } else if (now->base.IsNull() && !now->quote.IsNull()) {
                const uint64_t sold{Sub(before ? before->remaining : now->base_amount, now->remaining)};
                if (sold > 0) asset_volume[now->quote] = Add(asset_volume[now->quote], sold);
            }
        }
        // The auction's own entries.
        const std::set<Bytes> old_keys{before ? AuctionKeys(txid, *before) : std::set<Bytes>{}};
        const std::set<Bytes> new_keys{now ? AuctionKeys(txid, *now) : std::set<Bytes>{}};
        for (const Bytes& key : old_keys) {
            if (!new_keys.contains(key)) store.Erase(key);
        }
        for (const Bytes& key : new_keys) {
            if (!old_keys.contains(key)) store.Put(key, {});
        }
    }
    assets.erase(CHN);
    // The layout, with the first entry (LayoutCurrent).
    if (!assets.empty() || !pools.empty() || !old_auctions.empty()) {
        const Bytes layout{sidechain::EncodeValue(INDEX_LAYOUT)};
        if (store.Get(LAYOUT_KEY) != layout) store.Put(LAYOUT_KEY, layout);
    }

    Info info{GetInfo(store)};
    const Info info_before{info};
    RowCache<uint32_t, AssetRow> rows;
    rows.read = [&](const uint32_t& seq) { return GetAssetRow(store, seq); };
    RowCache<uint256, PoolRow> pool_rows;
    pool_rows.read = [&](const uint256& id) { return GetPoolRow(store, id); };

    for (const AssetId& id : assets) {
        const std::optional<AssetRecord> record{state.GetAsset(id)};
        const auto it{old_records.find(id)};
        const std::optional<AssetRecord> before{it != old_records.end() ? it->second : record};
        if (before && (!record || record->seq != before->seq)) {
            // Retired (and maybe registered again, under another number).
            if (auto& row{rows.Get(before->seq)}; row && !(row->flags & RETIRED)) {
                row->flags = (row->flags | RETIRED) & ~HAS_POOL & ~HAS_LOGO;
                row->depth = 0;
                row->pools = 0;
                row->last = height;
                ++info.retired;
            }
        }
        if (!record) continue;
        auto& row{rows.Get(record->seq)};
        if (!row) {
            row = AssetRow{};
            row->seq = record->seq;
            row->id = id;
            row->created = record->height;
            ++info.assets;
        }
        row->text = record->text;
        row->decimals = record->decimals;
        row->supply = record->supply;
        row->minted = record->minted;
        row->burned = record->burned;
        row->pools = record->provided_pools;
        uint8_t flags{0};
        if (record->fixed) flags |= FIXED;
        if (!record->text.empty()) flags |= NAMED;
        if (record->provided_pools > 0) flags |= HAS_POOL;
        if (state.GetLogo(id)) flags |= HAS_LOGO;
        row->flags = flags;
        const std::optional<Pool> chn_pool{state.FindPool(id, CHN)};
        row->depth = chn_pool && !amm::Abandoned(*chn_pool) ? chn_pool->reserve0 : 0;
        row->last = height;
        if (const auto v{asset_volume.find(id)}; v != asset_volume.end() && volume_window > 0) {
            row->volume = Add(row->volume, v->second);
            Bytes key{K('h')};
            Put32(key, Height32(height));
            Put32(key, record->seq);
            store.Put(key, sidechain::EncodeValue(VARINT(v->second)));
        }
    }

    for (const auto& [id, entry] : pool_now) {
        const auto& [now, volume] = entry;
        auto& row{pool_rows.Get(id)};
        if (!now) {
            if (row) {
                row.reset();
                info.pools = Sub(info.pools, 1);
            }
            continue;
        }
        if (!row || row->asset0 != now->asset0 || row->asset1 != now->asset1) {
            if (!row) ++info.pools;
            row = PoolRow{};
            row->id = id;
            row->asset0 = now->asset0;
            row->asset1 = now->asset1;
            row->created = now->height;
            row->ref0 = now->reserve0;
            row->ref1 = now->reserve1;
        }
        row->reserve0 = now->reserve0;
        row->reserve1 = now->reserve1;
        row->abandoned = amm::Abandoned(*now);
        row->last = height;
        if (volume_window > 0) {
            row->volume = Add(row->volume, volume);
            Bytes key{K('H')};
            Put32(key, Height32(height));
            PutHash(key, id);
            store.Put(key, sidechain::EncodeValue(PoolSnap{volume, now->reserve0, now->reserve1}));
        }
    }

    // What leaves the window: the blocks `volume_window` back.
    if (volume_window > 0 && height - volume_window >= 0) {
        const uint32_t gone{Height32(height - volume_window)};
        Bytes prefix{K('h')};
        Put32(prefix, gone);
        std::vector<Bytes> erase;
        store.ForEach(prefix, [&](const Bytes& key, const Bytes& value) {
            const uint32_t seq{Get32(std::span{key}.subspan(6))};
            uint64_t v{0};
            SpanReader{value} >> VARINT(v);
            if (auto& row{rows.Get(seq)}) row->volume = Sub(row->volume, v);
            erase.push_back(key);
            return true;
        });
        prefix = K('H');
        Put32(prefix, gone);
        store.ForEach(prefix, [&](const Bytes& key, const Bytes& value) {
            const uint256 id{GetHash(std::span{key}.subspan(6))};
            const PoolSnap snap{sidechain::DecodeValue<PoolSnap>(value)};
            if (auto& row{pool_rows.Get(id)}) {
                row->volume = Sub(row->volume, snap.volume);
                // The price at the end of that block: the latest the pool had at the start of the window.
                row->ref0 = snap.reserve0;
                row->ref1 = snap.reserve1;
            }
            erase.push_back(key);
            return true;
        });
        for (const Bytes& key : erase) store.Erase(key);
    }

    for (const auto& [seq, entry] : rows.rows) {
        if (entry.first != entry.second) PutAssetRow(store, entry.first, entry.second);
    }
    for (const auto& [id, entry] : pool_rows.rows) {
        if (entry.first != entry.second) PutPoolRow(store, entry.first, entry.second);
    }
    if (info.assets != info_before.assets || info.retired != info_before.retired || info.pools != info_before.pools) {
        store.Put(INFO_KEY, sidechain::EncodeValue(info));
    }
}

std::optional<Sort> ParseSort(const std::string& name)
{
    static const std::map<std::string, Sort> sorts{
        {"relevance", Sort::RELEVANCE}, {"name", Sort::NAME}, {"newest", Sort::NEWEST}, {"oldest", Sort::OLDEST},
        {"supply", Sort::SUPPLY}, {"activity", Sort::ACTIVITY}, {"volume", Sort::VOLUME}, {"pool_depth", Sort::POOL_DEPTH}};
    const auto it{sorts.find(name)};
    return it == sorts.end() ? std::nullopt : std::optional{it->second};
}

std::optional<PoolSort> ParsePoolSort(const std::string& name)
{
    static const std::map<std::string, PoolSort> sorts{
        {"depth", PoolSort::DEPTH}, {"volume", PoolSort::VOLUME}, {"price_change", PoolSort::CHANGE}, {"newest", PoolSort::NEWEST}};
    const auto it{sorts.find(name)};
    return it == sorts.end() ? std::nullopt : std::optional{it->second};
}

std::optional<AuctionSort> ParseAuctionSort(const std::string& name)
{
    if (name == "ending_soon") return AuctionSort::ENDING_SOON;
    if (name == "newest") return AuctionSort::NEWEST;
    return std::nullopt;
}

namespace {

/** A cursor: what search it belongs to, then where it goes on from. */
struct Cursor {
    uint8_t kind{0};
    uint8_t mode{0};
    uint8_t sort{0};
    uint8_t phase{0};
    Bytes key;
};

Bytes EncodeCursor(const Cursor& c)
{
    Bytes out{CURSOR_VERSION, c.kind, c.mode, c.sort, c.phase};
    out.insert(out.end(), c.key.begin(), c.key.end());
    return out;
}

std::optional<Cursor> DecodeCursor(const Bytes& bytes, uint8_t kind, uint8_t sort)
{
    if (bytes.empty()) return std::nullopt;
    if (bytes.size() < 5 || bytes[0] != CURSOR_VERSION || bytes[1] != kind || bytes[3] != sort) {
        throw std::invalid_argument("The cursor is not one of this search (same query, filters and sort)");
    }
    return Cursor{bytes[1], bytes[2], bytes[3], bytes[4], Bytes(bytes.begin() + 5, bytes.end())};
}

constexpr uint8_t MODE_SCAN{'S'};
constexpr uint8_t MODE_CANDIDATES{'C'};
//! In the phase of a cursor: it goes on at its key, not after it.
constexpr uint8_t AT_KEY{0x80};

/** The key just after `key` among keys that start with it. */
Bytes After(Bytes key)
{
    key.push_back(0);
    return key;
}

bool PassSummary(const Summary& s, const Filters& f, uint32_t seq)
{
    if (f.exclude_retired && (s.flags & RETIRED)) return false;
    if (f.has_pool && !(s.flags & HAS_POOL)) return false;
    if (f.has_logo && !(s.flags & HAS_LOGO)) return false;
    if (f.exclude_burned && s.supply == 0) return false;
    if (f.min_last && s.last < *f.min_last) return false;
    if (f.created_after && s.created <= *f.created_after) return false;
    if (f.min_supply && static_cast<double>(s.supply) / std::pow(10.0, s.decimals) < *f.min_supply) return false;
    if (f.only && !f.only->contains(seq)) return false;
    return true;
}

bool PassKeywords(const std::string& norm, const Filters& f)
{
    if (f.exclude_keywords.empty()) return true;
    for (const std::string& word : Words(norm)) {
        if (std::find(f.exclude_keywords.begin(), f.exclude_keywords.end(), word) != f.exclude_keywords.end()) return false;
    }
    return true;
}

/** The first word of normalised text, if it starts with one. */
std::string FirstWord(const std::string& text)
{
    size_t end{0};
    while (end < text.size() && IsWordChar(text[end])) ++end;
    return text.substr(0, end);
}

/** The prefix an order of assets is under. */
Bytes OrderPrefix(Sort sort)
{
    switch (sort) {
    case Sort::NAME: return K('n');
    case Sort::NEWEST: return K('o');
    case Sort::OLDEST: return K('r');
    case Sort::SUPPLY: return K('s');
    case Sort::RELEVANCE: case Sort::ACTIVITY: return K('a');
    case Sort::VOLUME: return K('v');
    case Sort::POOL_DEPTH: return K('d');
    }
    assert(false);
}

/** Sort a set of candidates in memory, by the key of the order, and take the page after the cursor. */
template <typename Entry>
void PageOf(std::vector<std::pair<Bytes, Entry>>& found, const std::optional<Cursor>& cursor, size_t limit, std::vector<Entry>& out, Bytes& next_key, bool& more)
{
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto it{found.begin()};
    if (cursor) it = std::upper_bound(found.begin(), found.end(), cursor->key, [](const Bytes& k, const auto& e) { return k < e.first; });
    for (; it != found.end() && out.size() < limit; ++it) {
        out.push_back(it->second);
        next_key = it->first;
    }
    more = it != found.end();
}

} // namespace

Page Search(const sidechain::StoreView& view, const Request& request)
{
    Page page;
    const size_t limit{std::clamp<size_t>(request.limit, 1, MAX_PAGE)};
    const Filters& f{request.filters};
    const std::string text{Normalize(request.text)};
    const Sort sort{request.sort == Sort::RELEVANCE && text.empty() ? Sort::ACTIVITY : request.sort};
    std::optional<Cursor> cursor{DecodeCursor(request.cursor, 'A', static_cast<uint8_t>(request.sort))};
    size_t rows_read{0};

    // Candidates sorted in memory: assets named one by one, the wallet's, or the matches of a text in another order than relevance.
    std::optional<std::set<uint32_t>> candidates;
    if (request.exact) {
        candidates.emplace(request.exact->begin(), request.exact->end());
    } else if (f.only && f.only->size() <= CANDIDATE_CAP) {
        candidates = *f.only;
    } else if (!text.empty() && sort != Sort::RELEVANCE && !(cursor && cursor->mode == MODE_SCAN)) {
        // The matches of the text, if not too many: by the name's start, then by a word's.
        std::set<uint32_t> found;
        bool too_many{false};
        Bytes prefix{K('n')};
        prefix.push_back(0);
        prefix.insert(prefix.end(), text.begin(), text.end());
        Bytes at{prefix};
        while (const auto entry{view.Next(at, prefix)}) {
            ++page.scanned;
            found.insert(SeqOfKey(entry->first));
            if (found.size() > CANDIDATE_CAP) { too_many = true; break; }
            at = After(entry->first);
        }
        const std::string word{FirstWord(text)};
        if (!too_many && !word.empty()) {
            prefix = K('t');
            prefix.insert(prefix.end(), word.begin(), word.end());
            at = prefix;
            while (const auto entry{view.Next(at, prefix)}) {
                ++page.scanned;
                found.insert(SeqOfKey(entry->first));
                if (found.size() > CANDIDATE_CAP) { too_many = true; break; }
                at = After(entry->first);
            }
        }
        if (!too_many) candidates = std::move(found);
    }

    if (candidates && !(cursor && cursor->mode != MODE_CANDIDATES)) {
        std::vector<std::pair<Bytes, Hit>> found;
        for (const uint32_t seq : *candidates) {
            ++page.scanned;
            auto row{GetAssetRow(view, seq)};
            if (!row || !PassSummary(row->Summarize(), f, seq)) continue;
            const std::string norm{Normalize(row->text)};
            Hit hit{*row, -1};
            if (!text.empty()) {
                if (!(row->flags & NAMED)) continue;
                const auto tier{MatchTier(norm, text)};
                if (!tier) continue;
                hit.match = *tier;
            }
            if (!PassKeywords(norm, f)) continue;
            std::optional<Bytes> key;
            if (sort == Sort::RELEVANCE) {
                // As the index orders them: the names that start with the text, by name; then those
                // with a word that does, by the first such word.
                std::string by{norm};
                if (hit.match == 2) {
                    const std::string first_word{FirstWord(text)};
                    by.clear();
                    for (const std::string& w : LaterWords(norm)) {
                        if (w.starts_with(first_word) && (by.empty() || w < by)) by = w;
                    }
                }
                key = Bytes{static_cast<unsigned char>(hit.match == 2 ? 1 : 0)};
                key->insert(key->end(), by.begin(), by.end());
                key->push_back(0);
                Put32(*key, seq);
            } else {
                key = OrderKey(sort, *row, norm);
            }
            if (key) found.emplace_back(std::move(*key), std::move(hit));
        }
        Bytes next_key;
        bool more{false};
        PageOf(found, cursor, limit, page.hits, next_key, more);
        if (more) page.next = EncodeCursor({'A', MODE_CANDIDATES, static_cast<uint8_t>(request.sort), 0, next_key});
        return page;
    }

    // Along an index, in its order, with the filters: by relevance, the names that start with the text,
    // then those with a word that does; otherwise the order's own index.
    struct Phase {
        Bytes prefix;
        //! The text is matched by a word of the name (its row read).
        bool words{false};
    };
    std::vector<Phase> phases;
    if (sort == Sort::RELEVANCE) {
        Bytes prefix{K('n')};
        prefix.push_back(0);
        prefix.insert(prefix.end(), text.begin(), text.end());
        phases.push_back({prefix, false});
        if (const std::string word{FirstWord(text)}; !word.empty()) {
            prefix = K('t');
            prefix.insert(prefix.end(), word.begin(), word.end());
            phases.push_back({prefix, true});
        }
    } else {
        phases.push_back({OrderPrefix(sort), false});
    }
    size_t phase{cursor ? static_cast<size_t>(cursor->phase & ~AT_KEY) : 0};
    Bytes at{cursor ? (cursor->phase & AT_KEY ? cursor->key : After(cursor->key)) : Bytes{}};
    const std::string word{FirstWord(text)};
    for (; phase < phases.size(); ++phase, at.clear()) {
        const Phase& ph{phases[phase]};
        if (at.empty()) at = ph.prefix;
        while (const auto entry{view.Next(at, ph.prefix)}) {
            const Bytes& key{entry->first};
            at = After(key);
            if (page.scanned >= SCAN_BUDGET || rows_read >= ROW_BUDGET) {
                // Out of budget: the next call goes on at this entry, not looked at yet.
                page.partial = true;
                page.next = EncodeCursor({'A', MODE_SCAN, static_cast<uint8_t>(request.sort), static_cast<uint8_t>(phase | AT_KEY), key});
                return page;
            }
            ++page.scanned;
            const uint32_t seq{SeqOfKey(key)};
            std::optional<AssetRow> row;
            Summary summary;
            if (key[1] == 'r') {
                row = sidechain::DecodeValue<AssetRow>(entry->second);
                summary = row->Summarize();
            } else {
                summary = sidechain::DecodeValue<Summary>(entry->second);
            }
            // The orders that end where the filters do.
            if (sort == Sort::ACTIVITY && f.min_last && summary.last < *f.min_last) return page;
            if (sort == Sort::NEWEST && f.created_after && summary.created <= *f.created_after) return page;
            if (!PassSummary(summary, f, seq)) continue;
            // The name: from the key of the order of names, else from the row.
            std::optional<std::string> norm;
            if (key[1] == 'n' && key[2] == 0) norm = std::string(key.begin() + 3, key.end() - 5);
            const bool need_name{!text.empty() || !f.exclude_keywords.empty()};
            if (need_name && !norm) {
                if (!row) {
                    ++rows_read;
                    row = GetAssetRow(view, seq);
                }
                if (!row) continue;
                norm = Normalize(row->text);
            }
            int match{-1};
            if (!text.empty()) {
                if (!(summary.flags & NAMED)) continue;
                const auto tier{MatchTier(*norm, text)};
                if (!tier) continue;
                if (ph.words) {
                    // Found by a word: not one the names' phase found, and once (at the first of its words that does).
                    if (*tier < 2) continue;
                    const std::string this_word(key.begin() + 2, key.end() - 5);
                    std::string first;
                    for (const std::string& w : LaterWords(*norm)) {
                        if (w.starts_with(word) && (first.empty() || w < first)) first = w;
                    }
                    if (this_word != first) continue;
                }
                match = *tier;
            }
            if (norm && !PassKeywords(*norm, f)) continue;
            if (!row) {
                ++rows_read;
                row = GetAssetRow(view, seq);
                if (!row) continue;
            }
            page.hits.push_back({std::move(*row), match});
            if (page.hits.size() >= limit) {
                page.next = EncodeCursor({'A', MODE_SCAN, static_cast<uint8_t>(request.sort), static_cast<uint8_t>(phase), key});
                return page;
            }
        }
    }
    return page;
}

std::vector<AssetId> MatchingAssets(const sidechain::StoreView& view, Request request, size_t max_assets, bool& partial)
{
    std::vector<AssetId> assets;
    size_t scanned{0};
    request.limit = MAX_PAGE;
    while (true) {
        const Page page{Search(view, request)};
        scanned += page.scanned;
        for (const Hit& hit : page.hits) {
            if (assets.size() >= max_assets) break;
            assets.push_back(hit.row.id);
        }
        if (page.next.empty()) return assets;
        if (assets.size() >= max_assets || scanned >= SCAN_BUDGET || page.partial) {
            partial = true;
            return assets;
        }
        request.cursor = page.next;
    }
}

PoolPage SearchPools(const sidechain::StoreView& view, const PoolRequest& request)
{
    PoolPage page;
    const size_t limit{std::clamp<size_t>(request.limit, 1, MAX_PAGE)};
    const std::optional<Cursor> cursor{DecodeCursor(request.cursor, 'P', static_cast<uint8_t>(request.sort))};
    const auto pass{[&](const PoolRow& row) {
        if (request.exclude_abandoned && row.abandoned) return false;
        if (request.chn_only && !row.asset0.IsNull()) return false;
        if (request.min_depth > 0 && row.Depth() < request.min_depth) return false;
        // The orders of depth and volume hold only the pools with some.
        if (request.sort == PoolSort::DEPTH && row.Depth() == 0) return false;
        if (request.sort == PoolSort::VOLUME && row.volume == 0) return false;
        return true;
    }};
    if (request.assets) {
        // The pools of the assets a text matched, sorted in memory.
        const State state{view};
        std::set<uint256> ids;
        for (const AssetId& asset : *request.assets) {
            if (asset.IsNull()) continue;
            for (const auto& [id, pool] : state.PoolsOf(asset)) {
                ids.insert(id);
                if (ids.size() >= CANDIDATE_CAP) break;
            }
            if (ids.size() >= CANDIDATE_CAP) {
                page.partial = true;
                break;
            }
        }
        std::vector<std::pair<Bytes, PoolRow>> found;
        for (const uint256& id : ids) {
            ++page.scanned;
            const auto row{GetPoolRow(view, id)};
            if (row && pass(*row)) found.emplace_back(PoolOrderKey(request.sort, *row), *row);
        }
        Bytes next_key;
        bool more{false};
        PageOf(found, cursor, limit, page.pools, next_key, more);
        if (more) page.next = EncodeCursor({'P', MODE_CANDIDATES, static_cast<uint8_t>(request.sort), 0, next_key});
        return page;
    }
    const Bytes any_key{PoolOrderKey(request.sort, PoolRow{})};
    const Bytes prefix{any_key.begin(), any_key.begin() + 2};
    Bytes at{cursor ? (cursor->phase & AT_KEY ? cursor->key : After(cursor->key)) : prefix};
    while (const auto entry{view.Next(at, prefix)}) {
        const Bytes& key{entry->first};
        if (page.scanned >= SCAN_BUDGET) {
            page.partial = true;
            page.next = EncodeCursor({'P', MODE_SCAN, static_cast<uint8_t>(request.sort), AT_KEY, key});
            return page;
        }
        at = After(key);
        ++page.scanned;
        const auto row{GetPoolRow(view, GetHash(std::span{key}.subspan(key.size() - 32)))};
        if (!row || !pass(*row)) continue;
        page.pools.push_back(*row);
        if (page.pools.size() >= limit) {
            page.next = EncodeCursor({'P', MODE_SCAN, static_cast<uint8_t>(request.sort), 0, key});
            return page;
        }
    }
    return page;
}

namespace {
bool AuctionStatusPasses(const Auction& a, int height, const std::string& status, bool include_closed)
{
    if (a.closed) return include_closed;
    const bool upcoming{height < a.start_height};
    const bool ended{a.remaining == 0 || height > a.EndHeight()};
    const bool open{!upcoming && !ended};
    if (status == "open") return open;
    if (status == "upcoming") return upcoming && !ended;
    if (status == "active") return !ended;
    if (status == "ended") return ended;
    return true;
}
} // namespace

AuctionPage SearchAuctions(const sidechain::StoreView& view, const AuctionRequest& request)
{
    AuctionPage page;
    const size_t limit{std::clamp<size_t>(request.limit, 1, MAX_PAGE)};
    const std::optional<Cursor> cursor{DecodeCursor(request.cursor, 'U', static_cast<uint8_t>(request.sort))};
    const State state{view};
    const bool include_closed{request.include_closed && request.sort == AuctionSort::NEWEST};
    const auto key_of{[&](const Txid& id, const Auction& a) { return request.sort == AuctionSort::NEWEST ? AuctionNewKey(id, a) : AuctionEndKey(id, a); }};
    if (request.assets) {
        std::set<Txid> ids;
        for (const AssetId& asset : *request.assets) {
            for (const Txid& id : state.AuctionsOf(asset, CANDIDATE_CAP)) {
                ids.insert(id);
                if (ids.size() >= CANDIDATE_CAP) break;
            }
            if (ids.size() >= CANDIDATE_CAP) {
                page.partial = true;
                break;
            }
        }
        std::vector<std::pair<Bytes, std::pair<Txid, Auction>>> found;
        for (const Txid& id : ids) {
            ++page.scanned;
            const auto auction{state.GetAuction(id)};
            if (auction && AuctionStatusPasses(*auction, request.height, request.status, include_closed)) found.emplace_back(key_of(id, *auction), std::make_pair(id, *auction));
        }
        Bytes next_key;
        bool more{false};
        PageOf(found, cursor, limit, page.auctions, next_key, more);
        if (more) page.next = EncodeCursor({'U', MODE_CANDIDATES, static_cast<uint8_t>(request.sort), 0, next_key});
        return page;
    }
    const Bytes prefix{K(request.sort == AuctionSort::NEWEST ? 'B' : 'E')};
    Bytes at{prefix};
    if (cursor) {
        at = cursor->phase & AT_KEY ? cursor->key : After(cursor->key);
    } else if (request.sort == AuctionSort::ENDING_SOON && request.status != "ended" && request.status != "any") {
        // Those that end before the next block are over.
        Put32(at, Height32(request.height));
    }
    while (const auto entry{view.Next(at, prefix)}) {
        const Bytes& key{entry->first};
        if (page.scanned >= SCAN_BUDGET) {
            page.partial = true;
            page.next = EncodeCursor({'U', MODE_SCAN, static_cast<uint8_t>(request.sort), AT_KEY, key});
            return page;
        }
        at = After(key);
        ++page.scanned;
        const Txid id{Txid::FromUint256(GetHash(std::span{key}.subspan(key.size() - 32)))};
        const auto auction{state.GetAuction(id)};
        if (!auction) continue;
        if (!AuctionStatusPasses(*auction, request.height, request.status, include_closed)) continue;
        page.auctions.emplace_back(id, *auction);
        if (page.auctions.size() >= limit) {
            page.next = EncodeCursor({'U', MODE_SCAN, static_cast<uint8_t>(request.sort), 0, key});
            return page;
        }
    }
    return page;
}

} // namespace bitassets::index

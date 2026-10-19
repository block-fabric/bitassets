// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/index.h>
#include <bitassets/state.h>
#include <dbwrapper.h>
#include <random.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <tinyformat.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace bitassets;
using namespace bitassets::index;
using sidechain::StoreBytes;

namespace {

uint256 Id(uint32_t n)
{
    uint256 id;
    for (int i{0}; i < 4; ++i) id.begin()[i] = static_cast<unsigned char>(n >> (8 * i));
    id.begin()[31] = 0x5a;
    return id;
}

AssetRow MakeRow(uint32_t seq, const std::string& name, uint64_t supply, int created, int last, uint8_t flags = 0, uint64_t depth = 0, uint64_t volume = 0)
{
    AssetRow row;
    row.seq = seq;
    row.id = Id(seq);
    row.text = name;
    row.decimals = 2;
    row.supply = supply;
    row.minted = supply;
    row.created = created;
    row.last = last;
    row.flags = flags | (name.empty() ? 0 : NAMED);
    row.depth = depth;
    row.volume = volume;
    return row;
}

/** Every page of a search, by its cursor. */
std::vector<uint32_t> All(const sidechain::StoreView& view, Request request, size_t* pages = nullptr)
{
    std::vector<uint32_t> seqs;
    size_t n{0};
    while (true) {
        const Page page{Search(view, request)};
        ++n;
        for (const Hit& hit : page.hits) seqs.push_back(hit.row.seq);
        if (page.next.empty()) break;
        request.cursor = page.next;
        BOOST_REQUIRE(n < 100000);
    }
    if (pages) *pages = n;
    return seqs;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bitassets_index_tests)

BOOST_AUTO_TEST_CASE(text)
{
    BOOST_CHECK_EQUAL(Normalize("  Gold Coin "), "gold coin");
    BOOST_CHECK(Words("gold-coin gold #2") == (std::vector<std::string>{"gold", "coin", "2"}));
    BOOST_CHECK_EQUAL(*MatchTier("gold", "gold"), 0);
    BOOST_CHECK_EQUAL(*MatchTier("golden", "gold"), 1);
    BOOST_CHECK_EQUAL(*MatchTier("my gold coin", "gold c"), 2);
    BOOST_CHECK_EQUAL(*MatchTier("$gold", "gold"), 2);
    BOOST_CHECK(!MatchTier("mygold", "gold"));
    BOOST_CHECK(!MatchTier("gold", ""));
}

BOOST_AUTO_TEST_CASE(search_orders_filters_and_cursors)
{
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    std::vector<AssetRow> rows{
        MakeRow(0, "GOLD", 1000, 10, 50, HAS_POOL, 500, 70),
        MakeRow(1, "Golden Goose", 0, 11, 11),
        MakeRow(2, "my gold coin", 300, 12, 40, HAS_LOGO),
        MakeRow(3, "Silver", 5000, 13, 60, HAS_POOL | HAS_LOGO, 900, 10),
        MakeRow(4, "", 7, 14, 14),
        MakeRow(5, "GOLD scam airdrop", 100, 15, 55, RETIRED),
        MakeRow(6, "goldy gold", 20, 16, 16),
    };
    for (const AssetRow& row : rows) PutAssetRow(store, std::nullopt, row);

    const auto seqs_of{[&](Request r) { return All(store, r); }};
    Request r;
    r.text = "gold";
    // The whole name, its start, then a word's; the retired one left out.
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 1, 6, 2}));
    r.filters.exclude_retired = false;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 5, 1, 6, 2}));
    r.filters.exclude_keywords = {"scam"};
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 1, 6, 2}));
    // A page of one at a time comes to the same.
    r.limit = 1;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 1, 6, 2}));
    r = Request{};
    r.text = "gold";
    r.sort = Sort::SUPPLY;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 2, 6, 1}));
    r.filters.exclude_burned = true;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 2, 6}));

    r = Request{};
    r.sort = Sort::NAME;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 1, 6, 2, 3, 4}));
    r.sort = Sort::NEWEST;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{6, 4, 3, 2, 1, 0}));
    r.sort = Sort::OLDEST;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 1, 2, 3, 4, 6}));
    r.sort = Sort::ACTIVITY;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{3, 0, 2, 6, 4, 1}));
    r.filters.min_last = 40;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{3, 0, 2}));
    r.filters = {};
    r.sort = Sort::VOLUME;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{0, 3}));
    r.sort = Sort::POOL_DEPTH;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{3, 0}));
    r.sort = Sort::SUPPLY;
    r.filters.has_logo = true;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{3, 2}));
    r.filters = {};
    r.filters.has_pool = true;
    r.filters.min_supply = 10; // whole units: 1000 units at 2 decimals
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{3, 0}));
    r.filters = {};
    r.filters.created_after = 13;
    r.sort = Sort::NEWEST;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{6, 4}));
    // The wallet's.
    r.filters = {};
    r.filters.only = std::set<uint32_t>{1, 3};
    r.sort = Sort::NAME;
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{1, 3}));
    // A number.
    r = Request{};
    r.exact = std::vector<uint32_t>{2};
    BOOST_CHECK(seqs_of(r) == (std::vector<uint32_t>{2}));

    // A cursor of another search is refused.
    r = Request{};
    r.sort = Sort::NAME;
    r.limit = 1;
    Request other;
    other.sort = Sort::SUPPLY;
    other.cursor = Search(store, r).next;
    BOOST_CHECK_THROW(Search(store, other), std::invalid_argument);

    // A row that changes moves in every order.
    AssetRow moved{rows[1]};
    moved.supply = 9000;
    moved.last = 70;
    PutAssetRow(store, rows[1], moved);
    r = Request{};
    r.sort = Sort::SUPPLY;
    BOOST_CHECK_EQUAL(All(store, r).front(), 1U);
    r.sort = Sort::ACTIVITY;
    BOOST_CHECK_EQUAL(All(store, r).front(), 1U);
    PutAssetRow(store, moved, std::nullopt);
    BOOST_CHECK_EQUAL(All(store, r).size(), 5U);
    // Nothing of it is left.
    size_t entries{0};
    store.ForEach(StoreBytes{sidechain::DERIVED_TABLE}, [&](const StoreBytes& key, const StoreBytes&) {
        BOOST_CHECK(key.size() < 6 || !(key[key.size() - 4] == 0 && key[key.size() - 3] == 0 && key[key.size() - 2] == 0 && key[key.size() - 1] == 1 && key[1] != 'o'));
        ++entries;
        return true;
    });
    BOOST_CHECK(entries > 0);
}

BOOST_AUTO_TEST_CASE(matching_assets_bounded)
{
    // The assets a text names, for a search of pools or auctions: bounded however few names match among many.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    constexpr uint32_t N{30'000};
    for (uint32_t seq{0}; seq < N; ++seq) PutAssetRow(store, std::nullopt, MakeRow(seq, strprintf("foo bar %u", seq), 1, 1, 1));
    Request r;
    // Every later word "bar" read, none matching: one call's budget of rows, then it stops.
    r.text = "bar 99999";
    bool partial{false};
    BOOST_CHECK(MatchingAssets(store, r, 1000, partial).empty());
    BOOST_CHECK(partial);
    const Page page{Search(store, r)};
    BOOST_CHECK(page.partial);
    BOOST_CHECK(!page.next.empty());
    BOOST_CHECK(page.scanned <= ROW_BUDGET + 1);
    // Many matches: the first 1000.
    r.text = "foo";
    partial = false;
    BOOST_CHECK_EQUAL(MatchingAssets(store, r, 1000, partial).size(), 1000U);
    BOOST_CHECK(partial);
    // Few: all of them, complete.
    r.text = "foo bar 2999";
    partial = false;
    BOOST_CHECK_EQUAL(MatchingAssets(store, r, 1000, partial).size(), 11U); // 2999 and 29990 to 29999
    BOOST_CHECK(!partial);
}

BOOST_AUTO_TEST_CASE(blocks_and_undo)
{
    // The index from what blocks change in the state, and back by their undo data.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay base{empty};
    const AssetId gold{HashName("GOLD")};
    const uint256 pool_id{PoolId(CHN, gold)};
    AssetRecord record;
    record.seq = 0;
    record.height = 1;
    record.text = "GOLD";
    record.supply = record.minted = 1000;
    const int window{3};
    std::vector<sidechain::StoreUndo> undos;
    const auto connect{[&](int height, const std::function<void(sidechain::StoreOverlay&)>& change) {
        sidechain::StoreOverlay block{base, /*journal=*/true};
        change(block);
        ConnectBlock(block, height, window);
        undos.push_back(block.TakeUndo());
        block.MergeInto(base);
    }};
    connect(1, [&](sidechain::StoreOverlay& s) { s.Put(sidechain::TableKey(0x31, gold), sidechain::EncodeValue(record)); });
    auto row{GetAssetRow(base, 0)};
    BOOST_REQUIRE(row);
    BOOST_CHECK_EQUAL(row->text, "GOLD");
    BOOST_CHECK_EQUAL(row->created, 1);
    BOOST_CHECK_EQUAL(row->last, 1);
    BOOST_CHECK_EQUAL(GetInfo(base).assets, 1U);
    BOOST_CHECK(LayoutCurrent(base));
    {
        // Assets indexed by no index (a store of a version without it): derived again.
        sidechain::StoreOverlay old_version{empty};
        old_version.Put(sidechain::TableKey(0x31, gold), sidechain::EncodeValue(record));
        BOOST_CHECK(!LayoutCurrent(old_version));
        BOOST_CHECK(LayoutCurrent(empty));
    }
    // A pool with CHN, then a trade in it.
    Pool pool;
    pool.asset0 = CHN;
    pool.asset1 = gold;
    pool.reserve0 = 10'000'000;
    pool.reserve1 = 100;
    pool.shares = 1'000'000;
    pool.height = 2;
    connect(2, [&](sidechain::StoreOverlay& s) {
        record.provided_pools = 1;
        s.Put(sidechain::TableKey(0x31, gold), sidechain::EncodeValue(record));
        s.Put(sidechain::TableKey(0x34, pool_id), sidechain::EncodeValue(pool));
        // The state's index of pools by asset (0x3a), which a search of the pools of an asset reads.
        for (const uint256& a : {CHN, gold}) {
            StoreBytes key{sidechain::TableKey(0x3a, a)};
            key.insert(key.end(), pool_id.begin(), pool_id.end());
            s.Put(key, {});
        }
    });
    row = GetAssetRow(base, 0);
    BOOST_CHECK_EQUAL(row->depth, 10'000'000U);
    BOOST_CHECK(row->flags & HAS_POOL);
    const auto before_trade{GetAssetRow(base, 0)};
    connect(3, [&](sidechain::StoreOverlay& s) {
        pool.volume0 += 2'000'000; // 0.02 CHN in
        pool.reserve0 += 2'000'000;
        pool.reserve1 -= 17;
        s.Put(sidechain::TableKey(0x34, pool_id), sidechain::EncodeValue(pool));
    });
    row = GetAssetRow(base, 0);
    BOOST_CHECK_EQUAL(row->volume, 2'000'000U);
    BOOST_CHECK_EQUAL(row->last, 3);
    BOOST_CHECK_EQUAL(GetPoolRow(base, pool_id)->volume, 2'000'000U);
    BOOST_CHECK(GetPoolRow(base, pool_id)->ChangePpm() > 0);
    {
        PoolRequest pr;
        BOOST_CHECK_EQUAL(SearchPools(base, pr).pools.size(), 1U);
        pr.sort = PoolSort::VOLUME;
        BOOST_CHECK_EQUAL(SearchPools(base, pr).pools.size(), 1U);
        pr.sort = PoolSort::CHANGE;
        BOOST_CHECK_EQUAL(SearchPools(base, pr).pools.size(), 1U);
        pr.assets = std::vector<AssetId>{gold};
        BOOST_CHECK_EQUAL(SearchPools(base, pr).pools.size(), 1U);
    }
    // Blocks that touch nothing else: the trade leaves the window at 3 + 3.
    connect(4, [](sidechain::StoreOverlay&) {});
    connect(5, [](sidechain::StoreOverlay&) {});
    BOOST_CHECK_EQUAL(GetAssetRow(base, 0)->volume, 2'000'000U);
    connect(6, [](sidechain::StoreOverlay&) {});
    BOOST_CHECK_EQUAL(GetAssetRow(base, 0)->volume, 0U);
    BOOST_CHECK_EQUAL(GetPoolRow(base, pool_id)->volume, 0U);
    // The price change is now against the price after the trade.
    BOOST_CHECK_EQUAL(GetPoolRow(base, pool_id)->ChangePpm(), 0);
    {
        PoolRequest pr;
        pr.sort = PoolSort::VOLUME;
        BOOST_CHECK(SearchPools(base, pr).pools.empty());
    }
    // Back to before the trade, block by block.
    while (undos.size() > 2) {
        base.Revert(undos.back());
        undos.pop_back();
    }
    BOOST_CHECK(GetAssetRow(base, 0) == before_trade);
    // Retired: the row stays, flagged.
    connect(3, [&](sidechain::StoreOverlay& s) {
        s.Erase(sidechain::TableKey(0x31, gold));
        s.Erase(sidechain::TableKey(0x34, pool_id));
    });
    row = GetAssetRow(base, 0);
    BOOST_CHECK(row->flags & RETIRED);
    BOOST_CHECK_EQUAL(row->depth, 0U);
    BOOST_CHECK(!GetPoolRow(base, pool_id));
    BOOST_CHECK_EQUAL(GetInfo(base).retired, 1U);
    BOOST_CHECK_EQUAL(GetInfo(base).pools, 0U);
    // Everything undone: nothing of the index is left.
    while (!undos.empty()) {
        base.Revert(undos.back());
        undos.pop_back();
    }
    BOOST_CHECK(!base.Next(StoreBytes{sidechain::DERIVED_TABLE}, StoreBytes{sidechain::DERIVED_TABLE}));
    // Left out of the hash of the state.
    sidechain::StoreOverlay with_index{empty};
    with_index.Put(StoreBytes{sidechain::DERIVED_TABLE, 'x'}, {1});
    BOOST_CHECK(sidechain::StoreHash(with_index) == sidechain::StoreHash(empty));
}

/**
 * A million assets in a LevelDB store, as a node keeps them: how long pages take, what it costs on disk.
 * Run only when asked (a minute or so, a few hundred MB of disk):
 *   BITASSETS_INDEX_BENCH=1 test_bitcoin --run_test=bitassets_index_tests/million
 */
BOOST_FIXTURE_TEST_CASE(million, BasicTestingSetup)
{
    if (!std::getenv("BITASSETS_INDEX_BENCH")) return;
    const fs::path path{m_args.GetDataDirBase() / "index_bench"};
    const auto rss{[] {
        std::ifstream status{"/proc/self/status"};
        std::string line;
        while (std::getline(status, line)) {
            if (line.starts_with("VmRSS:")) return std::stoul(line.substr(6)) / 1024;
        }
        return 0UL;
    }};
    const size_t rss_start{rss()};
    CDBWrapper db{DBParams{.path = path, .cache_bytes = 8 << 20, .wipe_data = true}};
    sidechain::DbStore store{db, StoreBytes{'s'}};
    FastRandomContext rng{/*fDeterministic=*/true};
    const std::vector<std::string> words{"gold", "silver", "coin", "token", "moon", "doge", "cat", "pepe", "inu", "dao", "fi", "swap",
                                         "chain", "layer", "meta", "verse", "ai", "bit", "sat", "stack", "punk", "ape", "frog", "rocket"};
    constexpr uint32_t N{1'000'000};
    const auto t0{SteadyClock::now()};
    {
        sidechain::StoreOverlay overlay{store};
        for (uint32_t seq{0}; seq < N; ++seq) {
            std::string name{words[rng.randrange(words.size())]};
            if (rng.randbool()) name += " " + words[rng.randrange(words.size())];
            name += strprintf(" %u", seq);
            const uint64_t r{rng.randrange(uint64_t{100})};
            uint8_t flags{0};
            if (r < 5) flags |= HAS_POOL;
            if (r % 50 == 7) flags |= HAS_LOGO;
            if (r == 99) flags |= RETIRED;
            AssetRow row{MakeRow(seq, name, rng.randrange(10) == 0 ? 0 : rng.randrange(1'000'000'000), static_cast<int>(seq / 100), static_cast<int>(seq / 100 + rng.randrange(5000)),
                                 flags, (flags & HAS_POOL) ? rng.randrange(1'000'000'000) + 1 : 0, (flags & HAS_POOL) && rng.randbool() ? rng.randrange(1'000'000'000) + 1 : 0)};
            PutAssetRow(overlay, std::nullopt, row);
            if (seq % 50'000 == 49'999) {
                CDBBatch batch{db};
                store.Write(batch, overlay.Changes());
                db.WriteBatch(batch);
                store.Reset();
                overlay.Clear();
            }
        }
    }
    db.CompactFull();
    const auto t1{SteadyClock::now()};
    uint64_t disk{0};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path.std_path())) {
        if (entry.is_regular_file()) disk += entry.file_size();
    }
    std::cout << strprintf("index of %u assets: built in %.1f s, %.1f MB on disk (%.0f bytes per asset), RSS %u MB -> %u MB\n",
                           N, Ticks<std::chrono::duration<double>>(t1 - t0), disk / 1e6, double(disk) / N, rss_start, rss());

    // As the commands read it: a snapshot of the database.
    const auto view{store.Snapshot()};
    const auto time_pages{[&](const std::string& what, Request request, int pages) {
        double worst{0}, total{0};
        size_t hits{0}, scanned{0};
        int done{0};
        for (; done < pages; ++done) {
            const auto start{SteadyClock::now()};
            const Page page{Search(*view, request)};
            const double ms{Ticks<std::chrono::duration<double, std::milli>>(SteadyClock::now() - start)};
            worst = std::max(worst, ms);
            total += ms;
            hits += page.hits.size();
            scanned += page.scanned;
            if (page.next.empty()) { ++done; break; }
            request.cursor = page.next;
        }
        std::cout << strprintf("  %-46s %2d pages: %6.2f ms/page avg, %6.2f ms worst, %5zu hits, %7zu entries scanned\n", what, done, total / done, worst, hits, scanned);
        BOOST_CHECK_LT(worst, 250.0);
    }};
    Request r;
    r.limit = 50;
    r.sort = Sort::ACTIVITY;
    time_pages("activity", r, 10);
    r.sort = Sort::NEWEST;
    time_pages("newest", r, 10);
    r.sort = Sort::NAME;
    time_pages("name", r, 10);
    r.sort = Sort::SUPPLY;
    time_pages("supply", r, 10);
    r.sort = Sort::VOLUME;
    time_pages("volume", r, 10);
    r.sort = Sort::POOL_DEPTH;
    time_pages("pool_depth", r, 10);
    r.sort = Sort::RELEVANCE;
    r.text = "gold";
    time_pages("text 'gold' relevance", r, 10);
    r.text = "coin";
    time_pages("text 'coin' relevance (words)", r, 10);
    r.text = "moon 12345";
    time_pages("text 'moon 12345' (rare)", r, 3);
    r.text = "gold";
    r.sort = Sort::SUPPLY;
    time_pages("text 'gold' by supply (> cap: along supply)", r, 10);
    r.text = "gold silver 1";
    time_pages("text 'gold silver 1' by supply (in memory)", r, 10);
    r = Request{};
    r.sort = Sort::SUPPLY;
    r.filters.has_logo = true;
    r.filters.exclude_burned = true;
    time_pages("supply, has_logo, exclude_burned", r, 10);
    r.filters = {};
    r.sort = Sort::ACTIVITY;
    r.filters.min_last = 9000;
    time_pages("activity, active within (stops early)", r, 10);
    r.filters = {};
    r.sort = Sort::NEWEST;
    r.filters.exclude_keywords = {"doge", "pepe", "inu"};
    time_pages("newest, exclude_keywords doge pepe inu", r, 10);
    r.filters = {};
    r.sort = Sort::NAME;
    r.filters.has_pool = true;
    r.filters.min_supply = 9'000'000;
    time_pages("name, has_pool, min_supply (selective)", r, 10);
    // The worst a call can do: a filter nothing passes, along an order that is not its own (the budget
    // of entries), and a text along another order with every row read (the budget of rows).
    r.filters = {};
    r.sort = Sort::SUPPLY;
    r.filters.min_supply = 1e18;
    time_pages("supply, min_supply nothing has (budget)", r, 3);
    r.filters = {};
    r.sort = Sort::ACTIVITY;
    r.text = "pepe";
    time_pages("text 'pepe' by activity (a row per entry)", r, 3);
    std::cout << strprintf("RSS after the searches: %u MB\n", rss());
}

BOOST_AUTO_TEST_SUITE_END()

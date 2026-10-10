// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/state.h>
#include <bitassets/rpcutil.h>
#include <addresstype.h>
#include <consensus/params.h>
#include <sidechain/state.h>
#include <arith_uint256.h>
#include <key.h>
#include <script/script.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace bitassets;

namespace {

/** The undo data of the blocks of a test, a journal each, reverted last first. */
struct TUndo {
    std::vector<sidechain::StoreUndo> parts;
    template <typename Stream>
    void Serialize(Stream& s) const { s << parts; }
    template <typename Stream>
    void Unserialize(Stream& s) { s >> parts; }
};

/** The assets on a store of their own, with the shape the tests were written for (tables as maps: for tests). */
class TState
{
public:
    TState() : m_store{std::make_unique<sidechain::StoreOverlay>(m_empty, /*journal=*/true)} {}
    TState(const TState& other) : TState()
    {
        other.m_store->ForEach({}, [&](const sidechain::StoreBytes& k, const sidechain::StoreBytes& v) {
            m_store->Put(k, v);
            return true;
        });
        m_store->TakeUndo();
    }
    TState& operator=(const TState& other)
    {
        // A store of our own, over our own (empty) base, with the other's entries.
        auto store{std::make_unique<sidechain::StoreOverlay>(m_empty, /*journal=*/true)};
        other.m_store->ForEach({}, [&](const sidechain::StoreBytes& k, const sidechain::StoreBytes& v) {
            store->Put(k, v);
            return true;
        });
        store->TakeUndo();
        m_store = std::move(store);
        return *this;
    }
    bool ApplyTx(const CTransaction& tx, int height, TUndo& undo, std::vector<CTxOut>& payouts, std::string& reason, CAmount* released = nullptr, int reveal_depth = 0)
    {
        const bool ok{bitassets::State{*m_store}.ApplyTx(tx, height, payouts, reason, released, reveal_depth)};
        undo.parts.push_back(m_store->TakeUndo());
        return ok;
    }
    bool CheckTx(const CTransaction& tx, int height, std::string& reason, std::vector<Result>* results = nullptr) const
    {
        return View().CheckTx(tx, height, reason, results);
    }
    void Revert(const TUndo& undo)
    {
        for (auto it{undo.parts.rbegin()}; it != undo.parts.rend(); ++it) m_store->Revert(*it);
        m_store->TakeUndo();
    }
    bitassets::State View() const { return bitassets::State{*m_store}; }
    const sidechain::StoreView& Store() const { return *m_store; }
    /** Writes an entry as it is, outside the rules: for states no transaction can make (forged). */
    void Forge(const sidechain::StoreBytes& key, const sidechain::StoreBytes& value)
    {
        m_store->Put(key, value);
        m_store->TakeUndo();
    }
    /** How many entries there are under a prefix. */
    size_t Count(const sidechain::StoreBytes& prefix) const
    {
        size_t n{0};
        m_store->ForEach(prefix, [&](const sidechain::StoreBytes&, const sidechain::StoreBytes&) { ++n; return true; });
        return n;
    }
    bool Releasable(const AssetId& asset, std::string* why = nullptr) const { return View().Releasable(asset, why); }
    std::optional<AssetId> AssetOfSeq(uint32_t seq) const { return View().AssetOfSeq(seq); }
    uint32_t NextSeq() const { return View().NextSeq(); }
    std::optional<Pool> FindPool(const AssetId& a, const AssetId& b) const { return View().FindPool(a, b); }
    std::map<COutPoint, Token> Tokens() const { std::map<COutPoint, Token> m; View().ForEachToken([&](const COutPoint& k, const Token& v) { m.emplace(k, v); return true; }); return m; }
    std::map<AssetId, AssetRecord> Assets() const { std::map<AssetId, AssetRecord> m; View().ForEachAsset([&](const AssetId& k, const AssetRecord& v) { m.emplace(k, v); return true; }); return m; }
    std::map<Txid, uint256> Reservations() const { std::map<Txid, uint256> m; View().ForEachReservation([&](const Txid& k, const uint256& v) { m.emplace(k, v); return true; }); return m; }
    std::map<uint256, Pool> Pools() const { std::map<uint256, Pool> m; View().ForEachPool([&](const uint256& k, const Pool& v) { m.emplace(k, v); return true; }); return m; }
    std::map<Txid, Auction> Auctions() const { std::map<Txid, Auction> m; View().ForEachAuction([&](const Txid& k, const Auction& v) { m.emplace(k, v); return true; }); return m; }
    friend bool operator==(const TState& a, const TState& b) { return sidechain::StoreHash(*a.m_store) == sidechain::StoreHash(*b.m_store); }

private:
    sidechain::EmptyStore m_empty;
    std::unique_ptr<sidechain::StoreOverlay> m_store;
};
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const AssetId GOLD{HashName("GOLD")};
const AssetId SILVER{HashName("SILVER")};

int g_nonce{0};

/** A transaction: spends `inputs` (and a coin, for its fee), outputs one token output per entry of `outs`, then the marker. */
CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0)
{
    CMutableTransaction tx;
    // A coin of its own, so that every transaction is different.
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce)}), static_cast<uint32_t>(g_nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (const auto& token : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), token});
        tx.vout.emplace_back(0, HOLDER);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

/** A reservation of `commitment`, carried by an output of `script`. */
CTransaction MakeReserve(const uint256& commitment, const CScript& script = HOLDER)
{
    CMutableTransaction tx{MakeTx({}, Reserve{commitment}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
    tx.vout[0].scriptPubKey = script;
    return CTransaction{tx};
}

Token Asset(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }
Token Unit(Token::Kind kind, const uint256& id) { return Token{kind, id, 1}; }

struct Fixture {
    TState state;
    TUndo undo;
    std::vector<CTxOut> payouts;
    int height{100};
    //! How deep a reservation is before it is revealed: 0, so that Issue registers in one block.
    int reveal_depth{0};

    bool Apply(const CTransaction& tx, std::string* reason = nullptr)
    {
        std::string r;
        const bool ok{state.ApplyTx(tx, height, undo, payouts, r, nullptr, reveal_depth)};
        if (reason) *reason = r;
        return ok;
    }
    std::string Reject(const CTransaction& tx)
    {
        std::string r;
        const TState before{state};
        BOOST_CHECK(!state.ApplyTx(tx, height, undo, payouts, r, nullptr, reveal_depth));
        BOOST_CHECK(state == before);
        return r;
    }
    /** Registers `name` with `supply`: the outputs of the control coin and of the supply. */
    std::pair<COutPoint, COutPoint> Issue(const std::string& name, uint64_t supply, uint8_t decimals = 0)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserve{MakeReserve(ReservationCommitment(id, nonce, HOLDER))};
        BOOST_REQUIRE(Apply(reserve));
        bitassets::Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.decimals = decimals;
        reg.text = name;
        std::vector<std::optional<Token>> outs{Unit(Token::Kind::CONTROL, id)};
        if (supply > 0) outs.push_back(Asset(id, supply));
        const CTransaction tx{MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, outs)};
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Apply(tx, &reason), reason);
        return {COutPoint{tx.GetHash(), 0}, COutPoint{tx.GetHash(), 1}};
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(bitassets_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(marker_round_trip)
{
    Marker marker;
    marker.operation = Swap{CHN, 12345, GOLD, 99, HOLDER};
    marker.outputs = {{0, Asset(GOLD, 7)}, {2, std::nullopt}, {3, Unit(Token::Kind::RECEIPT, uint256{})}};
    const CScript script{MarkerScript(marker)};
    BOOST_CHECK(IsMarkerScript(script));
    const auto parsed{ParseMarkerScript(script)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(*parsed == marker);
    // One way to write it: an extra byte is not a marker.
    CScript longer{CScript() << OP_RETURN << [&] { auto d{std::vector<unsigned char>(script.begin() + 2, script.end())}; d.push_back(0); return d; }()};
    BOOST_CHECK(!ParseMarkerScript(longer));
}

BOOST_AUTO_TEST_CASE(seq_format)
{
    for (uint32_t seq : {0u, 1u, 99999999u, 100000000u, 4000000000u}) BOOST_CHECK_EQUAL(*ParseSeq(FormatSeq(seq)), seq);
}

BOOST_AUTO_TEST_CASE(auction_cost_of_remaining)
{
    // 1 unit (no decimals) for 100 CHN: what buys it is 100 CHN, not the most that buys no more than
    // it (199.99999999 CHN, as the wallet's "buy all" paid).
    Auction auction;
    auction.base_amount = 1;
    auction.remaining = 1;
    auction.start_price = auction.end_price = 100 * COIN;
    auction.duration = 10;
    BOOST_CHECK_EQUAL(auction.CostOfRemaining(5), uint64_t(100 * COIN));
    BOOST_CHECK_EQUAL(auction.BuysAt(5, auction.CostOfRemaining(5)), 1u);
    BOOST_CHECK_EQUAL(auction.BuysAt(5, auction.CostOfRemaining(5) - 1), 0u);
    // 300 for 7: after 42 sold, 258 left; the least that buys them buys 300.
    auction.base_amount = 300;
    auction.remaining = 258;
    auction.start_price = auction.end_price = 7;
    BOOST_CHECK_EQUAL(auction.CostOfRemaining(5), 7u);
    BOOST_CHECK(auction.BuysAt(5, 7) > auction.remaining);
    BOOST_CHECK(auction.BuysAt(5, 6) < auction.remaining);
}

BOOST_AUTO_TEST_CASE(asset_arg_number_never_a_name)
{
    // The number of an asset retired since (or not given yet): an error, never the asset whose
    // name the number is (a private look-alike may have it).
    const std::string number{FormatSeq(7)};
    const AssetId gold{HashName("GOLD")};
    const auto lookup{[&](uint32_t seq) -> std::optional<AssetId> { return seq == 3 ? std::optional{gold} : std::nullopt; }};
    BOOST_CHECK(ParseAssetArg(UniValue{FormatSeq(3)}, lookup) == gold);
    BOOST_CHECK_THROW(ParseAssetArg(UniValue{number}, lookup), UniValue);
    BOOST_CHECK(ParseAssetArg(UniValue{"GOLD"}, lookup) == gold);
    BOOST_CHECK(ParseAssetArg(UniValue{"chn"}, lookup) == CHN);
}

BOOST_AUTO_TEST_CASE(register_and_conserve)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1000, 2)};
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).supply, 1000u);
    BOOST_CHECK(f.state.Tokens().at(control) == Unit(Token::Kind::CONTROL, GOLD));
    BOOST_CHECK(f.state.Tokens().at(coins) == Asset(GOLD, 1000));
    BOOST_CHECK(f.state.Reservations().empty());

    // Taken, even by a matching reservation.
    {
        const uint256 nonce{HashName("other")};
        const CTransaction reserve{MakeReserve(ReservationCommitment(GOLD, nonce, HOLDER))};
        BOOST_REQUIRE(f.Apply(reserve));
        bitassets::Register reg;
        reg.name = GOLD;
        reg.nonce = nonce;
        BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})), "bad-ba-name-taken");
    }

    // Zero supply works (it did not in the original).
    const auto [silver_control, none]{f.Issue("SILVER", 0)};
    BOOST_CHECK_EQUAL(f.state.Assets().at(SILVER).supply, 0u);

    // Moving coins: all accounted for.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, std::nullopt, {Asset(GOLD, 600)})), "bad-ba-tokens-lost");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, std::nullopt, {Asset(GOLD, 600), Asset(GOLD, 401)})), "bad-ba-tokens-unbacked");
    BOOST_CHECK(f.Reject(MakeTx({coins}, std::nullopt, {Asset(GOLD, 600), Asset(SILVER, 400)})).starts_with("bad-ba-tokens-"));
    const CTransaction split{MakeTx({coins}, std::nullopt, {Asset(GOLD, 600), Asset(GOLD, 400)})};
    BOOST_CHECK(f.Apply(split));
    BOOST_CHECK(!f.state.Tokens().contains(coins));
    // Spending tokens without a marker loses them: refused.
    CMutableTransaction plain;
    plain.vin.emplace_back(COutPoint{split.GetHash(), 0});
    plain.vout.emplace_back(1000, HOLDER);
    std::string reason;
    BOOST_CHECK(!f.state.CheckTx(CTransaction{plain}, f.height, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-tokens-lost");

    // Minting needs the control coin of that asset, not of another (the original let any do).
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({silver_control}, Mint{GOLD, 5}, {Unit(Token::Kind::CONTROL, SILVER), Asset(GOLD, 5)})), "bad-ba-no-control");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({silver_control}, UpdateAsset{GOLD, AssetUpdates{.info = {UpdateKind::SET, "x"}}}, {Unit(Token::Kind::CONTROL, SILVER)})), "bad-ba-no-control");
    const CTransaction mint{MakeTx({control}, Mint{GOLD, 50}, {Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 50)})};
    BOOST_CHECK(f.Apply(mint));
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).supply, 1050u);
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).minted, 1050u);

    // Updating its data.
    const CTransaction update{MakeTx({COutPoint{mint.GetHash(), 0}}, UpdateAsset{GOLD, AssetUpdates{.info = {UpdateKind::SET, "One gram"}}}, {Unit(Token::Kind::CONTROL, GOLD)})};
    BOOST_CHECK(f.Apply(update));
    BOOST_CHECK(f.state.Assets().at(GOLD).Current().info == "One gram");

    // Burning coins, then the control coin: the supply is fixed.
    const CTransaction burn{MakeTx({COutPoint{split.GetHash(), 1}}, Burn{{Asset(GOLD, 100)}}, {Asset(GOLD, 300)})};
    BOOST_CHECK(f.Apply(burn));
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).supply, 950u);
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).burned, 100u);
    const CTransaction fix{MakeTx({COutPoint{update.GetHash(), 0}}, Burn{{Unit(Token::Kind::CONTROL, GOLD)}}, {})};
    BOOST_CHECK(f.Apply(fix));
    BOOST_CHECK(f.state.Assets().at(GOLD).fixed);
}

BOOST_AUTO_TEST_CASE(reservation_bound_to_script)
{
    // The second audit: anyone could copy a reservation from the mempool with a higher fee; the copy,
    // older, made the maker's registration fail (not the oldest) and could not register either (the
    // nonce unknown). Bound to the script of its output, a reservation registers whatever copies exist.
    Fixture f;
    const CScript mine{HOLDER};
    const CScript theirs{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
    const uint256 nonce{HashName("secret")};
    const uint256 commitment{ReservationCommitment(GOLD, nonce, mine)};
    BOOST_CHECK(commitment != ReservationCommitment(GOLD, nonce, theirs));
    // The copy goes first, then the maker's.
    const CTransaction copy{MakeReserve(commitment, theirs)};
    BOOST_REQUIRE(f.Apply(copy));
    const CTransaction reservation{MakeReserve(commitment, mine)};
    BOOST_REQUIRE(f.Apply(reservation));
    BOOST_CHECK(f.state.View().GetReservationOrigin(reservation.GetHash()) == (ReservationOrigin{mine, f.height}));
    bitassets::Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.text = "GOLD";
    // The copier, once the nonce is out: their reservation commits to their script, not to the name with it.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{copy.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})), "bad-ba-no-reservation");
    // The maker, though younger.
    std::string reason;
    BOOST_CHECK_MESSAGE(f.Apply(MakeTx({COutPoint{reservation.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)}), &reason), reason);
    BOOST_CHECK(f.state.Assets().contains(GOLD));
    BOOST_CHECK(!f.state.View().GetReservationOrigin(reservation.GetHash()));

    // Undone, the reservations and where they were made are back as they were.
    TState reverted{f.state};
    reverted.Revert(f.undo);
    BOOST_CHECK(reverted == TState{});
}

BOOST_AUTO_TEST_CASE(pools)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    // A pool of GOLD and CHN: the marker burns the CHN that goes in, exactly.
    const AddLiquidity add{GOLD, CHN, 100'000, 50'000'000, 1};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, add, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 49'999'999)), "bad-ba-marker-value");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, add, {Asset(GOLD, 900'000)}, 50'000'000)), "bad-ba-results");
    const CTransaction added{MakeTx({coins}, add, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 50'000'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    const uint256 id{PoolId(GOLD, CHN)};
    const Pool pool{f.state.Pools().at(id)};
    BOOST_CHECK(pool.asset0 == CHN);
    BOOST_CHECK_EQUAL(pool.reserve0, 50'000'000u);
    BOOST_CHECK_EQUAL(pool.reserve1, 100'000u);
    const uint64_t shares{2'236'067}; // isqrt(5e12)
    BOOST_CHECK_EQUAL(pool.shares, shares);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{added.GetHash(), 1}) == (Token{Token::Kind::LP, id, shares - MIN_LIQUIDITY}));

    // Selling GOLD for CHN: CHN comes out by the coinbase, at least what was asked.
    const uint64_t out{amm::SwapOut(100'000, 50'000'000, 1'000)};
    BOOST_CHECK_EQUAL(out, 493'579u);
    const COutPoint gold{added.GetHash(), 0};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, Swap{GOLD, 1'000, CHN, out + 1, HOLDER}, {Asset(GOLD, 899'000)})), "bad-ba-swap-price");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, Swap{GOLD, 1'000, CHN, out, CScript{}}, {Asset(GOLD, 899'000)})), "bad-ba-chn-to");
    f.payouts.clear();
    const CTransaction sold{MakeTx({gold}, Swap{GOLD, 1'000, CHN, out, HOLDER}, {Asset(GOLD, 899'000)})};
    BOOST_REQUIRE(f.Apply(sold));
    BOOST_REQUIRE_EQUAL(f.payouts.size(), 1u);
    BOOST_CHECK_EQUAL(f.payouts[0].nValue, static_cast<CAmount>(out));
    BOOST_CHECK_EQUAL(f.state.Pools().at(id).reserve1, 101'000u);
    BOOST_CHECK_EQUAL(f.state.Pools().at(id).reserve0, 50'000'000u - out);
    // The product of the reserves never falls.
    BOOST_CHECK(static_cast<unsigned __int128>(f.state.Pools().at(id).reserve0) * f.state.Pools().at(id).reserve1 >= static_cast<unsigned __int128>(50'000'000) * 100'000);

    // Buying GOLD with CHN: the GOLD goes to the result output.
    const uint64_t gold_out{amm::SwapOut(f.state.Pools().at(id).reserve0, f.state.Pools().at(id).reserve1, 1'000'000)};
    const CTransaction bought{MakeTx({}, Swap{CHN, 1'000'000, GOLD, gold_out, CScript{}}, {std::nullopt}, 1'000'000)};
    BOOST_REQUIRE(f.Apply(bought));
    BOOST_CHECK(f.state.Tokens().at(COutPoint{bought.GetHash(), 0}) == Asset(GOLD, gold_out));

    // Swapping nothing out of an empty side, or in a pool that is not there.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Swap{CHN, 1'000, SILVER, 1, CScript{}}, {std::nullopt}, 1'000)), "bad-ba-no-pool");

    // Taking liquidity out: all the shares held, both assets back.
    const auto [out0, out1]{amm::Withdraw(f.state.Pools().at(id), shares - MIN_LIQUIDITY)};
    const CTransaction removed{MakeTx({COutPoint{added.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, shares - MIN_LIQUIDITY, out1, out0, HOLDER}, {std::nullopt})};
    f.payouts.clear();
    BOOST_REQUIRE_MESSAGE(f.Apply(removed, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{removed.GetHash(), 0}) == Asset(GOLD, out1));
    BOOST_REQUIRE_EQUAL(f.payouts.size(), 1u);
    BOOST_CHECK_EQUAL(f.payouts[0].nValue, static_cast<CAmount>(out0));
    // What nobody holds stays: the pool is never empty.
    BOOST_CHECK_EQUAL(f.state.Pools().at(id).shares, MIN_LIQUIDITY);
    BOOST_CHECK(f.state.Pools().at(id).reserve0 > 0 && f.state.Pools().at(id).reserve1 > 0);
}

BOOST_AUTO_TEST_CASE(abandoned_pools_close)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    // Too small to open.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, AddLiquidity{GOLD, CHN, 100, 900'000, 1}, {Asset(GOLD, 999'900), std::nullopt, std::nullopt}, 900'000)), "bad-ba-pool-too-small");
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 50'000'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    const uint256 id{PoolId(GOLD, CHN)};
    const uint64_t held{f.state.Pools().at(id).shares - MIN_LIQUIDITY};
    // Everyone leaves: the pool is abandoned.
    const auto [out0, out1]{amm::Withdraw(f.state.Pools().at(id), held)};
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{added.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, held, out1, out0, HOLDER}, {std::nullopt})));
    BOOST_CHECK(amm::Abandoned(f.state.Pools().at(id)));
    // Closed to trades: nothing can go into it to be lost.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Swap{CHN, 500'000'000, GOLD, 1, CScript{}}, {std::nullopt}, 500'000'000)), "bad-ba-pool-closed");
    // A dust deposit does not reopen it; a real one does, and it trades again.
    const COutPoint gold{added.GetHash(), 0};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, AddLiquidity{GOLD, CHN, 10, 100'000, 1}, {Asset(GOLD, 899'990), std::nullopt, std::nullopt}, 100'000)), "bad-ba-pool-too-small");
    const CTransaction reopened{MakeTx({gold}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 800'000), std::nullopt, std::nullopt}, 50'000'000)};
    BOOST_REQUIRE_MESSAGE(f.Apply(reopened, &reason), reason);
    BOOST_CHECK(!amm::Abandoned(f.state.Pools().at(id)));
    BOOST_CHECK(f.Apply(MakeTx({}, Swap{CHN, 1'000'000, GOLD, 1, CScript{}}, {std::nullopt}, 1'000'000)));
}

BOOST_AUTO_TEST_CASE(liquidity_reopen_front_run)
{
    // The fourth audit: a pool everyone left (only its MIN_LIQUIDITY shares nobody holds, and their
    // dust) took a deposit at the price of its dust, both amounts whole, for the shares of the side
    // that gave fewer. Front-running a reopening with a deposit at the dust's price, Mallory got the
    // shares of the price she chose, then the reopener's deposit gave her its excess.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 10'000'000)};
    const CTransaction opened{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 9'900'000), std::nullopt, std::nullopt}, 50'000'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(opened, &reason), reason);
    const uint256 id{PoolId(GOLD, CHN)};
    const uint64_t held{f.state.Pools().at(id).shares - MIN_LIQUIDITY};
    const auto [w0, w1]{amm::Withdraw(f.state.Pools().at(id), held)};
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{opened.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, held, w1, w0, HOLDER}, {std::nullopt})));
    const Pool dusty{f.state.Pools().at(id)};
    BOOST_REQUIRE(amm::Abandoned(dusty) && dusty.asset0 == CHN);
    const uint64_t dust_chn{dusty.reserve0}, dust_gold{dusty.reserve1};
    BOOST_REQUIRE(dust_chn > 0 && dust_gold > 0);
    const CTransaction split{MakeTx({COutPoint{opened.GetHash(), 0}}, std::nullopt, {Asset(GOLD, 5'000'000), Asset(GOLD, 4'900'000)})};
    BOOST_REQUIRE(f.Apply(split));
    const COutPoint mallory_gold{split.GetHash(), 0}, alice_gold{split.GetHash(), 1};
    const CScript alice_script{CScript() << OP_0 << std::vector<unsigned char>(20, 7)};

    // Alice reopens it at 5000 satoshis a unit: 100'000 units and 5 CHN, what the pool reopens with,
    // the dust counted (the wallet offers the rest). Quoted: isqrt(5e13) shares, MIN_LIQUIDITY aside.
    const uint64_t alice_chn{500'000'000 - dust_chn}, alice_units{100'000 - dust_gold};
    const auto quote{amm::Provide(dusty, alice_chn, alice_units)};
    BOOST_REQUIRE(quote && quote->opens);
    BOOST_CHECK_EQUAL(quote->shares, 7'071'067u - MIN_LIQUIDITY);
    const auto alice_tx{[&](uint64_t min_shares) {
        CMutableTransaction tx{MakeTx({alice_gold}, AddLiquidity{GOLD, CHN, alice_units, alice_chn, min_shares}, {Asset(GOLD, 4'900'000 - alice_units), std::nullopt, std::nullopt}, alice_chn)};
        // What comes back goes to Alice's address.
        tx.vout[2].scriptPubKey = alice_script;
        return CTransaction{tx};
    }};
    // Mallory, first: a deposit at the dust's price.
    const uint64_t m_chn{5'000'000}, m_gold{m_chn * dust_gold / dust_chn};
    const auto mallory_tx{[&](bool two_results = true) {
        std::vector<std::optional<Token>> outs{Asset(GOLD, 5'000'000 - m_gold), std::nullopt};
        if (two_results) outs.emplace_back();
        return MakeTx({mallory_gold}, AddLiquidity{GOLD, CHN, m_gold, m_chn, 1}, outs, m_chn);
    }};

    // A transaction lists two result outputs, the shares and what comes back: one alone is refused.
    BOOST_CHECK_EQUAL(f.Reject(mallory_tx(false)), "bad-ba-results");
    const TState before_mallory{f.state};
    const CTransaction mallory{mallory_tx()};
    BOOST_REQUIRE_MESSAGE(f.Apply(mallory, &reason), reason);
    // A reopening is a new pool, with the dust merged in: Mallory set its price.
    const Pool reopened{f.state.Pools().at(id)};
    BOOST_CHECK_EQUAL(reopened.reserve0, dust_chn + m_chn);
    BOOST_CHECK_EQUAL(reopened.reserve1, dust_gold + m_gold);
    const uint64_t mallory_shares{f.state.Tokens().at(COutPoint{mallory.GetHash(), 1}).amount};
    BOOST_CHECK_EQUAL(reopened.shares, mallory_shares + MIN_LIQUIDITY);
    BOOST_CHECK(!f.state.Tokens().contains(COutPoint{mallory.GetHash(), 2}));
    // Alice's deposit, made for her price, would go in at Mallory's and give far fewer shares than
    // quoted: refused, nothing lost.
    BOOST_CHECK_EQUAL(f.Reject(alice_tx(quote->shares * 99 / 100)), "bad-ba-liquidity-price");
    // Even with no least shares, nothing goes to Mallory: only what the price takes goes in, the rest
    // of Alice's CHN comes back, by the coinbase, to the address of her second result output.
    f.payouts.clear();
    const CTransaction alice{alice_tx(1)};
    BOOST_REQUIRE_MESSAGE(f.Apply(alice, &reason), reason);
    const Pool after{f.state.Pools().at(id)};
    BOOST_REQUIRE_EQUAL(f.payouts.size(), 1u);
    BOOST_CHECK(f.payouts[0].scriptPubKey == alice_script);
    // CHN and GOLD are conserved exactly: what went in and what came back are what Alice offered.
    BOOST_CHECK_EQUAL(after.reserve0 - reopened.reserve0 + static_cast<uint64_t>(f.payouts[0].nValue), alice_chn);
    BOOST_CHECK_EQUAL(after.reserve1 - reopened.reserve1, alice_units);
    BOOST_CHECK(!f.state.Tokens().contains(COutPoint{alice.GetHash(), 2}));
    // Mallory's shares are worth what she and the dust put in, rounding aside: not Alice's CHN.
    BOOST_CHECK(amm::Withdraw(after, mallory_shares).first <= dust_chn + m_chn + 1);

    // An honest race: Bob reopens it at Alice's price first. Alice's deposit goes in at that price,
    // for at least her least shares, and the CHN its price does not take comes back.
    Fixture g;
    g.state = before_mallory;
    BOOST_REQUIRE(g.state.Pools().at(id) == dusty);
    const CTransaction bob{MakeTx({mallory_gold}, AddLiquidity{GOLD, CHN, 10'000 - dust_gold, 50'000'000 - dust_chn, 1}, {Asset(GOLD, 5'000'000 - 10'000 + dust_gold), std::nullopt, std::nullopt}, 50'000'000 - dust_chn)};
    BOOST_REQUIRE_MESSAGE(g.Apply(bob, &reason), reason);
    BOOST_CHECK_EQUAL(g.state.Pools().at(id).reserve0, 50'000'000u);
    BOOST_CHECK_EQUAL(g.state.Pools().at(id).reserve1, 10'000u);
    g.payouts.clear();
    const CTransaction raced{alice_tx(quote->shares * 99 / 100)};
    BOOST_REQUIRE_MESSAGE(g.Apply(raced, &reason), reason);
    BOOST_CHECK(g.state.Tokens().at(COutPoint{raced.GetHash(), 1}).amount >= quote->shares * 99 / 100);
    BOOST_REQUIRE_EQUAL(g.payouts.size(), 1u);
    BOOST_CHECK(g.payouts[0].scriptPubKey == alice_script);

    // Too much GOLD into a live pool: its excess comes back as GOLD, to the second result output.
    const COutPoint more_gold{raced.GetHash(), 0};
    const Pool live{g.state.Pools().at(id)};
    const auto deposit{amm::Provide(live, 5'000'000, 500'000)};
    BOOST_REQUIRE(deposit && !deposit->opens);
    BOOST_CHECK_EQUAL(deposit->take0, 5'000'000u);
    const uint64_t back{500'000 - deposit->take1};
    BOOST_REQUIRE(back > 0);
    const uint64_t have{4'900'000 - alice_units};
    const CTransaction excess{MakeTx({more_gold}, AddLiquidity{GOLD, CHN, 500'000, 5'000'000, 1}, {Asset(GOLD, have - 500'000), std::nullopt, std::nullopt}, 5'000'000)};
    BOOST_REQUIRE_MESSAGE(g.Apply(excess, &reason), reason);
    BOOST_CHECK(g.state.Tokens().at(COutPoint{excess.GetHash(), 2}) == Asset(GOLD, back));
    BOOST_CHECK_EQUAL(g.state.Pools().at(id).reserve1, live.reserve1 + deposit->take1);
    BOOST_CHECK_EQUAL(g.state.Assets().at(GOLD).supply, 10'000'000u);
}

BOOST_AUTO_TEST_CASE(dead_assets_retire)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 50'000'000)};
    BOOST_REQUIRE(f.Apply(added));
    const uint256 id{PoolId(GOLD, CHN)};
    // Alive: its control coin exists, someone holds some, its pool has providers.
    std::string why;
    BOOST_CHECK(!f.state.Releasable(GOLD, &why));
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, ReleaseAsset{GOLD}, {})), "bad-ba-release-alive");
    const uint64_t held{f.state.Pools().at(id).shares - MIN_LIQUIDITY};
    const auto [out0, out1]{amm::Withdraw(f.state.Pools().at(id), held)};
    const CTransaction removed{MakeTx({COutPoint{added.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, held, out1, out0, HOLDER}, {std::nullopt})};
    BOOST_REQUIRE(f.Apply(removed));
    BOOST_REQUIRE(f.Apply(MakeTx({control}, Burn{{Unit(Token::Kind::CONTROL, GOLD)}}, {})));
    BOOST_CHECK(!f.state.Releasable(GOLD, &why));
    BOOST_CHECK_EQUAL(why, "someone holds some of it");
    // Everyone's coins burned: only the pool's are left.
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{added.GetHash(), 0}}, Burn{{Asset(GOLD, 900'000)}}, {})));
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{removed.GetHash(), 0}}, Burn{{Asset(GOLD, out1)}}, {})));
    BOOST_CHECK(f.state.Releasable(GOLD));
    const CAmount left{static_cast<CAmount>(f.state.Pools().at(id).reserve0)};
    std::string reason;
    const TState before{f.state};
    f.undo = TUndo{};
    CAmount released{0};
    BOOST_REQUIRE(f.state.ApplyTx(MakeTx({}, ReleaseAsset{GOLD}, {}), f.height, f.undo, f.payouts, reason, &released));
    BOOST_CHECK_EQUAL(released, left);
    BOOST_CHECK(!f.state.Assets().contains(GOLD));
    BOOST_CHECK(!f.state.Pools().contains(id));
    // Undone, it is all back.
    f.state.Revert(f.undo);
    BOOST_CHECK(f.state == before);
}

BOOST_AUTO_TEST_CASE(auction_does_not_keep_quote_alive)
{
    // The second audit: an auction of 1 unit of anything, quoting an asset, starting never and never
    // collected, kept that asset from ever being retired.
    Fixture f;
    const auto [gold_control, gold]{f.Issue("GOLD", 1000)};
    const auto [silver_control, silver]{f.Issue("SILVER", 10)};
    // At most MAX_AUCTION_DELAY blocks ahead.
    const auto auction_of{[&](int start) { return MakeTx({silver}, CreateAuction{SILVER, 1, GOLD, 10, 10, start, 10}, {Asset(SILVER, 9), Unit(Token::Kind::RECEIPT, uint256{})}); }};
    BOOST_CHECK_EQUAL(f.Reject(auction_of(f.height + MAX_AUCTION_DELAY + 1)), "bad-ba-auction-delay");
    BOOST_CHECK_EQUAL(f.Reject(auction_of(std::numeric_limits<int32_t>::max())), "bad-ba-auction-delay");
    std::string reason;
    const CTransaction made{auction_of(f.height + 1)};
    BOOST_REQUIRE_MESSAGE(f.Apply(made, &reason), reason);
    BOOST_CHECK(f.state.Auctions().at(made.GetHash()).quote_registration == f.state.Assets().at(GOLD).registration);
    // GOLD dies: its control coin and every coin burned.
    BOOST_REQUIRE(f.Apply(MakeTx({gold_control, gold}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 1000)}}, {})));
    std::string why;
    BOOST_CHECK(f.state.Releasable(GOLD));
    // SILVER, which the auction sells, stays alive by it.
    BOOST_REQUIRE(f.Apply(MakeTx({silver_control, COutPoint{made.GetHash(), 0}}, Burn{{Unit(Token::Kind::CONTROL, SILVER), Asset(SILVER, 9)}}, {})));
    BOOST_CHECK(!f.state.Releasable(SILVER, &why));
    BOOST_CHECK_EQUAL(why, "an auction of it is not collected");
    BOOST_REQUIRE(f.Apply(MakeTx({}, ReleaseAsset{GOLD}, {})));
    // GOLD again, another asset: the auction takes none of it.
    const auto [gold2_control, gold2]{f.Issue("GOLD", 50)};
    f.height = 101;
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold2}, Bid{made.GetHash(), 10, 1, CScript{}}, {Asset(GOLD, 40), std::nullopt})), "bad-ba-auction-asset-gone");
    // Collected, what it sells goes back.
    BOOST_REQUIRE_MESSAGE(f.Apply(MakeTx({COutPoint{made.GetHash(), 1}}, Collect{made.GetHash(), HOLDER}, {std::nullopt}), &reason), reason);

    // One that took some in keeps it alive.
    Fixture g;
    const auto [c1, g_gold]{g.Issue("GOLD", 1000)};
    const auto [c2, g_silver]{g.Issue("SILVER", 10)};
    const CTransaction auction{MakeTx({g_silver}, CreateAuction{SILVER, 2, GOLD, 10, 10, g.height, 10}, {Asset(SILVER, 8), Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE(g.Apply(auction));
    const CTransaction bid{MakeTx({g_gold}, Bid{auction.GetHash(), 5, 1, CScript{}}, {Asset(GOLD, 995), std::nullopt})};
    BOOST_REQUIRE_MESSAGE(g.Apply(bid, &reason), reason);
    BOOST_REQUIRE(g.Apply(MakeTx({c1, COutPoint{bid.GetHash(), 0}}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 995)}}, {})));
    BOOST_CHECK(!g.state.Releasable(GOLD, &why));
    BOOST_CHECK_EQUAL(why, "an auction of it is not collected");
}

BOOST_AUTO_TEST_CASE(names_that_read_as_others)
{
    for (const char* name : {"CHN", "chn", "cHn", "1234-5678", "0001-2345-6789", "0x", "0xabc", "0X12"}) BOOST_CHECK_MESSAGE(ReadsAsAnotherAsset(name), name);
    for (const char* name : {"CHNX", "CH", "1234-567", "12345678", "1234_5678", "1234-5678-", "x0", "GOLD", "0 x"}) BOOST_CHECK_MESSAGE(!ReadsAsAnotherAsset(name), name);

    const auto try_register{[](const std::string& name, bool publish) {
        Fixture f;
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("n")};
        const CTransaction reserve{MakeReserve(ReservationCommitment(id, nonce, HOLDER))};
        BOOST_REQUIRE(f.Apply(reserve));
        bitassets::Register reg;
        reg.name = id;
        reg.nonce = nonce;
        if (publish) reg.text = name;
        std::string reason;
        f.Apply(MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, id)}), &reason);
        return reason;
    }};
    for (const char* name : {"CHN", "Chn", "1739-0029", "0001-1739-0029", "0xdead"}) {
        BOOST_CHECK_EQUAL(try_register(name, true), "bad-ba-name-reserved");
    }
    // A private name is checked as far as it can be: CHN, by its hash.
    BOOST_CHECK_EQUAL(try_register("chn", false), "bad-ba-name-reserved");
    BOOST_CHECK_EQUAL(try_register("1739-0029", false), "");
    BOOST_CHECK_EQUAL(try_register("CHAIN", true), "");
}

BOOST_AUTO_TEST_CASE(small_providers_and_dust)
{
    // The second audit: a provider whose shares round to nothing on one side could not leave; CHN
    // results of a few satoshis each took a place in the coinbase's payout queue.
    Fixture f;
    const auto [gc, gold]{f.Issue("GOLD", 20'000'000)};
    const auto [sc, silver]{f.Issue("SILVER", 2'000)};
    const CTransaction added{MakeTx({gold, silver}, AddLiquidity{GOLD, SILVER, 10'000'000, 2'000, 1}, {Asset(GOLD, 10'000'000), std::nullopt, std::nullopt})};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    const COutPoint lp{added.GetHash(), 1};
    const uint64_t held{f.state.Tokens().at(lp).amount};
    const uint256 id{PoolId(GOLD, SILVER)};
    // 10 shares take 707 GOLD and no SILVER.
    const auto [out0, out1]{amm::Withdraw(f.state.Pools().at(id), 10)};
    const bool gold_first{f.state.Pools().at(id).asset0 == GOLD};
    BOOST_CHECK_EQUAL(gold_first ? out1 : out0, 0u);
    const uint64_t gold_out{gold_first ? out0 : out1};
    BOOST_CHECK(gold_out > 0);
    const CTransaction leave{MakeTx({lp}, RemoveLiquidity{GOLD, SILVER, 10, 1, 0, CScript{}}, {Token{Token::Kind::LP, id, held - 10}, std::nullopt})};
    BOOST_REQUIRE_MESSAGE(f.Apply(leave, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{leave.GetHash(), 1}) == Asset(GOLD, gold_out));
    // Asking for something of the side that gives nothing: refused.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{leave.GetHash(), 0}}, RemoveLiquidity{GOLD, SILVER, 10, 1, 1, CScript{}}, {Token{Token::Kind::LP, id, held - 20}, std::nullopt})), "bad-ba-liquidity-price");

    // With CHN: dust stays in the pool.
    const CTransaction chn_pool{MakeTx({COutPoint{added.GetHash(), 0}}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 9'900'000), std::nullopt, std::nullopt}, 50'000'000)};
    BOOST_REQUIRE_MESSAGE(f.Apply(chn_pool, &reason), reason);
    const uint256 chn_id{PoolId(GOLD, CHN)};
    const COutPoint chn_lp{chn_pool.GetHash(), 1};
    const uint64_t chn_held{f.state.Tokens().at(chn_lp).amount};
    // 40 shares: 894 sat of CHN (stays) and 1 GOLD.
    BOOST_CHECK(amm::Withdraw(f.state.Pools().at(chn_id), 40) == std::make_pair(uint64_t{894}, uint64_t{1}));
    BOOST_CHECK(amm::WithdrawPaid(f.state.Pools().at(chn_id), 40) == std::make_pair(uint64_t{0}, uint64_t{1}));
    const uint64_t chn_before{f.state.Pools().at(chn_id).reserve0};
    f.payouts.clear();
    const CTransaction out_gold{MakeTx({chn_lp}, RemoveLiquidity{GOLD, CHN, 40, 1, 0, HOLDER}, {Token{Token::Kind::LP, chn_id, chn_held - 40}, std::nullopt})};
    BOOST_REQUIRE_MESSAGE(f.Apply(out_gold, &reason), reason);
    BOOST_CHECK(f.payouts.empty());
    BOOST_CHECK_EQUAL(f.state.Pools().at(chn_id).reserve0, chn_before);
    // 10 shares: nothing on either side worth paying: refused.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{out_gold.GetHash(), 0}}, RemoveLiquidity{GOLD, CHN, 10, 0, 0, HOLDER}, {Token{Token::Kind::LP, chn_id, chn_held - 50}})), "bad-ba-liquidity-price");

    // A swap or a bid paying CHN dust: refused.
    const COutPoint some_gold{chn_pool.GetHash(), 0};
    BOOST_CHECK(amm::SwapOut(100'000, chn_before, 1) < MIN_CHN_PAYOUT);
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({some_gold}, Swap{GOLD, 1, CHN, 1, HOLDER}, {Asset(GOLD, 9'899'999)})), "bad-ba-chn-dust");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({some_gold}, Swap{GOLD, 10, CHN, 1, HOLDER}, {Asset(GOLD, 9'899'990)})), "bad-ba-chn-dust");
    BOOST_CHECK(amm::SwapOut(100'000, chn_before, 200) >= MIN_CHN_PAYOUT);
    BOOST_CHECK(f.Apply(MakeTx({some_gold}, Swap{GOLD, 200, CHN, 1, HOLDER}, {Asset(GOLD, 9'899'800)})));

    // Collecting CHN dust from an auction: not paid out.
    Fixture g;
    const auto [c, coins]{g.Issue("GOLD", 100)};
    const CTransaction auction{MakeTx({coins}, CreateAuction{GOLD, 10, CHN, 100, 100, g.height, 2}, {Asset(GOLD, 90), Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE(g.Apply(auction));
    BOOST_REQUIRE(g.Apply(MakeTx({}, Bid{auction.GetHash(), 10, 1, CScript{}}, {std::nullopt}, 10)));
    g.height += 2;
    g.payouts.clear();
    BOOST_REQUIRE_MESSAGE(g.Apply(MakeTx({COutPoint{auction.GetHash(), 1}}, Collect{auction.GetHash(), HOLDER}, {std::nullopt}), &reason), reason);
    BOOST_CHECK(g.payouts.empty());
}

BOOST_AUTO_TEST_CASE(swap_math_exact)
{
    // The swap math against a 256 bit reference, where amount * 997 * reserve does not fit in 128 bits.
    const auto reference_out{[](uint64_t rin, uint64_t rout, uint64_t in) {
        const arith_uint256 fee_in{arith_uint256{in} * 997};
        return (fee_in * arith_uint256{rout} / (arith_uint256{rin} * 1000 + fee_in)).GetLow64();
    }};
    const uint64_t big{MAX_AMOUNT / 2};
    for (const auto& [rin, rout, in] : std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>{
             {uint64_t{1} << 62, uint64_t{1} << 62, uint64_t{1} << 61},
             {big, big, big / 2},
             {1000, MAX_AMOUNT, MAX_AMOUNT},
             {MAX_AMOUNT, 1000, 1},
             {50'000'000, 100'000, 10'000},
         }) {
        const uint64_t out{amm::SwapOut(rin, rout, in)};
        BOOST_CHECK_EQUAL(out, reference_out(rin, rout, in));
        BOOST_CHECK(out < rout);
        // The reverse quote pays at least as much as the amount it was asked for.
        if (out > 0) {
            const auto needed{amm::SwapIn(rin, rout, out)};
            if (needed) BOOST_CHECK(amm::SwapOut(rin, rout, *needed) >= out);
        }
    }
}

BOOST_AUTO_TEST_CASE(retired_number_names_nothing)
{
    // After an asset is retired and its name registered again, the old number names nothing.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1000)};
    const uint32_t old_seq{f.state.Assets().at(GOLD).seq};
    BOOST_CHECK(f.state.AssetOfSeq(old_seq) == GOLD);
    BOOST_REQUIRE(f.Apply(MakeTx({control, coins}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 1000)}}, {})));
    BOOST_REQUIRE(f.Apply(MakeTx({}, ReleaseAsset{GOLD}, {})));
    BOOST_CHECK(!f.state.AssetOfSeq(old_seq));
    f.Issue("GOLD", 5);
    BOOST_CHECK(f.state.Assets().at(GOLD).seq != old_seq);
    BOOST_CHECK(!f.state.AssetOfSeq(old_seq));
    BOOST_CHECK(f.state.AssetOfSeq(f.state.Assets().at(GOLD).seq) == GOLD);
}

BOOST_AUTO_TEST_CASE(auction_prices)
{
    Auction a;
    a.base_amount = 2;
    a.start_price = 1000;
    a.end_price = 100;
    a.start_height = 10;
    a.duration = 10;
    a.remaining = 2;
    BOOST_CHECK_EQUAL(a.PriceAt(10), 1000u);
    BOOST_CHECK_EQUAL(a.PriceAt(19), 100u);
    BOOST_CHECK_EQUAL(a.PriceAt(14), 600u);
    // Rounded down: 501 of 1000 for 2 units buys 1, not 2 (the original gave both).
    BOOST_CHECK_EQUAL(a.BuysAt(10, 501), 1u);
    BOOST_CHECK_EQUAL(a.BuysAt(10, 1000), 2u);
    BOOST_CHECK(a.OpenAt(19));
    BOOST_CHECK(!a.OpenAt(20));
}

BOOST_AUTO_TEST_CASE(auctions)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 100)};
    CreateAuction create{GOLD, 40, CHN, 400'000, 40'000, f.height + 1, 10};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, CreateAuction{GOLD, 40, CHN, 400'000, 40'000, f.height - 1, 10}, {Asset(GOLD, 60), Unit(Token::Kind::RECEIPT, uint256{})})), "bad-ba-auction-started");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, create, {Asset(GOLD, 60)})), "bad-ba-tokens-lost");
    const CTransaction made{MakeTx({coins}, create, {Asset(GOLD, 60), Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE(f.Apply(made));
    const Txid id{made.GetHash()};
    const COutPoint receipt{id, 1};
    BOOST_CHECK(f.state.Tokens().at(receipt) == Unit(Token::Kind::RECEIPT, id.ToUint256()));

    // Not started yet.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 100'000, 1, CScript{}}, {std::nullopt}, 100'000)), "bad-ba-auction-closed");
    // Canceled before any bid, everything back: possible, but not now.
    f.height = 101;
    // A bid of 100000 at the start (400000 for 40) buys 10.
    const CTransaction bid{MakeTx({}, Bid{id, 100'000, 10, CScript{}}, {std::nullopt}, 100'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(bid, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{bid.GetHash(), 0}) == Asset(GOLD, 10));
    BOOST_CHECK_EQUAL(f.state.Auctions().at(id).remaining, 30u);
    // More than is left: refused.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 400'000, 1, CScript{}}, {std::nullopt}, 400'000)), "bad-ba-bid-price");
    // With a bid, it cannot be collected while it runs.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({receipt}, Collect{id, HOLDER}, {std::nullopt})), "bad-ba-auction-running");
    // In its last block, at the end price (the original's node stopped here): 40000 for 40, 1000 per unit (what is paid out is at least MIN_CHN_PAYOUT).
    f.height = 110;
    const CTransaction last{MakeTx({}, Bid{id, 10'000, 10, CScript{}}, {std::nullopt}, 10'000)};
    BOOST_REQUIRE_MESSAGE(f.Apply(last, &reason), reason);
    BOOST_CHECK_EQUAL(f.state.Auctions().at(id).remaining, 20u);
    f.height = 111;
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 10'000, 1, CScript{}}, {std::nullopt}, 10'000)), "bad-ba-auction-closed");
    // Collected: what is left (GOLD) to the result output, what came in (CHN) by the coinbase.
    f.payouts.clear();
    const CTransaction collect{MakeTx({receipt}, Collect{id, HOLDER}, {std::nullopt})};
    BOOST_REQUIRE_MESSAGE(f.Apply(collect, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{collect.GetHash(), 0}) == Asset(GOLD, 20));
    BOOST_REQUIRE_EQUAL(f.payouts.size(), 1u);
    BOOST_CHECK_EQUAL(f.payouts[0].nValue, 110'000);
    BOOST_CHECK(f.state.Auctions().at(id).closed);
}

BOOST_AUTO_TEST_CASE(history_apart)
{
    // The history of an asset's data is kept apart from its record: a change writes one entry per
    // field it changes, and the record keeps a fixed size however many changes there were.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1000)};
    const size_t record_size{sidechain::EncodeValue(f.state.Assets().at(GOLD)).size()};
    COutPoint at{control};
    std::vector<std::string> infos;
    for (int i{0}; i < 20; ++i) {
        f.height = 101 + i;
        const std::string info{strprintf("Version %d", i)};
        AssetUpdates updates{.info = {UpdateKind::SET, info}};
        // Every fifth, the commitment goes too.
        if (i % 5 == 4) updates.commitment = {UpdateKind::DELETE, std::nullopt};
        const CTransaction update{MakeTx({at}, UpdateAsset{GOLD, updates}, {Unit(Token::Kind::CONTROL, GOLD)})};
        BOOST_REQUIRE(f.Apply(update));
        at = COutPoint{update.GetHash(), 0};
        infos.push_back(info);
    }
    const AssetRecord record{f.state.Assets().at(GOLD)};
    // Only what the data is now is in it: a few bytes more for the info set, none per change.
    BOOST_CHECK_LE(sidechain::EncodeValue(record).size(), record_size + 1 + 10);
    BOOST_CHECK(record.Current().info == "Version 19");
    BOOST_CHECK_EQUAL(record.changes[static_cast<size_t>(DataField::INFO)], 21u);
    BOOST_CHECK_EQUAL(record.changes[static_cast<size_t>(DataField::COMMITMENT)], 5u);
    BOOST_CHECK_EQUAL(record.changes[static_cast<size_t>(DataField::IPV4)], 1u);
    const AssetHistory history{f.state.View().GetHistory(GOLD)};
    BOOST_REQUIRE_EQUAL(history.info.size(), 21u);
    BOOST_CHECK(!history.info[0].value);
    for (size_t i{0}; i < infos.size(); ++i) {
        BOOST_CHECK(history.info[i + 1].value == infos[i]);
        BOOST_CHECK_EQUAL(history.info[i + 1].height, 101 + static_cast<int>(i));
    }
    BOOST_CHECK_EQUAL(history.commitment.size(), 5u);
    // At a height: what it was at the end of that block; nothing before the registration.
    BOOST_CHECK(!f.state.View().DataAt(GOLD, 99));
    BOOST_CHECK(!f.state.View().DataAt(GOLD, 100)->info);
    BOOST_CHECK(f.state.View().DataAt(GOLD, 105)->info == "Version 4");
    BOOST_CHECK(f.state.View().DataAt(GOLD, 1000) == record.data);

    // Retired, its history goes; registered again, it starts again.
    BOOST_REQUIRE(f.Apply(MakeTx({at, coins}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 1000)}}, {})));
    BOOST_REQUIRE(f.Apply(MakeTx({}, ReleaseAsset{GOLD}, {})));
    BOOST_CHECK(f.state.View().GetHistory(GOLD) == AssetHistory{});
    f.Issue("GOLD", 5);
    BOOST_CHECK_EQUAL(f.state.View().GetHistory(GOLD).info.size(), 1u);
}

BOOST_AUTO_TEST_CASE(index_lookups)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1000)};
    // Many coins of it, and its control coin after them in outpoint order: found in one look.
    const CTransaction split{MakeTx({coins}, std::nullopt, {Asset(GOLD, 100), Asset(GOLD, 100), Asset(GOLD, 100), Asset(GOLD, 700)})};
    BOOST_REQUIRE(f.Apply(split));
    BOOST_CHECK(f.state.View().FirstTokenOf(GOLD, Token::Kind::CONTROL) == control);
    BOOST_CHECK(f.state.View().FirstTokenOf(GOLD, Token::Kind::ASSET).has_value());
    BOOST_CHECK(!f.state.View().FirstTokenOf(GOLD, Token::Kind::LP));

    // A closed auction leaves the index of auctions by asset.
    const CTransaction made{MakeTx({COutPoint{split.GetHash(), 0}}, CreateAuction{GOLD, 100, CHN, 4'000, 400, f.height + 1, 10}, {Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE(f.Apply(made));
    sidechain::StoreBytes by_gold{0x3b};
    by_gold.insert(by_gold.end(), GOLD.begin(), GOLD.end());
    BOOST_CHECK_EQUAL(f.state.Count(by_gold), 1u);
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{made.GetHash(), 0}}, Collect{made.GetHash(), HOLDER}, {std::nullopt})));
    BOOST_CHECK(f.state.Auctions().at(made.GetHash()).closed);
    BOOST_CHECK_EQUAL(f.state.Count(by_gold), 0u);
}

BOOST_AUTO_TEST_CASE(store_layout)
{
    // A store with assets and no layout written is of an earlier version: derived again.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/false};
    BOOST_CHECK(StoreLayoutCurrent(store));
    store.Put(sidechain::StoreBytes{0x31, 0x01}, sidechain::StoreBytes{0x00});
    BOOST_CHECK(!StoreLayoutCurrent(store));
    // Written by the first change to the assets.
    Fixture f;
    f.Issue("GOLD", 1);
    BOOST_CHECK(StoreLayoutCurrent(f.state.Store()));
    BOOST_CHECK(f.state.Store().Get(sidechain::StoreBytes{0x3f}) == sidechain::EncodeValue(STORE_LAYOUT));
}

BOOST_AUTO_TEST_CASE(revert_restores)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    const TState before{f.state};
    f.undo = TUndo{};
    // A block that makes a pool, trades in it, registers another asset and mints.
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 50'000'000)};
    BOOST_REQUIRE(f.Apply(added));
    BOOST_REQUIRE(f.Apply(MakeTx({}, Swap{CHN, 10'000, GOLD, 1, CScript{}}, {std::nullopt}, 10'000)));
    BOOST_REQUIRE(f.Apply(MakeTx({control}, Mint{GOLD, 5}, {Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 5)})));
    const TUndo undo{f.undo};
    f.Issue("SILVER", 7);
    BOOST_CHECK(!(f.state == before));
    // Undo everything since `before` (the registration's undo is in f.undo too).
    f.state.Revert(f.undo);
    BOOST_CHECK(f.state == before);
}

BOOST_AUTO_TEST_CASE(reservation_reveal_depth)
{
    // The third audit: whoever makes a block sees every registration waiting for it. It could reserve
    // the name under a nonce of its own and register it in that very block, ahead of the one it saw
    // (and ahead of its maker's, even bound to its script): nothing compared where a reservation was
    // made with where it is revealed. A registration in block h reveals a reservation made in block r
    // only if h - r >= reveal_depth.
    Fixture f;
    f.reveal_depth = 3;
    const uint256 nonce{HashName("secret")};
    const CTransaction reserve{MakeReserve(ReservationCommitment(GOLD, nonce, HOLDER))};
    BOOST_REQUIRE(f.Apply(reserve));
    bitassets::Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.text = "GOLD";
    const CTransaction registration{MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})};
    for (const int height : {100, 101, 102}) {
        f.height = height;
        BOOST_CHECK_EQUAL(f.Reject(registration), "bad-ba-reservation-too-young");
    }
    std::string reason;
    f.height = 103;
    BOOST_CHECK_MESSAGE(f.Apply(registration, &reason), reason);
    BOOST_CHECK(f.state.Assets().contains(GOLD));
}

BOOST_AUTO_TEST_CASE(release_counts)
{
    // The third audit: whether an asset could be retired went through every auction quoting it,
    // skipping those that took none of it in: 100k such auctions made each releaseasset (and
    // getasset, listassets) read 100k entries. Each record counts the auctions that
    // hold some of it and its pools with providers, kept as auctions and pools change.
    Fixture f;
    const auto [gold_control, gold]{f.Issue("GOLD", 1'000'000)};
    const auto [silver_control, silver]{f.Issue("SILVER", 1'000)};
    const auto counts{[&](const AssetId& asset) {
        const AssetRecord record{f.state.Assets().at(asset)};
        return std::make_pair(record.holding_auctions, record.provided_pools);
    }};
    using Counts = std::pair<uint32_t, uint32_t>;
    BOOST_CHECK(counts(GOLD) == (Counts{0, 0}));
    // Auctions of a unit of SILVER each, for GOLD, that take nothing in: they hold SILVER, not GOLD.
    COutPoint left{silver};
    std::vector<Txid> auctions;
    for (uint64_t i{0}; i < 50; ++i) {
        const CTransaction made{MakeTx({left}, CreateAuction{SILVER, 1, GOLD, 10, 10, f.height + 1, 10}, {Asset(SILVER, 999 - i), Unit(Token::Kind::RECEIPT, uint256{})})};
        BOOST_REQUIRE(f.Apply(made));
        auctions.push_back(made.GetHash());
        left = COutPoint{made.GetHash(), 0};
    }
    BOOST_CHECK(counts(GOLD) == (Counts{0, 0}));
    BOOST_CHECK(counts(SILVER) == (Counts{50, 0}));
    // A bid pays GOLD into one: it holds GOLD now.
    f.height += 1;
    const CTransaction bid{MakeTx({gold}, Bid{auctions[0], 10, 1, CScript{}}, {Asset(GOLD, 999'990), std::nullopt})};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(bid, &reason), reason);
    BOOST_CHECK(counts(GOLD) == (Counts{1, 0}));
    // Sold out, not collected: it still holds what bids paid in, and is still SILVER's.
    BOOST_CHECK(counts(SILVER) == (Counts{50, 0}));
    // Collected: it holds nothing.
    BOOST_REQUIRE_MESSAGE(f.Apply(MakeTx({COutPoint{auctions[0], 1}}, Collect{auctions[0], HOLDER}, {std::nullopt}), &reason), reason);
    BOOST_CHECK(counts(GOLD) == (Counts{0, 0}));
    BOOST_CHECK(counts(SILVER) == (Counts{49, 0}));
    // Canceled before any bid: the same.
    BOOST_REQUIRE_MESSAGE(f.Apply(MakeTx({COutPoint{auctions[1], 1}}, Collect{auctions[1], HOLDER}, {std::nullopt}), &reason), reason);
    BOOST_CHECK(counts(SILVER) == (Counts{48, 0}));

    // Pools: counted while someone provides liquidity to them.
    const TState before_pool{f.state};
    const TUndo undo_before{f.undo};
    const CTransaction added{MakeTx({COutPoint{bid.GetHash(), 0}}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 899'990), std::nullopt, std::nullopt}, 50'000'000)};
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    BOOST_CHECK(counts(GOLD) == (Counts{0, 1}));
    const uint256 id{PoolId(GOLD, CHN)};
    const uint64_t held{f.state.Pools().at(id).shares - MIN_LIQUIDITY};
    // Half out: still provided.
    BOOST_REQUIRE(f.Apply(MakeTx({COutPoint{added.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, held / 2, 1, 1, HOLDER}, {Token{Token::Kind::LP, id, held - held / 2}, std::nullopt})));
    BOOST_CHECK(counts(GOLD) == (Counts{0, 1}));
    // Everyone out: abandoned, not counted; reopened, counted again.
    COutPoint lp;
    for (const auto& [outpoint, token] : f.state.Tokens()) {
        if (token.kind == Token::Kind::LP) lp = outpoint;
    }
    BOOST_REQUIRE(f.Apply(MakeTx({lp}, RemoveLiquidity{GOLD, CHN, held - held / 2, 1, 1, HOLDER}, {std::nullopt})));
    BOOST_CHECK(amm::Abandoned(f.state.Pools().at(id)));
    BOOST_CHECK(counts(GOLD) == (Counts{0, 0}));
    BOOST_REQUIRE_MESSAGE(f.Apply(MakeTx({COutPoint{added.GetHash(), 0}}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 799'990), std::nullopt, std::nullopt}, 50'000'000), &reason), reason);
    BOOST_CHECK(counts(GOLD) == (Counts{0, 1}));

    // Undone, the counts are as they were.
    TUndo since;
    since.parts.assign(f.undo.parts.begin() + undo_before.parts.size(), f.undo.parts.end());
    f.state.Revert(since);
    BOOST_CHECK(f.state == before_pool);
    BOOST_CHECK(counts(GOLD) == (Counts{0, 0}));
}

BOOST_AUTO_TEST_CASE(release_of_one_satoshi)
{
    // The third audit: a release that freed exactly 1 satoshi of CHN gave it to nobody (a withdrawal to
    // mainchain miners was made from 2 on): it stayed in the mainchain's escrow, backed by nothing.
    // It is a withdrawal of 1 satoshi with no fee, to a script of its own (the release's txid in it).
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/false};
    sidechain::State side{store};
    Consensus::SidechainParams params;
    params.enabled = true;
    params.bitassets_reveal_depth = 1;
    std::vector<CTxOut> payouts;
    std::string reason;
    int height{100};
    const auto apply{[&](const CTransaction& tx) {
        const bool ok{side.ApplyTx(tx, height, params, payouts, reason)};
        BOOST_CHECK_MESSAGE(ok, reason);
        return ok;
    }};
    const uint256 nonce{HashName("n")};
    const CTransaction reserve{MakeReserve(ReservationCommitment(GOLD, nonce, HOLDER))};
    BOOST_REQUIRE(apply(reserve));
    height = 101;
    const uint64_t supply{1'000'000'000'000};
    bitassets::Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.supply = supply;
    const CTransaction registered{MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, supply)})};
    BOOST_REQUIRE(apply(registered));
    // A pool of 10^12 GOLD and 0.01 CHN: 10^9 shares, of which the MIN_LIQUIDITY nobody holds keep a
    // millionth of it, 1 satoshi, once everyone leaves.
    const CTransaction added{MakeTx({COutPoint{registered.GetHash(), 1}}, AddLiquidity{GOLD, CHN, supply, MIN_OPEN_CHN, 1}, {std::nullopt, std::nullopt}, MIN_OPEN_CHN)};
    BOOST_REQUIRE(apply(added));
    const uint256 id{PoolId(GOLD, CHN)};
    const bitassets::Pool pool{*side.BitAssets().GetPool(id)};
    const uint64_t held{pool.shares - MIN_LIQUIDITY};
    const auto [out0, out1]{amm::WithdrawPaid(pool, held)};
    const CTransaction removed{MakeTx({COutPoint{added.GetHash(), 0}}, RemoveLiquidity{GOLD, CHN, held, out1, out0, HOLDER}, {std::nullopt})};
    BOOST_REQUIRE(apply(removed));
    BOOST_REQUIRE_EQUAL(side.BitAssets().GetPool(id)->reserve0, 1u);
    BOOST_REQUIRE(apply(MakeTx({COutPoint{registered.GetHash(), 0}, COutPoint{removed.GetHash(), 0}}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, out1)}}, {})));
    height = 110;
    const CTransaction release{MakeTx({}, ReleaseAsset{GOLD}, {})};
    BOOST_REQUIRE(apply(release));
    const auto given{side.GetWithdrawal(COutPoint{release.GetHash(), sidechain::RELEASE_WITHDRAWAL_INDEX})};
    BOOST_REQUIRE(given);
    BOOST_CHECK_EQUAL(given->amount, 1);
    BOOST_CHECK_EQUAL(given->main_fee, 0);
    std::vector<unsigned char> tag{'r', 'e', 'l', 'e', 'a', 's', 'e'};
    const uint256 txid{release.GetHash().ToUint256()};
    tag.insert(tag.end(), txid.begin(), txid.end());
    BOOST_CHECK(given->main_script == (CScript() << OP_RETURN << tag));
}

namespace {
/** A transaction of a coin of its own (or a coinbase), with these outputs. */
CTransaction RawTx(std::vector<CTxOut> outs, bool coinbase = false)
{
    CMutableTransaction tx;
    if (coinbase) {
        tx.vin.emplace_back(COutPoint{});
        tx.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    } else {
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce)}), static_cast<uint32_t>(g_nonce)});
    }
    tx.vout = std::move(outs);
    return CTransaction{tx};
}

/** The script of a marker whose data after the tag is `body`, well formed or not. */
CScript RawMarker(const std::vector<unsigned char>& body)
{
    std::vector<unsigned char> data(std::begin(TAG), std::end(TAG));
    data.insert(data.end(), body.begin(), body.end());
    return CScript() << OP_RETURN << data;
}

/** A transaction with token outputs `before` (each of no value, to HOLDER, unless given) then the marker. */
CTransaction MarkerTx(const Marker& marker, std::vector<CTxOut> before = {}, CAmount value = 0)
{
    before.emplace_back(value, MarkerScript(marker));
    return RawTx(std::move(before));
}

CTransaction OpTx(Operation op, CAmount value = 0)
{
    Marker marker;
    marker.operation = std::move(op);
    return MarkerTx(marker, {}, value);
}

uint256 Filled(uint8_t byte)
{
    uint256 value;
    std::fill(value.begin(), value.end(), byte);
    return value;
}

/** The index of pools by asset: what the rules read to retire an asset. */
sidechain::StoreBytes PoolIndexKey(const AssetId& asset, const uint256& pool)
{
    sidechain::StoreBytes key{0x3a};
    sidechain::KeyCodec<uint256>::Encode(key, asset);
    sidechain::KeyCodec<uint256>::Encode(key, pool);
    return key;
}

// The tables of assets and pools, by their bytes (State's own; a Table of the same byte made here
// would take the byte a second time, which tells a broken build: sidechain::DuplicateTableIds).
sidechain::StoreBytes AssetKey(const AssetId& asset) { return sidechain::TableKey(0x31, asset); }
sidechain::StoreBytes PoolKey(const uint256& pool) { return sidechain::TableKey(0x34, pool); }

/** The message of the JSON-RPC error `fn` throws, or "" if none. */
std::string RpcError(const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const UniValue& error) {
        return error["message"].get_str();
    }
    return "";
}
} // namespace

BOOST_AUTO_TEST_CASE(check_marker_rejects)
{
    // Every reject of the marker alone (CheckMarker, GetMarker, ParseMarkerScript): one transaction
    // per way, each with the exact reason, the same through the state (MakePlan checks it first).
    const CScript unspendable{CScript() << OP_TRUE};
    const CScript burnt{CScript() << OP_RETURN};
    const uint256 nonce{HashName("nonce")};
    const Txid some_auction{Txid::FromUint256(uint256{9})};
    const auto outputs_tx{[](std::vector<MarkerOutput> outs, std::vector<CTxOut> vout) {
        Marker marker;
        marker.outputs = std::move(outs);
        return MarkerTx(marker, std::move(vout));
    }};
    const auto reg{[&](const AssetId& name, uint64_t supply, uint8_t decimals, AssetData data, std::optional<std::string> text) {
        bitassets::Register r;
        r.name = name;
        r.nonce = nonce;
        r.supply = supply;
        r.decimals = decimals;
        r.data = std::move(data);
        r.text = std::move(text);
        return r;
    }};
    const auto update{[](const AssetId& asset, AssetUpdates updates) { return UpdateAsset{asset, std::move(updates)}; }};
    const EncryptionKey bad_encryption_key{};
    const SigningKey bad_signing_key{Filled(0xff)};
    const std::string long_info(MAX_INFO_SIZE + 1, 'x');
    const std::string long_name(MAX_NAME_TEXT_SIZE + 1, 'N');

    // A marker pushed with OP_PUSHDATA1 where a direct push does: not the one way to write it.
    CScript pushdata1;
    {
        const std::vector<unsigned char> data{0x42, 0x41, 0x53, 0x54, 0, 0, 0};
        pushdata1.push_back(OP_RETURN);
        pushdata1.push_back(OP_PUSHDATA1);
        pushdata1.push_back(static_cast<unsigned char>(data.size()));
        pushdata1.insert(pushdata1.end(), data.begin(), data.end());
    }
    std::vector<unsigned char> bad_update{0, UpdateAsset::KIND};
    bad_update.insert(bad_update.end(), 32, 0x01);
    bad_update.push_back(3); // no such update kind
    std::vector<Token> too_many_burns(MAX_BURNS + 1, Asset(GOLD, 1));

    struct Case {
        std::string name;
        CTransaction tx;
        std::string reason;
    };
    const std::vector<Case> cases{
        // Not a marker that parses.
        {"marker version 1", RawTx({CTxOut{0, RawMarker({1, 0, 0})}}), "bad-ba-marker"},
        {"no operation of kind 13", RawTx({CTxOut{0, RawMarker({0, 13, 0})}}), "bad-ba-marker"},
        {"a byte after the marker", RawTx({CTxOut{0, RawMarker({0, 0, 0, 0})}}), "bad-ba-marker"},
        {"an operation cut short", RawTx({CTxOut{0, RawMarker({0, Swap::KIND})}}), "bad-ba-marker"},
        {"more outputs than a marker lists", RawTx({CTxOut{0, RawMarker({0, 0, MAX_MARKER_OUTPUTS + 1})}}), "bad-ba-marker"},
        {"a token of kind 5", RawTx({CTxOut{0, RawMarker({0, 0, 1, 0, 5})}}), "bad-ba-marker"},
        {"an update of kind 3", RawTx({CTxOut{0, RawMarker(bad_update)}}), "bad-ba-marker"},
        {"a count not written in the fewest bytes", RawTx({CTxOut{0, RawMarker({0, 0, 0xfd, 0, 0})}}), "bad-ba-marker"},
        {"pushed with OP_PUSHDATA1", RawTx({CTxOut{0, pushdata1}}), "bad-ba-marker"},
        {"an opcode after the push", RawTx({CTxOut{0, RawMarker({0, 0, 0}) << OP_1}}), "bad-ba-marker"},
        {"two markers", RawTx({CTxOut{0, MarkerScript(Marker{})}, CTxOut{0, MarkerScript(Marker{})}}), "bad-ba-markers"},
        // A marker of a coinbase, or of a value it cannot have.
        {"a marker in a coinbase", RawTx({CTxOut{0, MarkerScript(Marker{})}}, /*coinbase=*/true), "bad-ba-coinbase"},
        {"a negative marker value", MarkerTx(Marker{}, {}, -1), "bad-ba-marker-value"},
        {"a marker value above MAX_MONEY", OpTx(Swap{CHN, 1, GOLD, 0, {}}, MAX_MONEY + 1), "bad-ba-marker-value"},
        {"CHN burned without an operation", MarkerTx(Marker{}, {}, 1), "bad-ba-marker-value"},
        // The outputs it lists.
        {"an output past the end", outputs_tx({{5, Asset(GOLD, 1)}}, {}), "bad-ba-outputs"},
        {"the marker as an output", outputs_tx({{0, Asset(GOLD, 1)}}, {}), "bad-ba-outputs"},
        {"outputs out of order", outputs_tx({{1, Asset(GOLD, 1)}, {0, Asset(GOLD, 1)}}, {CTxOut{0, HOLDER}, CTxOut{0, HOLDER}}), "bad-ba-outputs"},
        {"an output twice", outputs_tx({{0, Asset(GOLD, 1)}, {0, Asset(GOLD, 1)}}, {CTxOut{0, HOLDER}}), "bad-ba-outputs"},
        {"a token output of value", outputs_tx({{0, Asset(GOLD, 1)}}, {CTxOut{1, HOLDER}}), "bad-ba-token-output"},
        {"a token output nobody can spend", outputs_tx({{0, Asset(GOLD, 1)}}, {CTxOut{0, unspendable}}), "bad-ba-token-output"},
        {"a result output nobody can spend", outputs_tx({{0, std::nullopt}}, {CTxOut{0, burnt}}), "bad-ba-token-output"},
        {"a token of no units", outputs_tx({{0, Asset(GOLD, 0)}}, {CTxOut{0, HOLDER}}), "bad-ba-amount"},
        {"a token above MAX_AMOUNT", outputs_tx({{0, Token{Token::Kind::LP, GOLD, MAX_AMOUNT + 1}}}, {CTxOut{0, HOLDER}}), "bad-ba-amount"},
        {"coins of no asset", outputs_tx({{0, Asset(uint256{}, 1)}}, {CTxOut{0, HOLDER}}), "bad-ba-token-id"},
        {"a control coin of no asset", outputs_tx({{0, Unit(Token::Kind::CONTROL, uint256{})}}, {CTxOut{0, HOLDER}}), "bad-ba-token-id"},
        {"shares of no pool", outputs_tx({{0, Token{Token::Kind::LP, uint256{}, 5}}}, {CTxOut{0, HOLDER}}), "bad-ba-token-id"},
        // Where CHN is paid.
        {"swap: CHN to a script nobody spends", OpTx(Swap{CHN, 1, GOLD, 0, burnt}), "bad-ba-chn-to"},
        {"remove: CHN to a script nobody spends", OpTx(RemoveLiquidity{GOLD, CHN, 1, 0, 0, burnt}), "bad-ba-chn-to"},
        {"bid: CHN to a script nobody spends", OpTx(Bid{some_auction, 1, 0, burnt}), "bad-ba-chn-to"},
        {"collect: CHN to a script nobody spends", OpTx(Collect{some_auction, unspendable}), "bad-ba-chn-to"},
        // Registrations.
        {"a supply above MAX_AMOUNT", OpTx(reg(GOLD, MAX_AMOUNT + 1, 0, {}, "GOLD")), "bad-ba-supply"},
        {"13 decimals", OpTx(reg(GOLD, 1, MAX_DECIMALS + 1, {}, "GOLD")), "bad-ba-supply"},
        {"an info too long", OpTx(reg(GOLD, 1, 0, AssetData{.info = long_info}, "GOLD")), "bad-ba-data"},
        {"an encryption key not on the curve", OpTx(reg(GOLD, 1, 0, AssetData{.encryption_key = bad_encryption_key}, "GOLD")), "bad-ba-data"},
        {"a signing key not on the curve", OpTx(reg(GOLD, 1, 0, AssetData{.signing_key = bad_signing_key}, "GOLD")), "bad-ba-data"},
        {"a name with a space at its start", OpTx(reg(HashName(" GOLD"), 1, 0, {}, " GOLD")), "bad-ba-name-text"},
        {"a name too long", OpTx(reg(HashName(long_name), 1, 0, {}, long_name)), "bad-ba-name-text"},
        {"a name not of its hash", OpTx(reg(SILVER, 1, 0, {}, "GOLD")), "bad-ba-name-text"},
        {"no name", OpTx(reg(uint256{}, 1, 0, {}, std::nullopt)), "bad-ba-name"},
        // Mints and updates.
        {"a mint of nothing", OpTx(Mint{GOLD, 0}), "bad-ba-amount"},
        {"a mint above MAX_AMOUNT", OpTx(Mint{GOLD, MAX_AMOUNT + 1}), "bad-ba-amount"},
        {"a mint of no asset", OpTx(Mint{uint256{}, 5}), "bad-ba-amount"},
        {"an update of no asset", OpTx(update(uint256{}, AssetUpdates{.info = {UpdateKind::SET, "x"}})), "bad-ba-data"},
        {"an update of nothing", OpTx(update(GOLD, AssetUpdates{})), "bad-ba-data"},
        {"an update to an info too long", OpTx(update(GOLD, AssetUpdates{.info = {UpdateKind::SET, long_info}})), "bad-ba-data"},
        {"an update to a bad encryption key", OpTx(update(GOLD, AssetUpdates{.encryption_key = {UpdateKind::SET, bad_encryption_key}})), "bad-ba-data"},
        {"an update to a bad signing key", OpTx(update(GOLD, AssetUpdates{.signing_key = {UpdateKind::SET, bad_signing_key}})), "bad-ba-data"},
        // Burns.
        {"a burn of nothing", OpTx(Burn{}), "bad-ba-burn"},
        {"a burn of 17 tokens", OpTx(Burn{too_many_burns}), "bad-ba-burn"},
        {"a burn of shares", OpTx(Burn{{Token{Token::Kind::LP, GOLD, 1}}}), "bad-ba-burn"},
        {"a burn of a receipt", OpTx(Burn{{Unit(Token::Kind::RECEIPT, GOLD)}}), "bad-ba-burn"},
        {"a burn of no asset", OpTx(Burn{{Asset(uint256{}, 1)}}), "bad-ba-burn"},
        {"a burn of no units", OpTx(Burn{{Asset(GOLD, 0)}}), "bad-ba-burn"},
        {"a burn above MAX_AMOUNT", OpTx(Burn{{Asset(GOLD, MAX_AMOUNT + 1)}}), "bad-ba-burn"},
        {"a burn of 2 control coins of one", OpTx(Burn{{Token{Token::Kind::CONTROL, GOLD, 2}}}), "bad-ba-burn"},
        {"a burn of 2 of one reservation", OpTx(Burn{{Token{Token::Kind::RESERVATION, GOLD, 2}}}), "bad-ba-burn"},
        // Pools.
        {"a swap of an asset for itself", OpTx(Swap{GOLD, 1, GOLD, 0, {}}), "bad-ba-swap"},
        {"a swap of nothing", OpTx(Swap{GOLD, 0, CHN, 0, HOLDER}), "bad-ba-swap"},
        {"a swap above MAX_AMOUNT", OpTx(Swap{GOLD, MAX_AMOUNT + 1, CHN, 0, HOLDER}), "bad-ba-swap"},
        {"a swap asking above MAX_AMOUNT", OpTx(Swap{GOLD, 1, CHN, MAX_AMOUNT + 1, HOLDER}), "bad-ba-swap"},
        {"a pool of an asset and itself", OpTx(AddLiquidity{GOLD, GOLD, 1, 1, 0}), "bad-ba-liquidity"},
        {"a deposit of none of the first", OpTx(AddLiquidity{GOLD, CHN, 0, 1, 0}), "bad-ba-liquidity"},
        {"a deposit of none of the second", OpTx(AddLiquidity{GOLD, CHN, 1, 0, 0}), "bad-ba-liquidity"},
        {"a deposit above MAX_AMOUNT", OpTx(AddLiquidity{GOLD, CHN, MAX_AMOUNT + 1, 1, 0}), "bad-ba-liquidity"},
        {"a deposit asking shares above MAX_AMOUNT", OpTx(AddLiquidity{GOLD, CHN, 1, 1, MAX_AMOUNT + 1}), "bad-ba-liquidity"},
        {"a withdrawal from a pool of an asset and itself", OpTx(RemoveLiquidity{GOLD, GOLD, 1, 0, 0, {}}), "bad-ba-liquidity"},
        {"a withdrawal of no shares", OpTx(RemoveLiquidity{GOLD, CHN, 0, 0, 0, {}}), "bad-ba-liquidity"},
        {"a withdrawal asking above MAX_AMOUNT of the first", OpTx(RemoveLiquidity{GOLD, CHN, 1, MAX_AMOUNT + 1, 0, {}}), "bad-ba-liquidity"},
        {"a withdrawal asking above MAX_AMOUNT of the second", OpTx(RemoveLiquidity{GOLD, CHN, 1, 0, MAX_AMOUNT + 1, {}}), "bad-ba-liquidity"},
        // Auctions.
        {"an auction of an asset for itself", OpTx(CreateAuction{GOLD, 1, GOLD, 10, 10, 1, 10}), "bad-ba-auction"},
        {"an auction of nothing", OpTx(CreateAuction{GOLD, 0, CHN, 10, 10, 1, 10}), "bad-ba-auction"},
        {"an auction ending at no price", OpTx(CreateAuction{GOLD, 1, CHN, 10, 0, 1, 10}), "bad-ba-auction-price"},
        {"an auction whose price rises", OpTx(CreateAuction{GOLD, 1, CHN, 10, 11, 1, 10}), "bad-ba-auction-price"},
        {"an auction starting above MAX_AMOUNT", OpTx(CreateAuction{GOLD, 1, CHN, MAX_AMOUNT + 1, 10, 1, 10}), "bad-ba-auction-price"},
        {"an auction of no blocks", OpTx(CreateAuction{GOLD, 1, CHN, 10, 10, 1, 0}), "bad-ba-auction-duration"},
        {"an auction too long", OpTx(CreateAuction{GOLD, 1, CHN, 10, 10, 1, MAX_AUCTION_DURATION + 1}), "bad-ba-auction-duration"},
        {"an auction starting below height 0", OpTx(CreateAuction{GOLD, 1, CHN, 10, 10, -1, 10}), "bad-ba-auction-duration"},
        {"a bid of nothing", OpTx(Bid{some_auction, 0, 0, {}}), "bad-ba-bid"},
        {"a bid above MAX_AMOUNT", OpTx(Bid{some_auction, MAX_AMOUNT + 1, 0, {}}), "bad-ba-bid"},
        {"a bid asking above MAX_AMOUNT", OpTx(Bid{some_auction, 1, MAX_AMOUNT + 1, {}}), "bad-ba-bid"},
    };
    const TState state;
    std::set<std::string> reasons;
    for (const Case& c : cases) {
        std::string reason;
        BOOST_CHECK_MESSAGE(!CheckMarker(c.tx, reason), c.name);
        BOOST_CHECK_MESSAGE(reason == c.reason, c.name << ": " << reason << ", not " << c.reason);
        std::string through_state;
        BOOST_CHECK_MESSAGE(!state.CheckTx(c.tx, 100, through_state), c.name);
        BOOST_CHECK_MESSAGE(through_state == c.reason, c.name << " (state): " << through_state);
        reasons.insert(c.reason);
    }
    // Each reason CheckMarker gives. Not here: the amount of a control coin, reservation or receipt a
    // marker lists other than 1 (bad-ba-amount): the marker does not write one for those kinds, and
    // reading one sets it to 1, so no marker can carry another.
    BOOST_CHECK_EQUAL(reasons.size(), 20u);

    // And what each of those transactions was close to is well formed.
    const std::vector<CTransaction> good{
        outputs_tx({{0, Asset(GOLD, 1)}, {1, std::nullopt}}, {CTxOut{0, HOLDER}, CTxOut{0, HOLDER}}),
        outputs_tx({{0, Unit(Token::Kind::RESERVATION, uint256{})}, {1, Unit(Token::Kind::RECEIPT, uint256{})}}, {CTxOut{0, HOLDER}, CTxOut{0, HOLDER}}),
        OpTx(Swap{CHN, 1, GOLD, 0, HOLDER}, 1),
        OpTx(reg(GOLD, MAX_AMOUNT, MAX_DECIMALS, AssetData{.info = std::string(MAX_INFO_SIZE, 'x')}, "GOLD")),
        OpTx(Burn{std::vector<Token>(MAX_BURNS, Asset(GOLD, 1))}),
        OpTx(CreateAuction{GOLD, 1, CHN, 10, 10, 0, MAX_AUCTION_DURATION}),
        OpTx(Collect{some_auction, {}}),
    };
    for (const CTransaction& tx : good) {
        std::string reason;
        BOOST_CHECK_MESSAGE(CheckMarker(tx, reason), reason);
    }
    // A transaction without a marker has nothing to check.
    std::string reason;
    BOOST_CHECK(CheckMarker(RawTx({CTxOut{1, HOLDER}}), reason));
    BOOST_CHECK(reason.empty());
}

BOOST_AUTO_TEST_CASE(plan_rejects)
{
    // The rejects of the rules against the state that no other test reaches, one transaction each.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    const AssetId unknown{HashName("UNKNOWN")};

    // Assets nobody registered.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Mint{unknown, 5}, {Asset(unknown, 5)})), "bad-ba-asset-unknown");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, UpdateAsset{unknown, AssetUpdates{.info = {UpdateKind::SET, "x"}}}, {})), "bad-ba-asset-unknown");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Burn{{Asset(unknown, 5)}}, {})), "bad-ba-asset-unknown");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, AddLiquidity{GOLD, unknown, 100'000, 100'000, 1}, {Asset(GOLD, 900'000), std::nullopt, std::nullopt})), "bad-ba-asset-unknown");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, CreateAuction{unknown, 1, CHN, 10, 10, f.height + 1, 10}, {Unit(Token::Kind::RECEIPT, uint256{})})), "bad-ba-asset-unknown");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, CreateAuction{GOLD, 1, unknown, 10, 10, f.height + 1, 10}, {Asset(GOLD, 999'999), Unit(Token::Kind::RECEIPT, uint256{})})), "bad-ba-asset-unknown");

    // Burns of more than there is, or of a reservation there is not.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Burn{{Unit(Token::Kind::RESERVATION, uint256{7})}}, {})), "bad-ba-burn");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, Burn{{Asset(GOLD, 1'000'001)}}, {})), "bad-ba-burn");
    // Two burns of one asset in a transaction count together.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, Burn{{Asset(GOLD, 600'000), Asset(GOLD, 600'000)}}, {})), "bad-ba-burn");

    // A supply beyond MAX_AMOUNT.
    const auto [big_control, big_coins]{f.Issue("BIG", MAX_AMOUNT)};
    const AssetId big{HashName("BIG")};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({big_control}, Mint{big, 1}, {Unit(Token::Kind::CONTROL, big), Asset(big, 1)})), "bad-ba-supply");

    // A pool of GOLD and CHN.
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt, std::nullopt}, 50'000'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    const uint256 id{PoolId(GOLD, CHN)};
    const COutPoint gold{added.GetHash(), 0};
    // What would go in takes a reserve beyond MAX_AMOUNT: a swap, a deposit.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, Swap{GOLD, MAX_AMOUNT, CHN, 1, HOLDER}, {})), "bad-ba-swap");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, AddLiquidity{GOLD, CHN, MAX_AMOUNT, 1'000, 1}, {std::nullopt, std::nullopt}, 1'000)), "bad-ba-liquidity");
    // More shares out than are held: the MIN_LIQUIDITY nobody holds stay.
    const uint64_t shares{f.state.Pools().at(id).shares};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{added.GetHash(), 1}}, RemoveLiquidity{GOLD, CHN, shares - MIN_LIQUIDITY + 1, 0, 0, HOLDER}, {std::nullopt})), "bad-ba-liquidity");
    // No pool to take shares out of; no auction to bid on or collect.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, RemoveLiquidity{GOLD, big, 1, 0, 0, {}}, {std::nullopt})), "bad-ba-no-pool");
    const Txid nothing{Txid::FromUint256(uint256{0x42})};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{nothing, 10, 1, HOLDER}, {}, 10)), "bad-ba-no-auction");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Collect{nothing, HOLDER}, {})), "bad-ba-no-auction");
    // Results the outputs do not match: GOLD bought with no output for it; CHN sold with one.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Swap{CHN, 1'000'000, GOLD, 1, {}}, {}, 1'000'000)), "bad-ba-results");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, Swap{GOLD, 1'000, CHN, 1, HOLDER}, {Asset(GOLD, 899'000), std::nullopt})), "bad-ba-results");

    // An auction selling CHN: a bid that buys CHN dust.
    Fixture g;
    const auto [g_control, g_gold]{g.Issue("GOLD", 1'000'000)};
    const CTransaction chn_auction{MakeTx({}, CreateAuction{CHN, 1'000'000, GOLD, 1'000, 1'000, g.height, 10}, {Unit(Token::Kind::RECEIPT, uint256{})}, 1'000'000)};
    BOOST_REQUIRE_MESSAGE(g.Apply(chn_auction, &reason), reason);
    // 10 GOLD buys 10 000 satoshis: under MIN_CHN_PAYOUT; 50 buy as much as that.
    BOOST_CHECK_EQUAL(g.Reject(MakeTx({g_gold}, Bid{chn_auction.GetHash(), 10, 1, HOLDER}, {Asset(GOLD, 999'990)})), "bad-ba-chn-dust");
    BOOST_CHECK(g.Apply(MakeTx({g_gold}, Bid{chn_auction.GetHash(), 50, 1, HOLDER}, {Asset(GOLD, 999'950)}), &reason));

    // Proceeds beyond MAX_AMOUNT: two bids of 2^62 each, at the price of MAX_AMOUNT for 2.
    Fixture h;
    const auto [h_gold_control, h_gold]{h.Issue("GOLD", 10)};
    const auto [h_silver_control, h_silver]{h.Issue("SILVER", MAX_AMOUNT)};
    const CTransaction dear{MakeTx({h_gold}, CreateAuction{GOLD, 2, SILVER, MAX_AMOUNT, MAX_AMOUNT, h.height, 10}, {Asset(GOLD, 8), Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE_MESSAGE(h.Apply(dear, &reason), reason);
    const uint64_t half{uint64_t{1} << 62};
    const CTransaction first_bid{MakeTx({h_silver}, Bid{dear.GetHash(), half, 1, {}}, {Asset(SILVER, MAX_AMOUNT - half), std::nullopt})};
    BOOST_REQUIRE_MESSAGE(h.Apply(first_bid, &reason), reason);
    BOOST_CHECK_EQUAL(h.state.Auctions().at(dear.GetHash()).remaining, 1u);
    BOOST_CHECK_EQUAL(h.Reject(MakeTx({COutPoint{first_bid.GetHash(), 0}}, Bid{dear.GetHash(), half, 1, {}}, {std::nullopt})), "bad-ba-bid");

    // CHN paid out beyond MAX_MONEY: a pool that holds more CHN than there can be (the state alone does
    // not know how much CHN there is: the chain does, MAX_MONEY in all).
    Fixture k;
    const uint64_t lots{1'000'000'000'000'000'000};
    const auto [k_control, k_coins]{k.Issue("GOLD", lots)};
    const uint64_t gold_in{1'000'000'000'000'000}, chn_in{2'000'000'000'000'000};
    const CTransaction k_open{MakeTx({k_coins}, AddLiquidity{GOLD, CHN, gold_in, chn_in, 1}, {Asset(GOLD, lots - gold_in), std::nullopt, std::nullopt}, chn_in)};
    BOOST_REQUIRE_MESSAGE(k.Apply(k_open, &reason), reason);
    const CTransaction k_more{MakeTx({COutPoint{k_open.GetHash(), 0}}, AddLiquidity{GOLD, CHN, gold_in, chn_in, 1}, {Asset(GOLD, lots - 2 * gold_in), std::nullopt, std::nullopt}, chn_in)};
    BOOST_REQUIRE_MESSAGE(k.Apply(k_more, &reason), reason);
    BOOST_CHECK_EQUAL(k.state.Pools().at(id).reserve0, 2 * chn_in);
    const uint64_t k_swap_in{100'000'000'000'000'000};
    BOOST_REQUIRE(amm::SwapOut(2 * gold_in, 2 * chn_in, k_swap_in) > static_cast<uint64_t>(MAX_MONEY));
    BOOST_CHECK_EQUAL(k.Reject(MakeTx({COutPoint{k_more.GetHash(), 0}}, Swap{GOLD, k_swap_in, CHN, 1, HOLDER}, {Asset(GOLD, lots - 2 * gold_in - k_swap_in)})), "bad-ba-chn-amount");
}

BOOST_AUTO_TEST_CASE(defensive_rejects)
{
    // Rejects no transaction reaches from a state the rules made: they keep a state that could
    // only be forged (here, written as it is) from going wrong. Each is shown on such a state.
    // Written by the bytes of the tables, not by tables of their own: no byte is taken twice.
    BOOST_CHECK(sidechain::DuplicateTableIds().empty());
    std::string reason;

    // Burned beyond MAX_AMOUNT: what is burned is at most what was minted, less the supply left.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000)};
    AssetRecord record{f.state.Assets().at(GOLD)};
    record.burned = MAX_AMOUNT - 1;
    f.state.Forge(AssetKey(GOLD), sidechain::EncodeValue(record));
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, Burn{{Asset(GOLD, 5)}}, {Asset(GOLD, 995)})), "bad-ba-burn");

    // Shares beyond MAX_AMOUNT: a pool's shares are at most the root of the product of its reserves.
    Fixture g;
    const auto [gc, g_gold]{g.Issue("GOLD", 1'000)};
    const auto [sc, g_silver]{g.Issue("SILVER", 1'000)};
    Pool pool;
    pool.asset0 = std::min(GOLD, SILVER);
    pool.asset1 = std::max(GOLD, SILVER);
    pool.reserve0 = pool.reserve1 = 1'000;
    pool.shares = MAX_AMOUNT - 10;
    g.state.Forge(PoolKey(PoolId(GOLD, SILVER)), sidechain::EncodeValue(pool));
    BOOST_CHECK_EQUAL(g.Reject(MakeTx({g_gold, g_silver}, AddLiquidity{GOLD, SILVER, 1'000, 1'000, 1}, {std::nullopt, std::nullopt})), "bad-ba-liquidity");

    // CHN freed beyond MAX_MONEY by retiring an asset: its pools hold at most all the CHN there is.
    Fixture h;
    const auto [h_control, h_coins]{h.Issue("GOLD", 1'000)};
    BOOST_REQUIRE(h.Apply(MakeTx({h_control, h_coins}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 1'000)}}, {})));
    BOOST_REQUIRE(h.state.Releasable(GOLD));
    Pool dead;
    dead.asset0 = CHN;
    dead.asset1 = GOLD;
    dead.reserve0 = static_cast<uint64_t>(MAX_MONEY) + 1;
    dead.reserve1 = 1;
    dead.shares = MIN_LIQUIDITY;
    const uint256 dead_id{PoolId(CHN, GOLD)};
    h.state.Forge(PoolKey(dead_id), sidechain::EncodeValue(dead));
    h.state.Forge(PoolIndexKey(CHN, dead_id), {});
    h.state.Forge(PoolIndexKey(GOLD, dead_id), {});
    BOOST_REQUIRE(h.state.Releasable(GOLD));
    BOOST_CHECK_EQUAL(h.Reject(MakeTx({}, ReleaseAsset{GOLD}, {})), "bad-ba-release");
    // At MAX_MONEY, it is retired.
    dead.reserve0 = static_cast<uint64_t>(MAX_MONEY);
    h.state.Forge(PoolKey(dead_id), sidechain::EncodeValue(dead));
    CAmount released{0};
    BOOST_CHECK_MESSAGE(h.state.ApplyTx(MakeTx({}, ReleaseAsset{GOLD}, {}), h.height, h.undo, h.payouts, reason, &released), reason);
    BOOST_CHECK_EQUAL(released, MAX_MONEY);

    // Not reached even so: an AddLiquidity where both sides have something back (amm::Provide takes
    // one side whole), bad-ba-liquidity at the check of what goes back.
}

BOOST_AUTO_TEST_CASE(update_every_field)
{
    // Every field of an asset's data set, then deleted: its history, and the data at each height.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 10)};
    const CKey key{GenerateRandomKey()};
    const CPubKey pubkey{key.GetPubKey()};
    EncryptionKey encryption_key;
    std::copy(pubkey.begin(), pubkey.end(), encryption_key.begin());
    const XOnlyPubKey xonly{pubkey};
    const SigningKey signing_key{std::span<const unsigned char>{xonly.data(), xonly.size()}};
    const SocketV4 v4{{203, 0, 113, 7}, 8333};
    const SocketV6 v6{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7}, 0};
    const uint256 commitment{HashName("terms")};

    AssetUpdates set;
    set.commitment = {UpdateKind::SET, commitment};
    set.ipv4 = {UpdateKind::SET, v4};
    set.ipv6 = {UpdateKind::SET, v6};
    set.encryption_key = {UpdateKind::SET, encryption_key};
    set.signing_key = {UpdateKind::SET, signing_key};
    set.info = {UpdateKind::SET, "All of it"};
    // The marker carries every field as it was.
    Marker marker;
    marker.operation = UpdateAsset{GOLD, set};
    marker.outputs = {{0, Unit(Token::Kind::CONTROL, GOLD)}};
    const auto parsed{ParseMarkerScript(MarkerScript(marker))};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(*parsed == marker);

    f.height = 101;
    const CTransaction updated{MakeTx({control}, UpdateAsset{GOLD, set}, {Unit(Token::Kind::CONTROL, GOLD)})};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(updated, &reason), reason);
    const AssetData all{f.state.Assets().at(GOLD).Current()};
    BOOST_CHECK(all.commitment == commitment);
    BOOST_CHECK(all.ipv4 == v4);
    BOOST_CHECK(all.ipv6 == v6);
    BOOST_CHECK(all.encryption_key == encryption_key);
    BOOST_CHECK(all.signing_key == signing_key);
    BOOST_CHECK(all.info == "All of it");
    // Each as it is stored, the same back.
    BOOST_CHECK(sidechain::DecodeValue<AssetData>(sidechain::EncodeValue(all)) == all);
    BOOST_CHECK(sidechain::DecodeValue<SocketV4>(sidechain::EncodeValue(v4)) == v4);
    BOOST_CHECK(sidechain::DecodeValue<SocketV6>(sidechain::EncodeValue(v6)) == v6);

    // Deleted, all but the info, which is kept.
    AssetUpdates deleted;
    deleted.commitment = {UpdateKind::DELETE, std::nullopt};
    deleted.ipv4 = {UpdateKind::DELETE, std::nullopt};
    deleted.ipv6 = {UpdateKind::DELETE, std::nullopt};
    deleted.encryption_key = {UpdateKind::DELETE, std::nullopt};
    deleted.signing_key = {UpdateKind::DELETE, std::nullopt};
    f.height = 102;
    const CTransaction removed{MakeTx({COutPoint{updated.GetHash(), 0}}, UpdateAsset{GOLD, deleted}, {Unit(Token::Kind::CONTROL, GOLD)})};
    BOOST_REQUIRE_MESSAGE(f.Apply(removed, &reason), reason);
    const AssetData left{f.state.Assets().at(GOLD).Current()};
    BOOST_CHECK(left == (AssetData{.info = "All of it"}));
    const AssetRecord record{f.state.Assets().at(GOLD)};
    for (const DataField field : {DataField::COMMITMENT, DataField::IPV4, DataField::IPV6, DataField::ENCRYPTION_KEY, DataField::SIGNING_KEY}) {
        BOOST_CHECK_EQUAL(record.changes[static_cast<size_t>(field)], 3u);
    }
    BOOST_CHECK_EQUAL(record.changes[static_cast<size_t>(DataField::INFO)], 2u);

    const AssetHistory history{f.state.View().GetHistory(GOLD)};
    BOOST_REQUIRE_EQUAL(history.ipv4.size(), 3u);
    BOOST_CHECK(!history.ipv4[0].value);
    BOOST_CHECK(history.ipv4[1] == (Stamped<SocketV4>{v4, updated.GetHash(), 101}));
    BOOST_CHECK(history.ipv4[2] == (Stamped<SocketV4>{std::nullopt, removed.GetHash(), 102}));
    BOOST_REQUIRE_EQUAL(history.ipv6.size(), 3u);
    BOOST_CHECK(history.ipv6[1].value == v6);
    BOOST_REQUIRE_EQUAL(history.encryption_key.size(), 3u);
    BOOST_CHECK(history.encryption_key[1].value == encryption_key);
    BOOST_REQUIRE_EQUAL(history.signing_key.size(), 3u);
    BOOST_CHECK(history.signing_key[1].value == signing_key);
    BOOST_REQUIRE_EQUAL(history.commitment.size(), 3u);
    BOOST_CHECK(history.commitment[1].value == commitment);
    BOOST_REQUIRE_EQUAL(history.info.size(), 2u);
    // At each height.
    BOOST_CHECK(f.state.View().DataAt(GOLD, 100) == AssetData{});
    BOOST_CHECK(f.state.View().DataAt(GOLD, 101) == all);
    BOOST_CHECK(f.state.View().DataAt(GOLD, 102) == left);

    // Undone, block by block.
    TUndo second;
    second.parts.push_back(f.undo.parts.back());
    f.state.Revert(second);
    BOOST_CHECK(f.state.Assets().at(GOLD).Current() == all);
    BOOST_CHECK_EQUAL(f.state.View().GetHistory(GOLD).ipv4.size(), 2u);
}

BOOST_AUTO_TEST_CASE(auction_undo)
{
    // Undoing the block that made an auction, and those that bid on it and collected it.
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 100)};
    const TState before{f.state};
    const AssetRecord gold_before{f.state.Assets().at(GOLD)};
    const auto block{[&](const CTransaction& tx) {
        f.undo = TUndo{};
        std::string reason;
        BOOST_REQUIRE_MESSAGE(f.Apply(tx, &reason), reason);
        return f.undo;
    }};
    const CTransaction made{MakeTx({coins}, CreateAuction{GOLD, 40, CHN, 400'000, 400'000, f.height, 10}, {Asset(GOLD, 60), Unit(Token::Kind::RECEIPT, uint256{})})};
    const TUndo undo_made{block(made)};
    const Txid id{made.GetHash()};
    const TState after_made{f.state};
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).holding_auctions, 1u);
    const CTransaction bid{MakeTx({}, Bid{id, 100'000, 1, {}}, {std::nullopt}, 100'000)};
    const TUndo undo_bid{block(bid)};
    const TState after_bid{f.state};
    f.height += 10;
    const CTransaction collect{MakeTx({COutPoint{id, 1}}, Collect{id, HOLDER}, {std::nullopt})};
    const TUndo undo_collect{block(collect)};
    BOOST_CHECK(f.state.Auctions().at(id).closed);
    BOOST_CHECK_EQUAL(f.state.Assets().at(GOLD).holding_auctions, 0u);

    f.state.Revert(undo_collect);
    BOOST_CHECK(f.state == after_bid);
    BOOST_CHECK(!f.state.Auctions().at(id).closed);
    f.state.Revert(undo_bid);
    BOOST_CHECK(f.state == after_made);
    BOOST_CHECK_EQUAL(f.state.Auctions().at(id).remaining, 40u);
    f.state.Revert(undo_made);
    BOOST_CHECK(f.state == before);
    BOOST_CHECK(f.state.Auctions().empty());
    BOOST_CHECK(!f.state.Tokens().contains(COutPoint{id, 1}));
    BOOST_CHECK(f.state.Tokens().at(coins) == Asset(GOLD, 100));
    BOOST_CHECK(f.state.Assets().at(GOLD) == gold_before);
    // The index of auctions by asset is empty again.
    sidechain::StoreBytes by_gold{0x3b};
    by_gold.insert(by_gold.end(), GOLD.begin(), GOLD.end());
    BOOST_CHECK_EQUAL(f.state.Count(by_gold), 0u);
}

BOOST_AUTO_TEST_CASE(release_burns_paired_asset)
{
    // Retiring an asset whose pool is with another asset, not CHN: what the pool holds of the other
    // asset is burned, its supply down and its burned up by as much; undone, all of it is back.
    Fixture f;
    const auto [gc, gold]{f.Issue("GOLD", 10'000'000)};
    const auto [sc, silver]{f.Issue("SILVER", 2'000'000)};
    const CTransaction added{MakeTx({gold, silver}, AddLiquidity{GOLD, SILVER, 1'000'000, 1'000'000, 1}, {Asset(GOLD, 9'000'000), Asset(SILVER, 1'000'000), std::nullopt, std::nullopt})};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(added, &reason), reason);
    const uint256 id{PoolId(GOLD, SILVER)};
    const COutPoint lp{added.GetHash(), 2};
    const uint64_t held{f.state.Tokens().at(lp).amount};
    const auto [w0, w1]{amm::Withdraw(f.state.Pools().at(id), held)};
    const bool gold_first{f.state.Pools().at(id).asset0 == GOLD};
    const uint64_t gold_out{gold_first ? w0 : w1}, silver_out{gold_first ? w1 : w0};
    const CTransaction removed{MakeTx({lp}, RemoveLiquidity{GOLD, SILVER, held, gold_out, silver_out, {}}, {std::nullopt, std::nullopt})};
    BOOST_REQUIRE_MESSAGE(f.Apply(removed, &reason), reason);
    BOOST_REQUIRE(amm::Abandoned(f.state.Pools().at(id)));
    // GOLD dies: its control coin and every coin outside the pool burned.
    BOOST_REQUIRE(f.Apply(MakeTx({gc, COutPoint{added.GetHash(), 0}, COutPoint{removed.GetHash(), 0}}, Burn{{Unit(Token::Kind::CONTROL, GOLD), Asset(GOLD, 9'000'000 + gold_out)}}, {}), &reason));
    BOOST_REQUIRE(f.state.Releasable(GOLD));
    // SILVER is alive: someone holds it, and its control coin exists.
    BOOST_CHECK(!f.state.Releasable(SILVER));

    const Pool dust{f.state.Pools().at(id)};
    const uint64_t silver_dust{gold_first ? dust.reserve1 : dust.reserve0};
    BOOST_REQUIRE(silver_dust > 0);
    const AssetRecord silver_before{f.state.Assets().at(SILVER)};
    const TState before{f.state};
    f.undo = TUndo{};
    CAmount released{-1};
    BOOST_REQUIRE_MESSAGE(f.state.ApplyTx(MakeTx({}, ReleaseAsset{GOLD}, {}), f.height, f.undo, f.payouts, reason, &released), reason);
    // No CHN in its pool: nothing for mainchain miners.
    BOOST_CHECK_EQUAL(released, 0);
    BOOST_CHECK(!f.state.Assets().contains(GOLD));
    BOOST_CHECK(!f.state.Pools().contains(id));
    const AssetRecord silver_after{f.state.Assets().at(SILVER)};
    BOOST_CHECK_EQUAL(silver_after.supply, silver_before.supply - silver_dust);
    BOOST_CHECK_EQUAL(silver_after.burned, silver_before.burned + silver_dust);
    BOOST_CHECK_EQUAL(silver_after.minted, silver_before.minted);
    BOOST_CHECK(f.state.View().PoolsOf(SILVER).empty());

    f.state.Revert(f.undo);
    BOOST_CHECK(f.state == before);
    BOOST_CHECK(f.state.Assets().at(SILVER) == silver_before);
    BOOST_CHECK_EQUAL(f.state.View().PoolsOf(SILVER).size(), 1u);
}

BOOST_AUTO_TEST_CASE(rpc_data_fields)
{
    // The data of an asset as the commands read and write it, every field.
    const CKey key{GenerateRandomKey()};
    const CPubKey pubkey{key.GetPubKey()};
    const std::string encryption_hex{HexStr(pubkey)};
    const XOnlyPubKey xonly{pubkey};
    const std::string signing_hex{HexStr(std::span<const unsigned char>{xonly.data(), xonly.size()})};
    const std::string commitment_hex{HashName("terms").GetHex()};
    UniValue data;
    BOOST_REQUIRE(data.read(strprintf(R"({"info": "Gold", "commitment": "%s", "ipv4": "203.0.113.7:8333", "ipv6": "[2001:db8::7]:8333", "encryptionkey": "%s", "signingkey": "%s"})",
                                      commitment_hex, encryption_hex, signing_hex)));
    const AssetData parsed{ParseData(data)};
    BOOST_CHECK(parsed.info == "Gold");
    BOOST_CHECK(parsed.commitment == HashName("terms"));
    BOOST_CHECK(parsed.ipv4 == (SocketV4{{203, 0, 113, 7}, 8333}));
    BOOST_CHECK(parsed.ipv6 == (SocketV6{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7}, 8333}));
    BOOST_CHECK(parsed.encryption_key.has_value() && HexStr(*parsed.encryption_key) == encryption_hex);
    BOOST_CHECK(parsed.signing_key.has_value() && HexStr(*parsed.signing_key) == signing_hex);
    // Written back as read.
    const UniValue written{DataToJSON(parsed)};
    BOOST_CHECK_EQUAL(written.write(), data.write());
    // Without ports.
    UniValue no_ports;
    BOOST_REQUIRE(no_ports.read(R"({"ipv4": "203.0.113.7", "ipv6": "2001:db8::7"})"));
    const UniValue plain{DataToJSON(ParseData(no_ports))};
    BOOST_CHECK_EQUAL(plain["ipv4"].get_str(), "203.0.113.7");
    BOOST_CHECK_EQUAL(plain["ipv6"].get_str(), "2001:db8::7");

    // Updates: a field left out is kept, null deletes it.
    UniValue changes;
    BOOST_REQUIRE(changes.read(R"({"info": null, "ipv4": "198.51.100.1"})"));
    const AssetUpdates updates{ParseUpdates(changes)};
    BOOST_CHECK(updates.info.kind == UpdateKind::DELETE);
    BOOST_CHECK(updates.ipv4.kind == UpdateKind::SET);
    BOOST_CHECK(updates.commitment.kind == UpdateKind::RETAIN);
    BOOST_CHECK(!updates.Empty());
    BOOST_CHECK(ParseUpdates(UniValue{UniValue::VOBJ}).Empty());

    // Each field refused as it should be.
    const auto refused{[](const std::string& json) {
        UniValue value;
        BOOST_REQUIRE(value.read(json));
        return RpcError([&] { ParseData(value); });
    }};
    BOOST_CHECK_EQUAL(refused(R"({"colour": "gold"})"), "Unknown field of asset data: colour");
    BOOST_CHECK_EQUAL(refused(strprintf(R"({"info": "%s"})", std::string(MAX_INFO_SIZE + 1, 'x'))), strprintf("info: at most %u bytes", MAX_INFO_SIZE));
    BOOST_CHECK_EQUAL(refused(R"({"commitment": "abcd"})"), "commitment: 32 bytes in hex");
    BOOST_CHECK(refused(R"({"ipv4": "2001:db8::7"})").starts_with("ipv4: an IPv4 address"));
    BOOST_CHECK(refused(R"({"ipv4": "not an address"})").starts_with("ipv4: an IPv4 address"));
    BOOST_CHECK(refused(R"({"ipv6": "203.0.113.7"})").starts_with("ipv6: an IPv6 address"));
    BOOST_CHECK_EQUAL(refused(R"({"encryptionkey": "02"})"), "encryptionkey: a compressed public key, 33 bytes in hex");
    BOOST_CHECK_EQUAL(refused(strprintf(R"({"encryptionkey": "%s"})", std::string(66, '0'))), "encryptionkey: a compressed public key, 33 bytes in hex");
    BOOST_CHECK_EQUAL(refused(R"({"signingkey": "00"})"), "signingkey: an x-only public key, 32 bytes in hex");
    BOOST_CHECK_EQUAL(refused(strprintf(R"({"signingkey": "%s"})", std::string(64, 'f'))), "signingkey: an x-only public key, 32 bytes in hex");
    BOOST_CHECK_EQUAL(RpcError([] { ParseData(UniValue{"info"}); }), "The data of an asset is an object");

    // Assets as arguments, labels and amounts.
    BOOST_CHECK(ParseAssetArg(UniValue{"0x" + GOLD.GetHex()}) == GOLD);
    BOOST_CHECK(ParseAssetArg(UniValue{"CHN"}) == CHN);
    BOOST_CHECK(ParseAssetArg(UniValue{"1739-0029"}) == HashName("1739-0029"));
    BOOST_CHECK_EQUAL(RpcError([] { ParseAssetArg(UniValue{""}); }), "The asset is empty");
    AssetRecord record;
    BOOST_CHECK_EQUAL(AssetLabel(CHN, nullptr), "CHN");
    BOOST_CHECK_EQUAL(AssetLabel(GOLD, &record), "0x" + GOLD.GetHex());
    record.text = "GOLD";
    BOOST_CHECK_EQUAL(AssetLabel(GOLD, &record), "GOLD");
    BOOST_CHECK_EQUAL(FormatUnits(150, 2), "1.50");
    BOOST_CHECK_EQUAL(FormatUnits(7, 0), "7");
    BOOST_CHECK_EQUAL(ParseUnits(UniValue{"1.5"}, 2), 150u);
    BOOST_CHECK_EQUAL(ParseUnits(UniValue{"0"}, 2, /*allow_zero=*/true), 0u);
    BOOST_CHECK_EQUAL(RpcError([] { ParseUnits(UniValue{"0"}, 2); }), "The amount is zero");
    BOOST_CHECK(RpcError([] { ParseUnits(UniValue{"1.001"}, 2); }).starts_with("Invalid amount 1.001"));
    BOOST_CHECK(RpcError([] { ParseUnits(UniValue{"-1"}, 2); }).starts_with("Invalid amount -1"));
    BOOST_CHECK_EQUAL(RpcError([] { ParseUnits(UniValue{UniValue::VOBJ}, 2); }), "An amount is a number or a string");
}

BOOST_AUTO_TEST_SUITE_END()

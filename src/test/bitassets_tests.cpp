// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/state.h>
#include <addresstype.h>
#include <arith_uint256.h>
#include <script/script.h>
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
    bool ApplyTx(const CTransaction& tx, int height, TUndo& undo, std::vector<CTxOut>& payouts, std::string& reason, int pool_rules_height = 0, CAmount* released = nullptr, int release_height = 0, int audit_height = 0, int audit2_height = 0)
    {
        const bool ok{bitassets::State{*m_store}.ApplyTx(tx, height, payouts, reason, pool_rules_height, released, release_height, audit_height, audit2_height)};
        undo.parts.push_back(m_store->TakeUndo());
        return ok;
    }
    bool CheckTx(const CTransaction& tx, int height, std::string& reason, std::vector<Result>* results = nullptr, int pool_rules_height = 0, int release_height = 0, int audit_height = 0, int audit2_height = 0) const
    {
        return View().CheckTx(tx, height, reason, results, pool_rules_height, release_height, audit_height, audit2_height);
    }
    void Revert(const TUndo& undo)
    {
        for (auto it{undo.parts.rbegin()}; it != undo.parts.rend(); ++it) m_store->Revert(*it);
        m_store->TakeUndo();
    }
    bitassets::State View() const { return bitassets::State{*m_store}; }
    const sidechain::StoreView& Store() const { return *m_store; }
    /** How many entries there are under a prefix. */
    size_t Count(const sidechain::StoreBytes& prefix) const
    {
        size_t n{0};
        m_store->ForEach(prefix, [&](const sidechain::StoreBytes&, const sidechain::StoreBytes&) { ++n; return true; });
        return n;
    }
    bool Releasable(const AssetId& asset, std::string* why = nullptr, bool audit2 = true) const { return View().Releasable(asset, why, audit2); }
    std::optional<AssetId> AssetOfSeq(uint32_t seq) const { return View().AssetOfSeq(seq); }
    uint32_t NextSeq() const { return View().NextSeq(); }
    std::optional<Pool> FindPool(const AssetId& a, const AssetId& b) const { return View().FindPool(a, b); }
    uint64_t NextReservationOrder() const { return View().NextReservationOrder(); }
    std::map<COutPoint, Token> Tokens() const { std::map<COutPoint, Token> m; View().ForEachToken([&](const COutPoint& k, const Token& v) { m.emplace(k, v); return true; }); return m; }
    std::map<AssetId, AssetRecord> Assets() const { std::map<AssetId, AssetRecord> m; View().ForEachAsset([&](const AssetId& k, const AssetRecord& v) { m.emplace(k, v); return true; }); return m; }
    std::map<Txid, uint256> Reservations() const { std::map<Txid, uint256> m; View().ForEachReservation([&](const Txid& k, const uint256& v) { m.emplace(k, v); return true; }); return m; }
    std::map<uint256, Pool> Pools() const { std::map<uint256, Pool> m; View().ForEachPool([&](const uint256& k, const Pool& v) { m.emplace(k, v); return true; }); return m; }
    std::map<Txid, Auction> Auctions() const { std::map<Txid, Auction> m; View().ForEachAuction([&](const Txid& k, const Auction& v) { m.emplace(k, v); return true; }); return m; }
    std::map<Txid, uint64_t> ReservationOrder() const
    {
        std::map<Txid, uint64_t> m;
        sidechain::Table<uint256, uint64_t>{0x36}.ForEach(*m_store, [&](const uint256& k, const uint64_t& v) { m.emplace(Txid::FromUint256(k), v); return true; });
        return m;
    }
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
    //! The second audit's rules: from 0, as on a new chain.
    int audit2_height{0};

    bool Apply(const CTransaction& tx, std::string* reason = nullptr)
    {
        std::string r;
        const bool ok{state.ApplyTx(tx, height, undo, payouts, r, 0, nullptr, 0, 0, audit2_height)};
        if (reason) *reason = r;
        return ok;
    }
    std::string Reject(const CTransaction& tx)
    {
        std::string r;
        const TState before{state};
        BOOST_CHECK(!state.ApplyTx(tx, height, undo, payouts, r, 0, nullptr, 0, 0, audit2_height));
        BOOST_CHECK(state == before);
        return r;
    }
    /** Registers `name` with `supply`: the outputs of the control coin and of the supply. */
    std::pair<COutPoint, COutPoint> Issue(const std::string& name, uint64_t supply, uint8_t decimals = 0)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserve{MakeReserve(height >= audit2_height ? ReservationCommitment(id, nonce, HOLDER) : ReservationCommitment(id, nonce))};
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
        const CTransaction reserve{MakeTx({}, Reserve{ReservationCommitment(GOLD, nonce)}, {Unit(Token::Kind::RESERVATION, uint256{})})};
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

BOOST_AUTO_TEST_CASE(oldest_reservation_reveals)
{
    // Someone sees a registration in the mempool, which reveals the name and nonce of a reservation,
    // makes a reservation of the same commitment and tries to register first: refused, the oldest
    // reservation of a commitment is the one that reveals it. (Before the second audit's rules, which
    // bind a reservation to its output's script: see reservation_bound_to_script.)
    Fixture f;
    f.audit2_height = 1000;
    const TState empty{f.state};
    const uint256 nonce{HashName("secret")};
    const uint256 commitment{ReservationCommitment(GOLD, nonce)};
    const CTransaction mine{MakeTx({}, Reserve{commitment}, {Unit(Token::Kind::RESERVATION, uint256{})})};
    BOOST_REQUIRE(f.Apply(mine));
    const CTransaction copy{MakeTx({}, Reserve{commitment}, {Unit(Token::Kind::RESERVATION, uint256{})})};
    BOOST_REQUIRE(f.Apply(copy));
    bitassets::Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.text = "GOLD";
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({COutPoint{copy.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})), "bad-ba-reservation-not-first");
    std::string reason;
    BOOST_CHECK_MESSAGE(f.Apply(MakeTx({COutPoint{mine.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)}), &reason), reason);
    BOOST_CHECK(f.state.Assets().contains(GOLD));

    // Undone, the order of the reservations goes with them.
    TState reverted{f.state};
    reverted.Revert(f.undo);
    BOOST_CHECK(reverted == empty);

    // Before the rule, any reservation of the commitment would do.
    Fixture before;
    const CTransaction a{MakeTx({}, Reserve{commitment}, {Unit(Token::Kind::RESERVATION, uint256{})})};
    const CTransaction b{MakeTx({}, Reserve{commitment}, {Unit(Token::Kind::RESERVATION, uint256{})})};
    std::string r;
    BOOST_REQUIRE(before.state.ApplyTx(a, 100, before.undo, before.payouts, r, 0, nullptr, 0, /*audit_height=*/1000, /*audit2_height=*/1000));
    BOOST_REQUIRE(before.state.ApplyTx(b, 100, before.undo, before.payouts, r, 0, nullptr, 0, /*audit_height=*/1000, /*audit2_height=*/1000));
    BOOST_CHECK_MESSAGE(before.state.ApplyTx(MakeTx({COutPoint{b.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)}), 100, before.undo, before.payouts, r, 0, nullptr, 0, /*audit_height=*/1000, /*audit2_height=*/1000), r);
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
    BOOST_CHECK(commitment != ReservationCommitment(GOLD, nonce));
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

    // A commitment to the name alone, made under the new rules, registers nothing.
    Fixture g;
    const CTransaction unbound{MakeReserve(ReservationCommitment(GOLD, nonce))};
    BOOST_REQUIRE(g.Apply(unbound));
    BOOST_CHECK_EQUAL(g.Reject(MakeTx({COutPoint{unbound.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})), "bad-ba-no-reservation");

    // One made before them still does, after them, under the oldest-reservation rule: a copy of it
    // made after the rules is younger, and itself registers nothing.
    Fixture h;
    h.audit2_height = 150;
    const CTransaction old{MakeReserve(ReservationCommitment(GOLD, nonce))};
    BOOST_REQUIRE(h.Apply(old));
    h.height = 150;
    const CTransaction later_copy{MakeReserve(ReservationCommitment(GOLD, nonce), theirs)};
    BOOST_REQUIRE(h.Apply(later_copy));
    BOOST_CHECK_EQUAL(h.Reject(MakeTx({COutPoint{later_copy.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)})), "bad-ba-no-reservation");
    BOOST_CHECK_MESSAGE(h.Apply(MakeTx({COutPoint{old.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, GOLD)}), &reason), reason);

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
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, add, {Asset(GOLD, 900'000), std::nullopt}, 49'999'999)), "bad-ba-marker-value");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, add, {Asset(GOLD, 900'000)}, 50'000'000)), "bad-ba-results");
    const CTransaction added{MakeTx({coins}, add, {Asset(GOLD, 900'000), std::nullopt}, 50'000'000)};
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
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, AddLiquidity{GOLD, CHN, 100, 900'000, 1}, {Asset(GOLD, 999'900), std::nullopt}, 900'000)), "bad-ba-pool-too-small");
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt}, 50'000'000)};
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
    // Before the rules' height, as on the test network before them, the trade went through.
    std::string r2;
    TState before{f.state};
    BOOST_CHECK(before.CheckTx(MakeTx({}, Swap{CHN, 500'000'000, GOLD, 1, CScript{}}, {std::nullopt}, 500'000'000), f.height, r2, nullptr, f.height + 1));
    // A dust deposit does not reopen it; a real one does, and it trades again.
    const COutPoint gold{added.GetHash(), 0};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({gold}, AddLiquidity{GOLD, CHN, 10, 100'000, 1}, {Asset(GOLD, 899'990), std::nullopt}, 100'000)), "bad-ba-pool-too-small");
    const CTransaction reopened{MakeTx({gold}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 800'000), std::nullopt}, 50'000'000)};
    BOOST_REQUIRE_MESSAGE(f.Apply(reopened, &reason), reason);
    BOOST_CHECK(!amm::Abandoned(f.state.Pools().at(id)));
    BOOST_CHECK(f.Apply(MakeTx({}, Swap{CHN, 1'000'000, GOLD, 1, CScript{}}, {std::nullopt}, 1'000'000)));
}

BOOST_AUTO_TEST_CASE(dead_assets_retire)
{
    Fixture f;
    const auto [control, coins]{f.Issue("GOLD", 1'000'000)};
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt}, 50'000'000)};
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
    // Not before the rule's height.
    std::string reason;
    BOOST_CHECK(!f.state.CheckTx(MakeTx({}, ReleaseAsset{GOLD}, {}), f.height, reason, nullptr, 0, f.height + 1));
    BOOST_CHECK_EQUAL(reason, "bad-ba-release-not-active");
    const TState before{f.state};
    f.undo = TUndo{};
    CAmount released{0};
    BOOST_REQUIRE(f.state.ApplyTx(MakeTx({}, ReleaseAsset{GOLD}, {}), f.height, f.undo, f.payouts, reason, 0, &released));
    BOOST_CHECK_EQUAL(released, left);
    BOOST_CHECK(!f.state.Assets().contains(GOLD));
    BOOST_CHECK(!f.state.Pools().contains(id));
    // Undone, it is all back.
    f.state.Revert(f.undo);
    BOOST_CHECK(f.state == before);
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
    CreateAuction create{GOLD, 40, CHN, 4'000, 400, f.height + 1, 10};
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, CreateAuction{GOLD, 40, CHN, 4'000, 400, f.height - 1, 10}, {Asset(GOLD, 60), Unit(Token::Kind::RECEIPT, uint256{})})), "bad-ba-auction-started");
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({coins}, create, {Asset(GOLD, 60)})), "bad-ba-tokens-lost");
    const CTransaction made{MakeTx({coins}, create, {Asset(GOLD, 60), Unit(Token::Kind::RECEIPT, uint256{})})};
    BOOST_REQUIRE(f.Apply(made));
    const Txid id{made.GetHash()};
    const COutPoint receipt{id, 1};
    BOOST_CHECK(f.state.Tokens().at(receipt) == Unit(Token::Kind::RECEIPT, id.ToUint256()));

    // Not started yet.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 1'000, 1, CScript{}}, {std::nullopt}, 1'000)), "bad-ba-auction-closed");
    // Canceled before any bid, everything back: possible, but not now.
    f.height = 101;
    // A bid of 1000 at the start (4000 for 40) buys 10.
    const CTransaction bid{MakeTx({}, Bid{id, 1'000, 10, CScript{}}, {std::nullopt}, 1'000)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(f.Apply(bid, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{bid.GetHash(), 0}) == Asset(GOLD, 10));
    BOOST_CHECK_EQUAL(f.state.Auctions().at(id).remaining, 30u);
    // More than is left: refused.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 4'000, 1, CScript{}}, {std::nullopt}, 4'000)), "bad-ba-bid-price");
    // With a bid, it cannot be collected while it runs.
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({receipt}, Collect{id, HOLDER}, {std::nullopt})), "bad-ba-auction-running");
    // In its last block, at the end price (the original's node stopped here): 400 for 40, 10 per unit.
    f.height = 110;
    const CTransaction last{MakeTx({}, Bid{id, 100, 10, CScript{}}, {std::nullopt}, 100)};
    BOOST_REQUIRE_MESSAGE(f.Apply(last, &reason), reason);
    BOOST_CHECK_EQUAL(f.state.Auctions().at(id).remaining, 20u);
    f.height = 111;
    BOOST_CHECK_EQUAL(f.Reject(MakeTx({}, Bid{id, 100, 1, CScript{}}, {std::nullopt}, 100)), "bad-ba-auction-closed");
    // Collected: what is left (GOLD) to the result output, what came in (CHN) by the coinbase.
    f.payouts.clear();
    const CTransaction collect{MakeTx({receipt}, Collect{id, HOLDER}, {std::nullopt})};
    BOOST_REQUIRE_MESSAGE(f.Apply(collect, &reason), reason);
    BOOST_CHECK(f.state.Tokens().at(COutPoint{collect.GetHash(), 0}) == Asset(GOLD, 20));
    BOOST_REQUIRE_EQUAL(f.payouts.size(), 1u);
    BOOST_CHECK_EQUAL(f.payouts[0].nValue, 1'100);
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
    const CTransaction added{MakeTx({coins}, AddLiquidity{GOLD, CHN, 100'000, 50'000'000, 1}, {Asset(GOLD, 900'000), std::nullopt}, 50'000'000)};
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


BOOST_AUTO_TEST_SUITE_END()

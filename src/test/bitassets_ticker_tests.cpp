// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/index.h>
#include <bitassets/state.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace bitassets;
using sidechain::StoreBytes;

namespace {

const CScript HOLDER{CScript() << OP_0 << std::vector<unsigned char>(20, 0x11)};
int g_nonce{0};

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs)
{
    CMutableTransaction tx;
    ++g_nonce;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(g_nonce)}), static_cast<uint32_t>(g_nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (const auto& token : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), token});
        tx.vout.emplace_back(0, HOLDER);
    }
    tx.vout.emplace_back(0, MarkerScript(marker));
    return CTransaction{tx};
}

Token Unit(Token::Kind kind, const uint256& id) { return Token{kind, id, 1}; }

/** Blocks of one transaction each, over a store, with the search index kept as a node keeps it. */
struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay base{empty};
    std::vector<sidechain::StoreUndo> undos;
    int height{100};

    /** Connects a block of `tx` (if the rules take it); the reason if not. */
    std::string Connect(const CTransaction& tx)
    {
        sidechain::StoreOverlay block{base, /*journal=*/true};
        std::vector<CTxOut> payouts;
        std::string reason;
        if (!State{block}.ApplyTx(tx, height, payouts, reason)) return reason.empty() ? "?" : reason;
        index::ConnectBlock(block, height, 10);
        undos.push_back(block.TakeUndo());
        block.MergeInto(base);
        ++height;
        return "";
    }
    void DisconnectLast()
    {
        base.Revert(undos.back());
        undos.pop_back();
        --height;
    }
    State View() const { return State{base}; }

    /** Registers `name`: the output of its control coin. */
    COutPoint Issue(const std::string& name, const std::string& display = "")
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserve{[&] {
            CMutableTransaction tx{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Unit(Token::Kind::RESERVATION, uint256{})})};
            return CTransaction{tx};
        }()};
        BOOST_REQUIRE_EQUAL(Connect(reserve), "");
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = 10;
        reg.text = name;
        reg.display_name = display;
        const CTransaction tx{MakeTx({COutPoint{reserve.GetHash(), 0}}, reg, {Unit(Token::Kind::CONTROL, id), Token{Token::Kind::ASSET, id, 10}})};
        BOOST_REQUIRE_EQUAL(Connect(tx), "");
        return COutPoint{tx.GetHash(), 0};
    }
    /** Sets a display name with the control coin at `control`; the new output of it. */
    std::pair<std::string, COutPoint> SetName(const COutPoint& control, const AssetId& id, const std::string& display)
    {
        const CTransaction tx{MakeTx({control}, SetDisplayName{id, display}, {Unit(Token::Kind::CONTROL, id)})};
        const std::string reason{Connect(tx)};
        return {reason, reason.empty() ? COutPoint{tx.GetHash(), 0} : control};
    }
};

std::string U8(const char8_t* s) { return std::string{reinterpret_cast<const char*>(s)}; }

std::vector<uint32_t> Seqs(const sidechain::StoreView& view, const std::string& text, index::Sort sort = index::Sort::RELEVANCE, std::vector<std::string> exclude = {})
{
    index::Request r;
    r.text = text;
    r.sort = sort;
    r.filters.exclude_keywords = std::move(exclude);
    std::vector<uint32_t> seqs;
    while (true) {
        const index::Page page{index::Search(view, r)};
        for (const auto& hit : page.hits) seqs.push_back(hit.row.seq);
        if (page.next.empty()) break;
        r.cursor = page.next;
    }
    return seqs;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bitassets_ticker_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(tickers)
{
    for (const char* good : {"GOLD", "AB", "BTC2", "ABCDEFGHIJKL", "1INCH", "00", "17390029"}) BOOST_CHECK_MESSAGE(IsTicker(good), good);
    for (const char* bad : {"", "G", "ABCDEFGHIJKLM", "gold", "Gold", "GO LD", "GO-LD", "GO_LD", "GOLD.", "#1", "(A)", "1739-0029", "ÄB"}) BOOST_CHECK_MESSAGE(!IsTicker(bad), bad);
    // Tickers are names: the look-alike rules hold them too.
    BOOST_CHECK(ReadsAsAnotherAsset("CHN"));
    BOOST_CHECK(ReadsAsAnotherAsset("0XAB"));
    BOOST_CHECK(!NameMimicsNumber("17390029"));
}

BOOST_AUTO_TEST_CASE(display_names)
{
    const std::string max(MAX_DISPLAY_NAME_SIZE, 'a');
    for (const std::string& good : {std::string{"Gold Bar Token, 1 g"}, std::string{"A"}, max, U8(u8"Café à Montréal"), U8(u8"金条"),
                                    U8(u8"Золото"), U8(u8"\U0001FA99 Gold"), U8(u8"© € 100%"), U8(u8"한글"),
                                    std::string{"GOLD (#1739-0029)"}}) {
        BOOST_CHECK_MESSAGE(IsDisplayName(good), good);
    }
    // 64 bytes of UTF-8 (32 of 2 bytes): taken; one more byte is not.
    std::string two;
    for (int i{0}; i < 32; ++i) two += U8(u8"é");
    BOOST_CHECK(IsDisplayName(two));
    BOOST_CHECK(!IsDisplayName(two + "a"));
    BOOST_CHECK(!IsDisplayName(max + "a"));

    const std::vector<std::pair<std::string, const char*>> bad{
        {"", "empty"},
        {" Gold", "space first"},
        {"Gold ", "space last"},
        {std::string{"Go\nld"}, "a newline"},
        {std::string{"Go\tld"}, "a tab"},
        {std::string{"Go\x7f" "ld"}, "DEL"},
        {std::string("Go\0ld", 5), "NUL"},
        {U8(u8"Gold\u0085"), "a C1 control"},
        // Spoofing: what changes how the rest reads.
        {U8(u8"GOLD‮DLOG"), "right-to-left override"},
        {U8(u8"GOLD‪X"), "left-to-right embedding"},
        {U8(u8"GOLD⁦X⁩"), "isolates"},
        {U8(u8"GOLD‏X"), "right-to-left mark"},
        {U8(u8"GO​LD"), "zero-width space"},
        {U8(u8"GO‍LD"), "zero-width joiner"},
        {U8(u8"GO‌LD"), "zero-width non-joiner"},
        {U8(u8"GO⁠LD"), "word joiner"},
        {U8(u8"﻿GOLD"), "byte order mark"},
        {U8(u8"GO­LD"), "soft hyphen"},
        {U8(u8"GO؜LD"), "Arabic letter mark"},
        {U8(u8"GOLD\U000E0041"), "a tag character"},
        {U8(u8"GOLD️"), "a variation selector"},
        {U8(u8"Gold Bar"), "no-break space"},
        {U8(u8"Gold　Bar"), "ideographic space"},
        {U8(u8"Gold Bar"), "line separator"},
        {U8(u8"Goldㅤ"), "Hangul filler"},
        {U8(u8"Gold⠀"), "braille blank"},
        // Not in NFC, or combining.
        {U8(u8"Café"), "a combining acute (NFD)"},
        {U8(u8"Å"), "the angstrom sign (NFC: U+00C5)"},
        {U8(u8"Ω"), "the ohm sign (NFC: U+03A9)"},
        {U8(u8"가"), "conjoining jamo"},
        {U8(u8"豈"), "a CJK compatibility ideograph"},
        // Not assigned, private, not characters.
        {U8(u8"͸"), "unassigned"},
        {U8(u8""), "private use"},
        {U8(u8"￾"), "a noncharacter"},
        // Not well-formed UTF-8.
        {std::string{"\xc0\xaf"}, "an overlong slash"},
        {std::string{"\xe0\x80\xaf"}, "an overlong in three"},
        {std::string{"\xed\xa0\x80"}, "a surrogate"},
        {std::string{"\xf4\x90\x80\x80"}, "past U+10FFFF"},
        {std::string{"\xc3"}, "cut short"},
        {std::string{"\x80"}, "a lone continuation"},
        {std::string{"\xff"}, "0xFF"},
    };
    for (const auto& [text, what] : bad) {
        BOOST_CHECK_MESSAGE(!IsDisplayName(text), what);
        BOOST_CHECK_MESSAGE(WhyNotDisplayName(text).has_value(), what);
    }
}

BOOST_AUTO_TEST_CASE(display_name_markers)
{
    // SetDisplayName, a name and none; a registration with one (and a logo or not): read back the same.
    for (const std::string& text : {std::string{"Gold Bar Token"}, std::string{}}) {
        Marker marker;
        marker.operation = SetDisplayName{uint256::ONE, text};
        const auto parsed{ParseMarkerScript(MarkerScript(marker))};
        BOOST_REQUIRE(parsed);
        BOOST_CHECK(*parsed == marker);
    }
    Register reg;
    reg.name = HashName("GOLD");
    reg.text = "GOLD";
    reg.display_name = "Gold Bar Token";
    Marker marker;
    marker.operation = reg;
    const CScript script{MarkerScript(marker)};
    const auto parsed{ParseMarkerScript(script)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(std::get<Register>(*parsed->operation).display_name == "Gold Bar Token");
    // The kind of a registration with a display name.
    {
        CScript::const_iterator pc{script.begin() + 1};
        opcodetype opcode;
        std::vector<unsigned char> data;
        BOOST_REQUIRE(script.GetOp(pc, opcode, data));
        BOOST_CHECK_EQUAL(data.at(sizeof(TAG) + 1), REGISTER_WITH_DISPLAY_NAME_KIND);
    }
    // Written with that kind but no display name: not a marker (one way to write one).
    {
        DataStream s{};
        Register plain{reg};
        plain.display_name.clear();
        s << std::span{TAG} << MARKER_VERSION << REGISTER_WITH_DISPLAY_NAME_KIND << plain << std::vector<unsigned char>{} << std::string{};
        WriteCompactSize(s, 0);
        std::vector<unsigned char> data(UCharCast(s.data()), UCharCast(s.data()) + s.size());
        BOOST_CHECK(!ParseMarkerScript(CScript() << OP_RETURN << data));
    }
    BOOST_CHECK(IsDisplayNameOperation(reg));
    BOOST_CHECK(IsDisplayNameOperation(SetDisplayName{uint256::ONE, ""}));
    reg.display_name.clear();
    BOOST_CHECK(!IsDisplayNameOperation(reg));

    // Refused by the checks of the marker alone.
    std::string reason;
    const CTransaction bad_set{MakeTx({}, SetDisplayName{uint256::ONE, U8(u8"GOLD‮")}, {})};
    BOOST_CHECK(!CheckMarker(bad_set, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name");
    const CTransaction chn_set{MakeTx({}, SetDisplayName{uint256{}, "x"}, {})};
    BOOST_CHECK(!CheckMarker(chn_set, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name-asset");
    reg.display_name = " spaced";
    BOOST_CHECK(!CheckMarker(MakeTx({}, reg, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name");
}

BOOST_AUTO_TEST_CASE(activation)
{
    Register reg;
    reg.name = HashName("gold bar");
    reg.text = "gold bar";
    std::string reason;
    // Before: the names of before, and no display name.
    BOOST_CHECK(CheckTickerActivation(MakeTx({}, reg, {}), /*active=*/false, reason));
    reg.display_name = "Gold";
    BOOST_CHECK(!CheckTickerActivation(MakeTx({}, reg, {}), /*active=*/false, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name-inactive");
    BOOST_CHECK(!CheckTickerActivation(MakeTx({}, SetDisplayName{uint256::ONE, ""}, {}), /*active=*/false, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name-inactive");
    // From: a ticker, published.
    BOOST_CHECK(!CheckTickerActivation(MakeTx({}, reg, {}), /*active=*/true, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-ticker");
    reg.text = "GOLD";
    reg.name = HashName("GOLD");
    BOOST_CHECK(CheckTickerActivation(MakeTx({}, reg, {}), /*active=*/true, reason));
    reg.text.reset();
    BOOST_CHECK(!CheckTickerActivation(MakeTx({}, reg, {}), /*active=*/true, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-ticker-private");
    BOOST_CHECK(CheckTickerActivation(MakeTx({}, SetDisplayName{uint256::ONE, "x"}, {}), /*active=*/true, reason));
    // What is no registration is not held to it: an asset of before (any name) keeps working.
    BOOST_CHECK(CheckTickerActivation(MakeTx({}, Mint{HashName("gold bar"), 5}, {}), /*active=*/true, reason));
}

BOOST_AUTO_TEST_CASE(display_name_state_and_index)
{
    Chain chain;
    // An asset of before: its name is no ticker; it keeps it, and gets a display name all the same.
    const AssetId old_id{HashName("my gold coin")};
    COutPoint old_control{chain.Issue("my gold coin")};
    const AssetId gold{HashName("GOLD")};
    COutPoint control{chain.Issue("GOLD", "Gold Bar Token, 1 g")};
    BOOST_REQUIRE(chain.View().GetDisplayName(gold));
    BOOST_CHECK_EQUAL(chain.View().GetDisplayName(gold)->text, "Gold Bar Token, 1 g");
    BOOST_CHECK(!chain.View().GetDisplayName(old_id));
    const auto row{index::GetAssetRow(chain.base, 1)};
    BOOST_REQUIRE(row);
    BOOST_CHECK_EQUAL(row->display, "Gold Bar Token, 1 g");
    BOOST_CHECK(row->flags & index::HAS_DISPLAY);

    // Change it, by the controller.
    auto [reason, next]{chain.SetName(control, gold, "Gold Bar")};
    BOOST_CHECK_EQUAL(reason, "");
    control = next;
    BOOST_CHECK_EQUAL(chain.View().GetDisplayName(gold)->text, "Gold Bar");
    BOOST_CHECK_EQUAL(index::GetAssetRow(chain.base, 1)->display, "Gold Bar");
    // Not without the control coin of the asset: that of another is not it.
    std::tie(reason, std::ignore) = chain.SetName(old_control, gold, "Mine now");
    BOOST_CHECK_EQUAL(reason, "bad-ba-no-control");
    // Not unknown assets.
    std::tie(reason, std::ignore) = chain.SetName(old_control, HashName("NOPE"), "x");
    BOOST_CHECK_EQUAL(reason, "bad-ba-asset-unknown");
    // Not removed when there is none.
    std::tie(reason, std::ignore) = chain.SetName(old_control, old_id, "");
    BOOST_CHECK_EQUAL(reason, "bad-ba-display-name-none");
    // Display names are not unique: the old asset takes the same one.
    std::tie(reason, old_control) = chain.SetName(old_control, old_id, "Gold Bar");
    BOOST_CHECK_EQUAL(reason, "");

    // Found by the display name: the names first, then the display names' starts (by display name, then
    // number: both have "Gold Bar"), then their words.
    const COutPoint silver{chain.Issue("SILV", "Silver and gold dust")};
    (void)silver;
    BOOST_CHECK(Seqs(chain.base, "gold") == (std::vector<uint32_t>{1, 0, 2}));
    BOOST_CHECK(Seqs(chain.base, "gold bar") == (std::vector<uint32_t>{0, 1}));
    BOOST_CHECK(Seqs(chain.base, "dust") == (std::vector<uint32_t>{2}));
    BOOST_CHECK(Seqs(chain.base, "silv") == (std::vector<uint32_t>{2}));
    BOOST_CHECK(Seqs(chain.base, "gold", index::Sort::NEWEST) == (std::vector<uint32_t>{2, 1, 0}));
    BOOST_CHECK(Seqs(chain.base, "gold", index::Sort::NAME) == (std::vector<uint32_t>{1, 0, 2}));
    // A word left out is left out of the display names too.
    BOOST_CHECK(Seqs(chain.base, "gold", index::Sort::RELEVANCE, {"dust"}) == (std::vector<uint32_t>{1, 0}));
    {
        index::Request r;
        r.text = "dust";
        const auto page{index::Search(chain.base, r)};
        BOOST_REQUIRE_EQUAL(page.hits.size(), 1U);
        BOOST_CHECK_EQUAL(page.hits[0].match, 4);
        r.text = "silver";
        BOOST_CHECK_EQUAL(index::Search(chain.base, r).hits.at(0).match, 3);
        r.text = "gold";
        BOOST_CHECK_EQUAL(index::Search(chain.base, r).hits.at(0).match, 0);
    }

    // Removed, by the controller; a reorganisation puts it back, in the state and the index.
    std::tie(reason, control) = chain.SetName(control, gold, "");
    BOOST_CHECK_EQUAL(reason, "");
    BOOST_CHECK(!chain.View().GetDisplayName(gold));
    BOOST_CHECK(index::GetAssetRow(chain.base, 1)->display.empty());
    BOOST_CHECK(!(index::GetAssetRow(chain.base, 1)->flags & index::HAS_DISPLAY));
    BOOST_CHECK(Seqs(chain.base, "gold bar") == (std::vector<uint32_t>{0}));
    chain.DisconnectLast();
    BOOST_CHECK_EQUAL(chain.View().GetDisplayName(gold)->text, "Gold Bar");
    BOOST_CHECK_EQUAL(index::GetAssetRow(chain.base, 1)->display, "Gold Bar");
    BOOST_CHECK(Seqs(chain.base, "gold bar") == (std::vector<uint32_t>{0, 1}));
}

BOOST_AUTO_TEST_CASE(index_layout_upgrade)
{
    // A row of layout 1 (no display name) reads as a row without one, and is written the same.
    index::AssetRow row;
    row.seq = 7;
    row.text = "GOLD";
    row.flags = index::NAMED;
    const StoreBytes bytes{sidechain::EncodeValue(row)};
    BOOST_CHECK(sidechain::DecodeValue<index::AssetRow>(bytes) == row);
    row.display = "Gold";
    BOOST_CHECK(sidechain::EncodeValue(row).size() > bytes.size());
    BOOST_CHECK(sidechain::DecodeValue<index::AssetRow>(sidechain::EncodeValue(row)) == row);

    // An index of layout 1 is of layout 2 in place, unless the store holds display names.
    Chain chain;
    chain.Issue("GOLD");
    BOOST_CHECK(index::LayoutCurrent(chain.base));
    const StoreBytes layout_key{sidechain::DERIVED_TABLE, 'L'};
    chain.base.Put(layout_key, sidechain::EncodeValue(uint32_t{1}));
    BOOST_CHECK(!index::LayoutCurrent(chain.base));
    BOOST_CHECK(index::UpgradeLayout(chain.base));
    BOOST_CHECK(index::LayoutCurrent(chain.base));
    BOOST_CHECK(!index::UpgradeLayout(chain.base));
    chain.base.Put(sidechain::TableKey(0x92, HashName("GOLD")), sidechain::EncodeValue(DisplayNameRecord{"Gold", 1, Txid{}}));
    chain.base.Put(layout_key, sidechain::EncodeValue(uint32_t{1}));
    BOOST_CHECK(!index::UpgradeLayout(chain.base));
    BOOST_CHECK(!index::LayoutCurrent(chain.base));
}

BOOST_AUTO_TEST_SUITE_END()

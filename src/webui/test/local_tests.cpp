// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The journal of BitAssets (local.cpp): the assets being created and the prices seen, in a file of the
// data directory, with no cap but a sanity one (in the preferences of the app, capped at 256 kB, it could
// fill up).

#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/dialog.h>
#include <webui/guard.h>
#include <webui/local.h>
#include <test/bitassets_logo_samples.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <string>

BOOST_FIXTURE_TEST_SUITE(webui_local_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(journal_of_bitassets)
{
    const fs::path path{m_args.GetDataDirNet() / webui::local::JOURNAL_FILE};
    BOOST_CHECK(webui::local::ReadStore(path).isNull());

    // Larger than the preferences ever held: written whole, read back.
    UniValue value{UniValue::VOBJ};
    UniValue pending{UniValue::VOBJ};
    for (int i{0}; i < 4000; ++i) {
        UniValue rec{UniValue::VOBJ};
        rec.pushKV("supply", "1000");
        rec.pushKV("name", std::string(64, 'a' + i % 6));
        pending.pushKV(strprintf("w|%d", i), rec);
    }
    value.pushKV("pending", pending);
    BOOST_CHECK_GT(value.write().size(), 256U << 10);
    webui::local::WriteStore(path, value);
    const UniValue back{webui::local::ReadStore(path)};
    BOOST_CHECK_EQUAL(back.write(), value.write());
    BOOST_CHECK(!fs::exists(fs::PathFromString(fs::PathToString(path) + ".new")));

    // What is not an object, or too large, is refused, and the file stays as it was.
    BOOST_CHECK_THROW(webui::local::WriteStore(path, UniValue{"text"}), webui::HostError);
    UniValue huge{UniValue::VOBJ};
    huge.pushKV("x", std::string(webui::local::MAX_STORE_BYTES, 'x'));
    BOOST_CHECK_THROW(webui::local::WriteStore(path, huge), webui::HostError);
    BOOST_CHECK_EQUAL(webui::local::ReadStore(path).write(), value.write());

    // A file that cannot be read is kept aside, not lost: the names being created, with their supply and data.
    {
        std::ofstream broken{path.std_path(), std::ios::trunc};
        broken << "{\"pending\": {";
    }
    BOOST_CHECK(webui::local::ReadStore(path).isNull());
    BOOST_CHECK(fs::exists(fs::PathFromString(fs::PathToString(path) + ".unreadable")));
}

BOOST_AUTO_TEST_CASE(confirmation_of_an_amount_past_2_53_units)
{
    // What the host shows of a send of 2^60 + 1 units of an asset with 8 decimals (audit 7 E-BA6): the page sends the
    // amount as text, and the dialog of the host says it digit for digit, as the node will take it.
    UniValue params{UniValue::VARR};
    params.push_back("rbast1qexample");
    params.push_back("0x" + std::string(64, 'a'));
    params.push_back("11529215046.06846977");
    const auto rows{webui::guard::ParamRows("sendasset", params, {"address", "asset", "amount"}, [](const std::string& name) {
        return name == "address" ? webui::guard::ParamRole::DESTINATION : name == "amount" ? webui::guard::ParamRole::AMOUNT : webui::guard::ParamRole::OTHER;
    })};
    bool shown{false};
    for (const auto& row : rows) {
        if (row.label == "amount") {
            BOOST_CHECK_EQUAL(row.value, "11529215046.06846977");
            shown = true;
        }
    }
    BOOST_CHECK(shown);
}

BOOST_AUTO_TEST_CASE(logos_as_the_host_shows_them)
{
    // A logo the rules take: a data: URL of the type they found, its size and hash; the rows of a confirmation show it as a picture.
    std::string reason;
    const auto png{webui::local::ReadLogoParam(UniValue{EncodeBase64(ParseHex(LOGO_PNG_PALETTE))}, reason)};
    BOOST_REQUIRE(png);
    BOOST_CHECK_EQUAL(png->mime, "image/png");
    BOOST_CHECK(png->url.starts_with("data:image/png;base64,iVBORw0KGgo"));
    BOOST_CHECK(webui::dialog::IsImageDataUrl(png->url));
    const auto rows{webui::local::LogoRows(*png)};
    BOOST_REQUIRE_EQUAL(rows.size(), 3U);
    BOOST_CHECK(rows[0].kind == webui::dialog::Row::Kind::IMAGE);
    BOOST_CHECK_EQUAL(rows[2].value, png->hash);
    const auto jpeg{webui::local::ReadLogoParam(UniValue{EncodeBase64(ParseHex(LOGO_JPEG_BASELINE))}, reason)};
    BOOST_REQUIRE(jpeg);
    BOOST_CHECK(jpeg->url.starts_with("data:image/jpeg;base64,"));
    // Whatever the page or the chain says the type is, a file the rules refuse is no logo: an SVG, a PNG with more after it.
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{EncodeBase64(std::string{"<svg onload=alert(1)/>"})}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-format");
    auto trailing{ParseHex(LOGO_PNG_PALETTE)};
    trailing.push_back('<');
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{EncodeBase64(trailing)}, reason));
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{"not base64!"}, reason));
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{std::string(50000, 'A')}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-oversize");
    // Setting one pays: confirmed on the desktop; the page reads the transfers without the keys.
    const auto methods{webui::local::ConfirmedRpcMethods()};
    BOOST_CHECK(methods.contains("setassetlogo") && methods.at("setassetlogo").kind == webui::confirm::Method::Kind::SPEND);
    BOOST_CHECK(webui::local::RpcMethods().contains("listassettransactions"));
    BOOST_CHECK(!webui::local::RpcMethods().contains("setassetlogo"));
    // A display name pays too, with its two parameters (an empty one removes it), never from the page alone.
    BOOST_REQUIRE(methods.contains("setassetdisplayname"));
    BOOST_CHECK(methods.at("setassetdisplayname").kind == webui::confirm::Method::Kind::SPEND);
    BOOST_CHECK_EQUAL(methods.at("setassetdisplayname").required, 2U);
    BOOST_CHECK(!webui::local::RpcMethods().contains("setassetdisplayname"));
    // The registration sent after a reservation counts its display name in the fee it may pay.
    const auto& reveal{methods.at("reserveasset").reveal};
    BOOST_REQUIRE(reveal && reveal->second_vsize);
    UniValue plain{UniValue::VARR}, named{UniValue::VARR};
    for (UniValue* p : {&plain, &named}) {
        p->push_back("GOLD");
        p->push_back("1000");
        p->push_back(2);
        p->push_back(UniValue{UniValue::VOBJ});
        p->push_back(true);
    }
    named.push_back(UniValue{});
    named.push_back(std::string(64, 'g'));
    BOOST_CHECK_GE(reveal->second_vsize(named), reveal->second_vsize(plain) + 64);
}

BOOST_AUTO_TEST_CASE(logo_cache_counts_what_it_keeps)
{
    // The same asset asked again and again: one entry, its bytes counted once (the security review of the cache).
    webui::local::LogoCache cache;
    webui::local::Logo logo{"image/png", 3, std::string(64, 'a'), "data:image/png;base64," + std::string(40000, 'A')};
    for (int i{0}; i < 2000; ++i) cache.Put(std::string(64, 'b'), logo);
    BOOST_CHECK_EQUAL(cache.Size(), 1U);
    BOOST_CHECK_EQUAL(cache.Bytes(), logo.url.size());
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // Many assets: the oldest go, never below an empty list, and the count stays the sum.
    for (int i{0}; i < 2000; ++i) cache.Put(strprintf("%064x", i), logo);
    BOOST_CHECK(cache.Size() <= webui::local::LogoCache::MAX_ENTRIES);
    BOOST_CHECK(cache.Bytes() <= webui::local::LogoCache::MAX_BYTES);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // One larger than the whole cache is not kept, and empties nothing it should not count.
    webui::local::Logo huge{logo};
    huge.url = std::string(webui::local::LogoCache::MAX_BYTES + 1, 'A');
    const size_t before{cache.Size()};
    cache.Put(std::string(64, 'c'), huge);
    BOOST_CHECK_EQUAL(cache.Size(), before);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // Replacing a kept asset's logo by a huge one drops the old one too.
    cache.Put(std::string(64, 'd'), logo);
    cache.Put(std::string(64, 'd'), huge);
    BOOST_CHECK(cache.Size() < before);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
}

BOOST_AUTO_TEST_SUITE_END()

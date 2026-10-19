// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of BitAssets (src/webui/app/src/chain/local.ts): the RPC methods of the assets,
// the pools and the auctions they call (src/rpc/bitassets.cpp, src/wallet/rpc/bitassets.cpp), and the journal
// of what the chain does not keep (the assets being created, the prices of the pools as the app saw them),
// in a file of the data directory.

#include <webui/local.h>

#include <bitassets/logo.h>
#include <bitassets/state.h>
#include <common/args.h>
#include <crypto/sha256.h>
#include <util/strencodings.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>

namespace webui::local {

namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

/** Where the journal is kept: in the data directory of the network, apart from the settings of the app. */
fs::path StorePath(interfaces::Node& node, const std::string& name)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->args) throw HostError("The node is still starting");
    return context->args->GetDataDirNet() / fs::u8path(name);
}
} // namespace

std::optional<Logo> ReadLogo(std::span<const unsigned char> bytes, std::string& reason)
{
    const auto format{bitassets::CheckLogo(bytes, reason)};
    if (!format) return std::nullopt;
    Logo logo;
    logo.mime = bitassets::LogoMime(*format);
    logo.size = bytes.size();
    uint256 hash;
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash.data());
    logo.hash = HexStr(hash);
    // The type is the one the rules found, never one the chain or the page says.
    logo.url = "data:" + logo.mime + ";base64," + EncodeBase64(bytes);
    return logo;
}

std::optional<Logo> ReadLogoParam(const UniValue& value, std::string& reason)
{
    if (!value.isStr()) {
        reason = "not a string";
        return std::nullopt;
    }
    if (value.get_str().size() > (bitassets::MAX_LOGO_BYTES + 2) / 3 * 4) {
        reason = "bad-ba-logo-oversize";
        return std::nullopt;
    }
    const auto bytes{DecodeBase64(value.get_str())};
    if (!bytes) {
        reason = "not base64";
        return std::nullopt;
    }
    return ReadLogo(*bytes, reason);
}

std::vector<dialog::Row> LogoRows(const Logo& logo)
{
    return {
        confirm::Image("Logo", logo.url),
        confirm::Text("Logo file", strprintf("%s, %ux%u pixels, %u bytes", logo.mime == "image/png" ? "PNG" : "JPEG", bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, logo.size)),
        confirm::Text("Logo SHA-256", logo.hash),
    };
}

UniValue LogoCache::Get(interfaces::Node& node, const std::string& asset, const std::string& hash)
{
    if (asset.size() != 64 || !IsHex(asset)) throw HostError(BRIDGE_INVALID_REQUEST, "asset: the id of an asset (64 hex digits)");
    {
        std::lock_guard lock{m_mutex};
        for (auto it{m_entries.begin()}; it != m_entries.end(); ++it) {
            if (it->asset != asset || (!hash.empty() && it->logo.hash != hash)) continue;
            m_entries.splice(m_entries.begin(), m_entries, it);
            return ToJson(asset, it->logo);
        }
    }
    UniValue args{UniValue::VARR};
    args.push_back("0x" + asset);
    UniValue result;
    try {
        result = node.executeRpc("getassetlogo", args, "/");
    } catch (const UniValue&) {
        return UniValue{}; // no logo (or no such asset)
    } catch (const std::exception&) {
        return UniValue{};
    }
    if (!result.isObject() || !result.find_value("base64").isStr()) return UniValue{};
    std::string reason;
    const auto logo{ReadLogoParam(result.find_value("base64"), reason)};
    // Checked again here, as the rules check it: what the page gets is a picture of PNG or JPEG bytes, or nothing.
    if (!logo) return UniValue{};
    Put(asset, *logo);
    return ToJson(asset, *logo);
}

void LogoCache::Put(const std::string& asset, const Logo& logo)
{
    std::lock_guard lock{m_mutex};
    // The one before of this asset goes, its bytes counted out with it.
    for (auto it{m_entries.begin()}; it != m_entries.end();) {
        if (it->asset == asset) {
            m_bytes -= it->logo.url.size();
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Larger than the whole cache (no logo of the rules is: at most 44 KiB of URL): given, not kept.
    if (logo.url.size() > MAX_BYTES) return;
    m_entries.push_front({asset, logo});
    m_bytes += logo.url.size();
    while (!m_entries.empty() && (m_entries.size() > MAX_ENTRIES || m_bytes > MAX_BYTES)) {
        m_bytes -= m_entries.back().logo.url.size();
        m_entries.pop_back();
    }
}

size_t LogoCache::Bytes() const
{
    std::lock_guard lock{m_mutex};
    return m_bytes;
}

size_t LogoCache::Size() const
{
    std::lock_guard lock{m_mutex};
    return m_entries.size();
}

size_t LogoCache::CountedBytes() const
{
    std::lock_guard lock{m_mutex};
    size_t n{0};
    for (const Entry& e : m_entries) n += e.logo.url.size();
    return n;
}

UniValue LogoCache::ToJson(const std::string& asset, const Logo& logo)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("asset", asset);
    out.pushKV("hash", logo.hash);
    out.pushKV("mime", logo.mime);
    out.pushKV("size", static_cast<uint64_t>(logo.size));
    out.pushKV("url", logo.url);
    return out;
}

UniValue ReadStore(const fs::path& path)
{
    std::ostringstream text;
    {
        // Closed before anything else: on Windows an open file cannot be renamed.
        std::ifstream file{path.std_path(), std::ios::binary};
        if (!file) return UniValue{};
        text << file.rdbuf();
    }
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        // Kept aside rather than lost: the names being created, with their supply and data.
        std::error_code ec;
        fs::rename(path, fs::PathFromString(fs::PathToString(path) + ".unreadable"), ec);
        return UniValue{};
    }
    return value;
}

void WriteStore(const fs::path& path, const UniValue& value)
{
    if (!value.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "A store is an object");
    const std::string text{value.write()};
    if (text.size() > MAX_STORE_BYTES) throw HostError(strprintf("The record is too large (%u bytes; at most %u)", text.size(), MAX_STORE_BYTES));
    // Written whole to another file, flushed to the disk, then put in place: a crash leaves the old one or the new one.
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new")};
    FILE* file{fsbridge::fopen(temp, "wb")};
    if (!file) throw HostError(strprintf("%s cannot be written", fs::PathToString(temp)));
    const bool written{std::fwrite(text.data(), 1, text.size(), file) == text.size() && FileCommit(file)};
    if (std::fclose(file) != 0 || !written || !RenameOver(temp, path)) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw HostError(strprintf("%s could not be written", fs::PathToString(path)));
    }
}

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // What the node says of the assets, the pools and the auctions: read only.
        {"getbitassetsinfo", Bridge::Thread::WORKER},
        {"listassets", Bridge::Thread::WORKER},
        {"getasset", Bridge::Thread::WORKER},
        {"getassethistory", Bridge::Thread::WORKER},
        {"listpools", Bridge::Thread::WORKER},
        {"getpool", Bridge::Thread::WORKER},
        {"quoteswap", Bridge::Thread::WORKER},
        {"listauctions", Bridge::Thread::WORKER},
        {"getauction", Bridge::Thread::WORKER},
        {"quotebid", Bridge::Thread::WORKER},
        // Searches of the node, a page at a time (cursor; the pages never read a whole list: user feedback 2026-10-19).
        // A node without them answers "method not found", and the pages ask the list methods above instead.
        {"searchassets", Bridge::Thread::WORKER},
        {"searchpools", Bridge::Thread::WORKER},
        {"searchauctions", Bridge::Thread::WORKER},
        // What the wallet holds and did: they need the keys too (a reservation is told by a signature of the wallet).
        {"listmyassets", Bridge::Thread::WORKER},
        // Browse assets' "Held or controlled": searchassets among the wallet's assets (the same arguments; no key).
        {"searchmyassets", Bridge::Thread::WORKER},
        {"listassetactivity", Bridge::Thread::WORKER},
        // The asset moves of the wallet with the other side: it reads only (no key).
        {"listassettransactions", Bridge::Thread::WORKER},
        // What the wallet does, transactions it signs: confirmed on the desktop (ConfirmedRpcMethods, below).
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // They sign the nonce of a reservation to tell it (src/wallet/rpc/bitassets.cpp ReadyWallet): the passphrase of a locked
    // wallet asked by the host.
    return {"listmyassets", "listassetactivity"};
}

namespace {
/** The second step of reserveasset: registerasset of the same name, [name, supply, decimals, data, public]. */
confirm::Reveal RegisterAfterReserve()
{
    confirm::Reveal reveal;
    reveal.method = "registerasset";
    reveal.title = "Create an asset";
    reveal.accept = "Reserve and register";
    reveal.first_label = "Reservation";
    reveal.second_label = "Registration";
    reveal.what = "Two transactions: the reservation hides the name until it is deep enough, so that nobody who sees it coming can "
                  "take it first; the registration then creates the asset with the supply and data below, and its control coin.";
    reveal.matches = confirm::SameFirstParam;
    // A registration spends the reservation and makes the control coin, the supply and the change; its data adds to it.
    reveal.second_vsize = [](const UniValue& params) {
        // Its logo, if any, goes whole into the marker: a byte for a virtual byte.
        const int64_t logo{params.size() > 5 && params[5].isStr() ? static_cast<int64_t>(params[5].get_str().size() * 3 / 4) : 0};
        // And its display name (at most 64 bytes, and the logo's length when it has none).
        const int64_t display{params.size() > 6 && params[6].isStr() ? static_cast<int64_t>(params[6].get_str().size()) + 2 : 0};
        return int64_t{350} + (params.size() > 3 ? static_cast<int64_t>(params[3].write().size()) : 0) + logo + display;
    };
    reveal.when = confirm::AfterDepth("getbitassetsinfo", "reveal_depth");
    return reveal;
}
} // namespace

namespace {
/**
 * A describer that shows a logo among the parameters (`index`, base64) as a picture with its size and hash, never as text,
 * the other parameters as `generic` does; a logo the rules refuse is refused before anything is asked.
 */
std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> WithLogo(std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> generic, size_t index, bool remove_allowed)
{
    return [=](const confirm::Call& call) -> std::optional<Window::ConfirmDialog> {
        const UniValue value{call.Arg(index)};
        UniValue others{call.params};
        if (others.isArray() && index < others.size()) {
            UniValue trimmed{UniValue::VARR};
            for (size_t i{0}; i < others.size(); ++i) trimmed.push_back(i == index ? UniValue{} : others[i]);
            others = trimmed;
        }
        confirm::Call shown{call.node, call.wallet, call.method, others, call.fee_for};
        auto dialog{generic(shown)};
        if (!dialog) return dialog;
        if (value.isNull() || (value.isStr() && value.get_str().empty())) {
            if (remove_allowed && value.isStr()) dialog->rows.push_back(confirm::Text("Logo", "removed: the asset is shown by its initials"));
            return dialog;
        }
        std::string reason;
        const auto logo{ReadLogoParam(value, reason)};
        if (!logo) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this logo (%s): a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata", reason, bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES));
        for (auto& row : LogoRows(*logo)) dialog->rows.push_back(std::move(row));
        if (call.method == "setassetlogo" && call.fee_for) {
            // The logo goes whole into the transaction: about its size, and the inputs and outputs of an update.
            if (const auto fee{call.fee_for(static_cast<int64_t>(logo->size) + 260)}) dialog->rows.push_back(confirm::Amount("Network fee, about", *fee));
        }
        return dialog;
    };
}
} // namespace

namespace {
/** Whether text is plain ASCII (a display name beyond it is told: the ticker is what tells assets apart). */
bool PlainAscii(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f; });
}

/**
 * A describer that shows the names of an asset as rows of their own after the command: the name (`name_index`; "Ticker"
 * when it is one, from ticker_height every new name is) and the display name (`display_index`; "removed" when empty, if
 * `remove_allowed`), the other parameters as `generic` shows them. SIZE_MAX: no such parameter.
 */
std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> WithNames(std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> generic, size_t name_index, size_t display_index, bool remove_allowed)
{
    return [=](const confirm::Call& call) -> std::optional<Window::ConfirmDialog> {
        const UniValue name{name_index == SIZE_MAX ? UniValue{} : call.Arg(name_index)};
        const UniValue display{display_index == SIZE_MAX ? UniValue{} : call.Arg(display_index)};
        UniValue others{call.params};
        if (others.isArray()) {
            UniValue trimmed{UniValue::VARR};
            for (size_t i{0}; i < others.size(); ++i) trimmed.push_back(i == name_index || i == display_index ? UniValue{} : others[i]);
            others = trimmed;
        }
        confirm::Call shown{call.node, call.wallet, call.method, others, call.fee_for};
        auto dialog{generic(shown)};
        if (!dialog) return dialog;
        std::vector<dialog::Row> rows;
        if (name.isStr()) {
            const std::string& text{name.get_str()};
            rows.push_back(confirm::Text(bitassets::IsTicker(text) ? "Ticker" : "Name", text));
        }
        if (display.isStr()) {
            const std::string& text{display.get_str()};
            if (text.empty()) {
                if (remove_allowed) rows.push_back(confirm::Text("Display name", "removed: the asset is shown by its ticker alone"));
            } else {
                if (const auto why{bitassets::WhyNotDisplayName(text)}) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this display name: %s", *why));
                rows.push_back(confirm::Text("Display name", text));
                if (!PlainAscii(text)) rows.push_back(confirm::Note("The display name holds characters beyond plain ASCII. Display names are not unique: the ticker and the number tell assets apart."));
            }
        }
        if (call.method == "setassetdisplayname" && call.fee_for && display.isStr()) {
            // An update: its inputs and outputs, and the text.
            if (const auto fee{call.fee_for(static_cast<int64_t>(display.get_str().size()) + 260)}) rows.push_back(confirm::Amount("Network fee, about", *fee));
        }
        // After the command (the first rows: the wallet, then "Command").
        auto at{std::find_if(dialog->rows.begin(), dialog->rows.end(), [](const dialog::Row& r) { return r.label == "Command"; })};
        dialog->rows.insert(at == dialog->rows.end() ? dialog->rows.end() : at + 1, rows.begin(), rows.end());
        return dialog;
    };
}
} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // What the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        // Reserved and registered in one question: the registration, exactly as shown, is sent by the app without asking
        // again once the reservation is deep enough (confirm::Reveal).
        {"reserveasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Reserve an asset", "Reserve", "The wallet pays for a reservation of an asset name."), 0, SIZE_MAX, false), /*fee_unknown=*/true, 1, false, RegisterAfterReserve()}},
        {"registerasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(WithLogo(confirm::Generic("Register an asset", "Register", "The wallet registers an asset it reserved, with the supply shown."), 5, false), 0, 6, false), /*fee_unknown=*/true}},
        {"setassetlogo", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithLogo(confirm::Generic("Set the logo of an asset", "Set the logo", "The wallet puts this logo on the chain for an asset it controls, in place of the one before: everyone sees it where the asset is shown."), 1, true), /*fee_unknown=*/false, 2}},
        {"setassetdisplayname", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Set the display name of an asset", "Set the display name", "The wallet puts this display name on the chain for an asset it controls, in place of the one before: everyone sees it after the asset's ticker. Display names are not unique."), SIZE_MAX, 1, true), /*fee_unknown=*/false, 2}},
        {"releaseassetreservation", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release a reservation", "Release", "The wallet gives up a reservation of an asset name."), /*fee_unknown=*/true}},
        {"sendasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Send an asset", "Send", "The wallet sends units of an asset to the address shown.", {"address"}, {"amount"}), /*fee_unknown=*/true}},
        {"mintasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Mint an asset", "Mint", "The wallet mints more units of an asset it controls.", {"address"}, {"amount"}), /*fee_unknown=*/true}},
        {"burnasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Burn an asset", "Burn", "The wallet destroys units of an asset: they cannot come back.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"updateasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Update an asset", "Update", "The wallet signs a change of an asset it controls."), /*fee_unknown=*/true}},
        {"transferassetcontrol", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Transfer the control of an asset", "Transfer", "The wallet gives the control of an asset to the address shown: it cannot be taken back.", {"address"}, {}), /*fee_unknown=*/true}},
        {"fixassetsupply", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Fix the supply of an asset", "Fix the supply", "The wallet fixes the supply of an asset for good: no more can ever be minted."), /*fee_unknown=*/true}},
        {"releaseasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release an asset", "Release", "The wallet gives up an asset it controls."), /*fee_unknown=*/true}},
        {"swapasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Swap", "Swap", "The wallet trades an asset in a pool, for at least what is shown (min_out): refused, not signed for less, should the pool have moved.", {}, {"amount_in", "min_out"}), /*fee_unknown=*/true}},
        {"addliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Add liquidity", "Add", "The wallet puts both assets into a pool, for shares of it.", {}, {"amount_a", "amount_b"}), /*fee_unknown=*/true}},
        {"removeliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Remove liquidity", "Remove", "The wallet takes its shares out of a pool, for both assets."), /*fee_unknown=*/true}},
        {"createauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Create an auction", "Create", "The wallet puts units of an asset up for auction.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"bidauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Bid in an auction", "Bid", "The wallet pays a bid in an auction.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"collectauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Collect an auction", "Collect", "The wallet collects what an auction owes it."), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)keys;
    // The logo of an asset as a data: URL of the type the rules found (image/png or image/jpeg), for an <img> only; null when
    // it has none. Kept in memory (LogoCache), by asset and hash: the pages ask with the hash listassets gives.
    static LogoCache logos;
    bridge.RegisterHost("bitassets.logo", [node](const UniValue& params) {
        const UniValue& asset{Param(params, "asset")};
        const UniValue& hash{Param(params, "hash")};
        if (!asset.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "asset: the id of an asset");
        if (!hash.isNull() && (!hash.isStr() || hash.get_str().size() != 64 || !IsHex(hash.get_str()))) throw HostError(BRIDGE_INVALID_REQUEST, "hash: 64 hex digits");
        return logos.Get(node(), asset.get_str(), hash.isStr() ? hash.get_str() : std::string{});
    }, Bridge::Thread::WORKER);
    // A logo the user picked, as the rules and the confirmation will see it: {ok, reason?, mime, size, hash}. Read only.
    bridge.RegisterHost("bitassets.checkLogo", [](const UniValue& params) {
        std::string reason;
        const auto logo{ReadLogoParam(Param(params, "base64"), reason)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("ok", logo.has_value());
        if (!logo) {
            out.pushKV("reason", reason);
            return out;
        }
        out.pushKV("mime", logo->mime);
        out.pushKV("size", static_cast<uint64_t>(logo->size));
        out.pushKV("hash", logo->hash);
        return out;
    }, Bridge::Thread::WORKER);
    // The journal of the app: the assets being created (what to register once each reservation is deep enough), and
    // the prices of the pools as the app saw them; in a file of the data directory, with a sanity cap.
    bridge.RegisterHost("bitassets.store.load", [node](const UniValue&) {
        return ReadStore(StorePath(node(), JOURNAL_FILE));
    }, Bridge::Thread::LONG);
    bridge.RegisterHost("bitassets.store.save", [node](const UniValue& params) {
        WriteStore(StorePath(node(), JOURNAL_FILE), Param(params, "value"));
        return UniValue{true};
    }, Bridge::Thread::LONG);
    (void)confirm;
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local

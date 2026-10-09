// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <bitassets/state.h>

#include <arith_uint256.h>

#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <script/solver.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/check.h>

#include <algorithm>

namespace bitassets {

namespace {
template <typename T>
void WriteOperation(DataStream& s, const T& op)
{
    s << T::KIND << op;
}

template <typename T>
void ReadOperation(DataStream& s, std::optional<Operation>& out)
{
    T op;
    s >> op;
    out = std::move(op);
}

/** Marker outputs: the index, then a kind byte (a token kind, or RESULT_KIND), then the id and amount of a token. */
constexpr uint8_t RESULT_KIND{0xFF};

void WriteOutputs(DataStream& s, const std::vector<MarkerOutput>& outputs)
{
    WriteCompactSize(s, outputs.size());
    for (const MarkerOutput& out : outputs) {
        s << VARINT(out.n);
        if (!out.token) {
            s << RESULT_KIND;
            continue;
        }
        s << static_cast<uint8_t>(out.token->kind) << out.token->id;
        if (Token::Divisible(out.token->kind)) s << VARINT(out.token->amount);
    }
}

void ReadOutputs(DataStream& s, std::vector<MarkerOutput>& outputs)
{
    const uint64_t count{ReadCompactSize(s)};
    if (count > MAX_MARKER_OUTPUTS) throw std::ios_base::failure("too many outputs");
    outputs.clear();
    for (uint64_t i{0}; i < count; ++i) {
        MarkerOutput out;
        uint8_t kind;
        s >> VARINT(out.n) >> kind;
        if (kind != RESULT_KIND) {
            if (kind > static_cast<uint8_t>(Token::Kind::RECEIPT)) throw std::ios_base::failure("unknown token kind");
            Token& token{out.token.emplace()};
            token.kind = static_cast<Token::Kind>(kind);
            s >> token.id;
            token.amount = 1;
            if (Token::Divisible(token.kind)) s >> VARINT(token.amount);
        }
        outputs.push_back(out);
    }
}

bool GoodData(const AssetData& data)
{
    if (data.info && data.info->size() > MAX_INFO_SIZE) return false;
    if (data.encryption_key && !CPubKey{std::span{data.encryption_key->data(), data.encryption_key->size()}}.IsFullyValid()) return false;
    if (data.signing_key && !XOnlyPubKey{*data.signing_key}.IsFullyValid()) return false;
    return true;
}

bool GoodUpdates(const AssetUpdates& updates)
{
    AssetData set;
    if (updates.encryption_key.kind == UpdateKind::SET) set.encryption_key = updates.encryption_key.value;
    if (updates.signing_key.kind == UpdateKind::SET) set.signing_key = updates.signing_key.value;
    if (updates.info.kind == UpdateKind::SET) set.info = updates.info.value;
    return GoodData(set);
}

/** Calls `fn` for each field of the data of an asset: its number, and where it is in AssetData, AssetUpdates and AssetHistory. */
template <typename Fn>
void ForEachField(Fn&& fn)
{
    fn(DataField::COMMITMENT, &AssetData::commitment, &AssetUpdates::commitment, &AssetHistory::commitment);
    fn(DataField::IPV4, &AssetData::ipv4, &AssetUpdates::ipv4, &AssetHistory::ipv4);
    fn(DataField::IPV6, &AssetData::ipv6, &AssetUpdates::ipv6, &AssetHistory::ipv6);
    fn(DataField::ENCRYPTION_KEY, &AssetData::encryption_key, &AssetUpdates::encryption_key, &AssetHistory::encryption_key);
    fn(DataField::SIGNING_KEY, &AssetData::signing_key, &AssetUpdates::signing_key, &AssetHistory::signing_key);
    fn(DataField::INFO, &AssetData::info, &AssetUpdates::info, &AssetHistory::info);
}

template <typename T>
std::optional<T> AtHeightOf(const std::vector<Stamped<T>>& history, int height)
{
    std::optional<T> value;
    for (const Stamped<T>& entry : history) {
        if (entry.height > height) break;
        value = entry.value;
    }
    return value;
}

std::optional<std::vector<unsigned char>> MarkerData(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < sizeof(TAG) || !std::equal(std::begin(TAG), std::end(TAG), data.begin())) return std::nullopt;
    return data;
}

uint64_t Isqrt(unsigned __int128 n)
{
    if (n == 0) return 0;
    // Newton's method from above, in 128 bits.
    unsigned __int128 x{n}, y{(x + 1) / 2};
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return static_cast<uint64_t>(x);
}

/** a + b, if it is at most MAX_AMOUNT. */
std::optional<uint64_t> Add(uint64_t a, uint64_t b)
{
    if (a > MAX_AMOUNT || b > MAX_AMOUNT - a) return std::nullopt;
    return a + b;
}
} // namespace

bool AssetUpdates::Empty() const
{
    return commitment.kind == UpdateKind::RETAIN && ipv4.kind == UpdateKind::RETAIN && ipv6.kind == UpdateKind::RETAIN &&
           encryption_key.kind == UpdateKind::RETAIN && signing_key.kind == UpdateKind::RETAIN && info.kind == UpdateKind::RETAIN;
}

CScript MarkerScript(const Marker& marker)
{
    DataStream s{};
    s.write(std::as_bytes(std::span{TAG}));
    s << MARKER_VERSION;
    if (!marker.operation) {
        s << uint8_t{0};
    } else {
        std::visit([&](const auto& op) { WriteOperation(s, op); }, *marker.operation);
    }
    WriteOutputs(s, marker.outputs);
    return CScript() << OP_RETURN << std::vector<unsigned char>{UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

bool IsMarkerScript(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return false;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4) return false;
    return data.size() >= sizeof(TAG) && std::equal(std::begin(TAG), std::end(TAG), data.begin());
}

std::optional<Marker> ParseMarkerScript(const CScript& script)
{
    const auto data{MarkerData(script)};
    if (!data) return std::nullopt;
    try {
        DataStream s{std::span{*data}.subspan(sizeof(TAG))};
        uint8_t version, kind;
        s >> version >> kind;
        if (version != MARKER_VERSION) return std::nullopt;
        Marker marker;
        switch (kind) {
        case 0: break;
        case Reserve::KIND: ReadOperation<Reserve>(s, marker.operation); break;
        case Register::KIND: ReadOperation<Register>(s, marker.operation); break;
        case Mint::KIND: ReadOperation<Mint>(s, marker.operation); break;
        case UpdateAsset::KIND: ReadOperation<UpdateAsset>(s, marker.operation); break;
        case Burn::KIND: ReadOperation<Burn>(s, marker.operation); break;
        case Swap::KIND: ReadOperation<Swap>(s, marker.operation); break;
        case AddLiquidity::KIND: ReadOperation<AddLiquidity>(s, marker.operation); break;
        case RemoveLiquidity::KIND: ReadOperation<RemoveLiquidity>(s, marker.operation); break;
        case CreateAuction::KIND: ReadOperation<CreateAuction>(s, marker.operation); break;
        case Bid::KIND: ReadOperation<Bid>(s, marker.operation); break;
        case Collect::KIND: ReadOperation<Collect>(s, marker.operation); break;
        case ReleaseAsset::KIND: ReadOperation<ReleaseAsset>(s, marker.operation); break;
        default: return std::nullopt;
        }
        ReadOutputs(s, marker.outputs);
        if (!s.empty()) return std::nullopt;
        // Written back the same: there is one way to write a marker.
        if (MarkerScript(marker) != script) return std::nullopt;
        return marker;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error)
{
    std::optional<Marker> found;
    bool seen{false};
    for (const CTxOut& out : tx.vout) {
        if (!IsMarkerScript(out.scriptPubKey)) continue;
        if (seen) {
            if (error) *error = "bad-ba-markers";
            return std::nullopt;
        }
        seen = true;
        found = ParseMarkerScript(out.scriptPubKey);
        if (!found) {
            if (error) *error = "bad-ba-marker";
            return std::nullopt;
        }
    }
    if (error) error->clear();
    return found;
}

AssetId HashName(const std::string& name)
{
    uint256 hash;
    CSHA256().Write(UCharCast(name.data()), name.size()).Finalize(hash.data());
    return hash;
}

uint256 ReservationCommitment(const AssetId& name, const uint256& nonce, const CScript& script)
{
    uint256 commitment;
    CHMAC_SHA256(nonce.data(), nonce.size()).Write(name.data(), name.size()).Write(script.data(), script.size()).Finalize(commitment.data());
    return commitment;
}

uint256 PoolId(const AssetId& a, const AssetId& b)
{
    HashWriter writer{};
    writer << std::string{"BitAssets pool"} << std::min(a, b) << std::max(a, b);
    return writer.GetSHA256();
}

bool IsAssetName(const std::string& name)
{
    if (name.empty() || name.size() > MAX_NAME_TEXT_SIZE || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

bool ReadsAsAnotherAsset(const std::string& name)
{
    const auto lower{[](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }};
    if (name.size() == 3 && lower(name[0]) == 'c' && lower(name[1]) == 'h' && lower(name[2]) == 'n') return true;
    if (name.size() >= 2 && name[0] == '0' && lower(name[1]) == 'x') return true;
    // Groups of four digits, two or three of them.
    if (name.size() != 9 && name.size() != 14) return false;
    for (size_t i{0}; i < name.size(); ++i) {
        const bool dash{i % 5 == 4};
        if (dash ? name[i] != '-' : (name[i] < '0' || name[i] > '9')) return false;
    }
    return true;
}

namespace {
/** Whether an asset is CHN by name, in any case. */
bool IsChnName(const AssetId& asset)
{
    static const std::set<AssetId> chn{[] {
        std::set<AssetId> names;
        for (int mask{0}; mask < 8; ++mask) {
            std::string name{"chn"};
            for (int i{0}; i < 3; ++i) {
                if (mask & (1 << i)) name[i] = static_cast<char>(name[i] - 'a' + 'A');
            }
            names.insert(HashName(name));
        }
        return names;
    }()};
    return chn.contains(asset);
}

constexpr uint32_t SEQ_DISPLAY_OFFSET{23071990};
constexpr uint32_t SEQ_LOW{100000000};
} // namespace

std::string FormatSeq(uint32_t seq)
{
    const uint32_t hi{seq / SEQ_LOW};
    const uint32_t lo{(seq % SEQ_LOW + SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const std::string d{strprintf("%08u", lo)};
    const std::string first{d[4], d[3], d[1], d[6]};
    const std::string second{d[2], d[7], d[0], d[5]};
    if (hi > 0) return strprintf("%04u-%s-%s", hi, first, second);
    return first + "-" + second;
}

std::optional<uint32_t> ParseSeq(const std::string& text)
{
    std::vector<std::string> groups;
    size_t start{0};
    while (true) {
        const size_t end{std::min(text.find('-', start), text.size())};
        groups.push_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    if (groups.size() != 2 && groups.size() != 3) return std::nullopt;
    for (const std::string& group : groups) {
        if (group.size() != 4 || !std::all_of(group.begin(), group.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    }
    uint64_t hi{0};
    if (groups.size() == 3) hi = std::stoul(groups[0]);
    const std::string& first{groups[groups.size() - 2]};
    const std::string& second{groups[groups.size() - 1]};
    std::string d(8, '0');
    d[4] = first[0];
    d[3] = first[1];
    d[1] = first[2];
    d[6] = first[3];
    d[2] = second[0];
    d[7] = second[1];
    d[0] = second[2];
    d[5] = second[3];
    const uint64_t lo{std::stoul(d)};
    const uint64_t low{(lo + SEQ_LOW - SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const uint64_t seq{hi * SEQ_LOW + low};
    if (seq > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    if (groups.size() == 3 && hi == 0) return std::nullopt;
    return static_cast<uint32_t>(seq);
}

std::optional<AssetData> AssetHistory::AtHeight(int registered, int at) const
{
    if (at < registered) return std::nullopt;
    AssetData data;
    ForEachField([&](DataField, auto data_field, auto, auto history_field) { data.*data_field = AtHeightOf(this->*history_field, at); });
    return data;
}

uint64_t Auction::PriceAt(int height) const
{
    if (duration <= 1 || height <= start_height) return start_price;
    if (height >= EndHeight()) return end_price;
    // In a straight line, rounded up: the seller never gets less than the line.
    const unsigned __int128 drop{static_cast<unsigned __int128>(start_price - end_price) * static_cast<uint32_t>(height - start_height) / static_cast<uint32_t>(duration - 1)};
    return start_price - static_cast<uint64_t>(drop);
}

uint64_t Auction::BuysAt(int height, uint64_t quote_amount) const
{
    const uint64_t price{PriceAt(height)};
    if (price == 0) return 0;
    // Rounded down: the buyer never gets more than paid for.
    const unsigned __int128 units{static_cast<unsigned __int128>(quote_amount) * base_amount / price};
    return units > MAX_AMOUNT ? MAX_AMOUNT : static_cast<uint64_t>(units);
}

uint64_t Auction::CostOfRemaining(int height) const
{
    if (base_amount == 0) return 0;
    const unsigned __int128 cost{(static_cast<unsigned __int128>(remaining) * PriceAt(height) + base_amount - 1) / base_amount};
    return static_cast<uint64_t>(std::min<unsigned __int128>(cost, MAX_AMOUNT));
}

namespace amm {
uint64_t SwapOut(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in)
{
    if (reserve_in == 0 || reserve_out == 0 || amount_in == 0) return 0;
    // Uniswap v2: the fee stays in the pool, the product of the reserves never falls. In 256 bits:
    // amount (63 bits) times 997 (10 bits) times a reserve (63 bits) does not fit in 128.
    const arith_uint256 in_with_fee{arith_uint256{amount_in} * (1000 - SWAP_FEE_PER_MILLE)};
    const arith_uint256 out{in_with_fee * arith_uint256{reserve_out} / (arith_uint256{reserve_in} * 1000 + in_with_fee)};
    // Less than reserve_out, so it fits.
    return out.GetLow64();
}

std::optional<uint64_t> SwapIn(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_out)
{
    if (reserve_in == 0 || amount_out == 0 || amount_out >= reserve_out) return std::nullopt;
    const arith_uint256 in{arith_uint256{reserve_in} * arith_uint256{amount_out} * 1000 / (arith_uint256{reserve_out - amount_out} * (1000 - SWAP_FEE_PER_MILLE)) + 1};
    if (in > arith_uint256{MAX_AMOUNT}) return std::nullopt;
    return in.GetLow64();
}

std::optional<Deposit> Provide(const Pool& pool, uint64_t amount0, uint64_t amount1)
{
    if (amount0 == 0 || amount1 == 0 || amount0 > MAX_AMOUNT || amount1 > MAX_AMOUNT) return std::nullopt;
    if (Abandoned(pool)) {
        // As a new pool is made, with what it holds (the dust everyone left) merged in.
        const auto r0{Add(pool.reserve0, amount0)}, r1{Add(pool.reserve1, amount1)};
        if (!r0 || !r1) return std::nullopt;
        const uint64_t total{Isqrt(static_cast<unsigned __int128>(*r0) * *r1)};
        if (total <= MIN_LIQUIDITY) return std::nullopt;
        return Deposit{total - MIN_LIQUIDITY, amount0, amount1, /*opens=*/true};
    }
    if (pool.reserve0 == 0 || pool.reserve1 == 0) return std::nullopt;
    const unsigned __int128 s0{static_cast<unsigned __int128>(amount0) * pool.shares / pool.reserve0};
    const unsigned __int128 s1{static_cast<unsigned __int128>(amount1) * pool.shares / pool.reserve1};
    const unsigned __int128 shares{std::min(s0, s1)};
    if (shares == 0 || shares > MAX_AMOUNT) return std::nullopt;
    // What the shares are worth of a reserve, rounded up: at most the amount offered, as the shares
    // are at most what that amount gives.
    const auto worth{[&](uint64_t reserve) { return static_cast<uint64_t>((shares * reserve + pool.shares - 1) / pool.shares); }};
    Deposit deposit{static_cast<uint64_t>(shares), amount0, amount1, /*opens=*/false};
    if (s0 <= s1) {
        deposit.take1 = worth(pool.reserve1);
    } else {
        deposit.take0 = worth(pool.reserve0);
    }
    // CHN goes back by the coinbase: dust stays in the pool, for its providers.
    if (pool.asset0.IsNull() && amount0 - deposit.take0 < MIN_CHN_PAYOUT) deposit.take0 = amount0;
    if (pool.asset1.IsNull() && amount1 - deposit.take1 < MIN_CHN_PAYOUT) deposit.take1 = amount1;
    return deposit;
}

double PriceImpact(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in, uint64_t amount_out)
{
    if (reserve_in == 0 || reserve_out == 0 || amount_in == 0) return 100.0;
    const double impact{100.0 * (1.0 - (static_cast<double>(amount_out) / amount_in) / (static_cast<double>(reserve_out) / reserve_in))};
    return std::clamp(impact, 0.0, 100.0);
}

std::pair<uint64_t, uint64_t> Withdraw(const Pool& pool, uint64_t shares)
{
    if (pool.shares == 0 || shares > pool.shares) return {0, 0};
    return {static_cast<uint64_t>(static_cast<unsigned __int128>(pool.reserve0) * shares / pool.shares),
            static_cast<uint64_t>(static_cast<unsigned __int128>(pool.reserve1) * shares / pool.shares)};
}

std::pair<uint64_t, uint64_t> WithdrawPaid(const Pool& pool, uint64_t shares)
{
    auto [out0, out1]{Withdraw(pool, shares)};
    if (pool.asset0.IsNull() && out0 < MIN_CHN_PAYOUT) out0 = 0;
    if (pool.asset1.IsNull() && out1 < MIN_CHN_PAYOUT) out1 = 0;
    return {out0, out1};
}
} // namespace amm

bool Spendable(const CScript& script)
{
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(script, solutions)) {
    case TxoutType::PUBKEYHASH:
    case TxoutType::SCRIPTHASH:
    case TxoutType::WITNESS_V0_KEYHASH:
    case TxoutType::WITNESS_V0_SCRIPTHASH:
    case TxoutType::WITNESS_V1_TAPROOT:
        return true;
    default:
        return false;
    }
}

namespace {
/** The CHN destinations an operation names, if any. */
const CScript* ChnTo(const Operation& op)
{
    if (const auto* swap{std::get_if<Swap>(&op)}) return &swap->chn_to;
    if (const auto* remove{std::get_if<RemoveLiquidity>(&op)}) return &remove->chn_to;
    if (const auto* bid{std::get_if<Bid>(&op)}) return &bid->chn_to;
    if (const auto* collect{std::get_if<Collect>(&op)}) return &collect->chn_to;
    return nullptr;
}
} // namespace

bool CheckMarker(const CTransaction& tx, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    std::string error;
    const auto marker{GetMarker(tx, &error)};
    if (!error.empty()) return invalid(error.c_str());
    if (!marker) return true;
    if (tx.IsCoinBase()) return invalid("bad-ba-coinbase");
    size_t marker_index{0};
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (IsMarkerScript(tx.vout[i].scriptPubKey)) marker_index = i;
    }
    if (!MoneyRange(tx.vout[marker_index].nValue)) return invalid("bad-ba-marker-value");
    for (size_t i{0}; i < marker->outputs.size(); ++i) {
        const MarkerOutput& out{marker->outputs[i]};
        if (out.n >= tx.vout.size() || out.n == marker_index || (i > 0 && out.n <= marker->outputs[i - 1].n)) return invalid("bad-ba-outputs");
        // A token is carried by an output of no value that someone can spend.
        if (tx.vout[out.n].nValue != 0 || !Spendable(tx.vout[out.n].scriptPubKey)) return invalid("bad-ba-token-output");
        if (out.token) {
            if (out.token->amount == 0 || out.token->amount > MAX_AMOUNT) return invalid("bad-ba-amount");
            if (!Token::Divisible(out.token->kind) && out.token->amount != 1) return invalid("bad-ba-amount");
            // Only reservations and receipts made by the transaction itself are named by a null id.
            const bool may_be_new{out.token->kind == Token::Kind::RESERVATION || out.token->kind == Token::Kind::RECEIPT};
            if (out.token->id.IsNull() && !may_be_new) return invalid("bad-ba-token-id");
        }
    }
    // The CHN the marker burns go into a pool or an auction: only an operation that puts CHN in may carry any.
    if (!marker->operation) {
        if (tx.vout[marker_index].nValue != 0) return invalid("bad-ba-marker-value");
        return true;
    }
    const Operation& op{*marker->operation};
    if (const CScript* chn_to{ChnTo(op)}; chn_to && !chn_to->empty() && !Spendable(*chn_to)) return invalid("bad-ba-chn-to");
    const auto positive{[](uint64_t amount) { return amount > 0 && amount <= MAX_AMOUNT; }};
    if (const auto* reg{std::get_if<Register>(&op)}) {
        if (reg->supply > MAX_AMOUNT || reg->decimals > MAX_DECIMALS) return invalid("bad-ba-supply");
        if (!GoodData(reg->data)) return invalid("bad-ba-data");
        if (reg->text && (!IsAssetName(*reg->text) || HashName(*reg->text) != reg->name)) return invalid("bad-ba-name-text");
        if (reg->name.IsNull()) return invalid("bad-ba-name");
    } else if (const auto* mint{std::get_if<Mint>(&op)}) {
        if (!positive(mint->amount) || mint->asset.IsNull()) return invalid("bad-ba-amount");
    } else if (const auto* update{std::get_if<UpdateAsset>(&op)}) {
        if (update->asset.IsNull() || update->updates.Empty() || !GoodUpdates(update->updates)) return invalid("bad-ba-data");
    } else if (const auto* burn{std::get_if<Burn>(&op)}) {
        if (burn->tokens.empty() || burn->tokens.size() > MAX_BURNS) return invalid("bad-ba-burn");
        for (const Token& token : burn->tokens) {
            if (token.kind == Token::Kind::LP || token.kind == Token::Kind::RECEIPT || token.id.IsNull()) return invalid("bad-ba-burn");
            if (token.amount == 0 || token.amount > MAX_AMOUNT || (!Token::Divisible(token.kind) && token.amount != 1)) return invalid("bad-ba-burn");
        }
    } else if (const auto* swap{std::get_if<Swap>(&op)}) {
        if (swap->asset_in == swap->asset_out || !positive(swap->amount_in) || swap->min_out > MAX_AMOUNT) return invalid("bad-ba-swap");
    } else if (const auto* add{std::get_if<AddLiquidity>(&op)}) {
        if (add->asset_a == add->asset_b || !positive(add->amount_a) || !positive(add->amount_b) || add->min_shares > MAX_AMOUNT) return invalid("bad-ba-liquidity");
    } else if (const auto* remove{std::get_if<RemoveLiquidity>(&op)}) {
        if (remove->asset_a == remove->asset_b || !positive(remove->shares) || remove->min_a > MAX_AMOUNT || remove->min_b > MAX_AMOUNT) return invalid("bad-ba-liquidity");
    } else if (const auto* create{std::get_if<CreateAuction>(&op)}) {
        if (create->base == create->quote || !positive(create->base_amount)) return invalid("bad-ba-auction");
        if (!positive(create->end_price) || create->start_price < create->end_price || create->start_price > MAX_AMOUNT) return invalid("bad-ba-auction-price");
        if (create->duration < 1 || create->duration > MAX_AUCTION_DURATION || create->start_height < 0) return invalid("bad-ba-auction-duration");
    } else if (const auto* bid{std::get_if<Bid>(&op)}) {
        if (!positive(bid->quote_amount) || bid->min_base > MAX_AMOUNT) return invalid("bad-ba-bid");
    }
    return true;
}

std::set<uint32_t> TokenOutputs(const CTransaction& tx)
{
    std::set<uint32_t> outputs;
    const auto marker{GetMarker(tx)};
    if (!marker) return outputs;
    for (const MarkerOutput& out : marker->outputs) outputs.insert(out.n);
    return outputs;
}

struct State::Plan {
    std::vector<COutPoint> spent;
    std::vector<std::pair<COutPoint, Token>> created;
    std::vector<std::pair<AssetId, AssetRecord>> assets;
    //! Entries of the history of assets' data, added (keys and values).
    std::vector<std::pair<sidechain::StoreBytes, sidechain::StoreBytes>> history;
    std::optional<std::pair<Txid, uint256>> reservation_made;
    ReservationOrigin reservation_origin;
    std::vector<Txid> reservations_gone;
    std::optional<std::pair<uint32_t, AssetId>> seq;
    std::optional<std::pair<uint256, Pool>> pool;
    std::optional<std::pair<Txid, Auction>> auction;
    std::vector<CTxOut> payouts;
    std::vector<Result> results;
    //! Retiring an asset: the pools that go, and the CHN freed for mainchain miners.
    std::vector<uint256> pools_gone;
    std::optional<AssetId> asset_gone;
    CAmount released{0};
    //! An AddLiquidity: its two results (the shares, then what goes back) go to its two result
    //! outputs, the second carrying nothing if nothing, or CHN, goes back.
    bool returns{false};
    //! Whether the transaction is one of the assets (has a marker or spends tokens): it writes the layout.
    bool assets_tx{false};
};

namespace {
const sidechain::Table<COutPoint, Token> TOKENS{0x30};
const sidechain::Table<uint256, AssetRecord> ASSETS{0x31};
const sidechain::Table<uint256, uint256> RESERVATIONS{0x32};
const sidechain::Table<uint32_t, uint256> SEQ{0x33};
const sidechain::Table<uint256, Pool> POOLS{0x34};
const sidechain::Table<uint256, Auction> AUCTIONS{0x35};
const sidechain::Cell<uint32_t> NEXT_SEQ{0x37, 0};
constexpr uint8_t TOKENS_BY_ID{0x39};
constexpr uint8_t POOLS_BY_ASSET{0x3a};
constexpr uint8_t AUCTIONS_BY_ASSET{0x3b};
const sidechain::Table<uint256, ReservationOrigin> ORIGINS{0x3d};
constexpr uint8_t HISTORY{0x3e};
//! Missing: the layout before it was written down (1).
const sidechain::Cell<uint32_t> LAYOUT{0x3f, 1};

sidechain::StoreBytes Key2(uint8_t table, const uint256& a, const uint256& b)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, a);
    sidechain::KeyCodec<uint256>::Encode(key, b);
    return key;
}
/** The outputs that carry tokens of an id and a kind, in outpoint order, under this. */
sidechain::StoreBytes TokenIdPrefix(const uint256& id, Token::Kind kind)
{
    sidechain::StoreBytes key{TOKENS_BY_ID};
    sidechain::KeyCodec<uint256>::Encode(key, id);
    key.push_back(static_cast<uint8_t>(kind));
    return key;
}
sidechain::StoreBytes TokenIdKey(const Token& token, const COutPoint& outpoint)
{
    sidechain::StoreBytes key{TokenIdPrefix(token.id, token.kind)};
    sidechain::KeyCodec<COutPoint>::Encode(key, outpoint);
    return key;
}
sidechain::StoreBytes HistoryPrefix(const AssetId& asset, DataField field)
{
    sidechain::StoreBytes key{HISTORY};
    sidechain::KeyCodec<uint256>::Encode(key, asset);
    key.push_back(static_cast<uint8_t>(field));
    return key;
}
sidechain::StoreBytes HistoryKey(const AssetId& asset, DataField field, uint32_t n)
{
    sidechain::StoreBytes key{HistoryPrefix(asset, field)};
    sidechain::KeyCodec<uint32_t>::Encode(key, n);
    return key;
}
sidechain::StoreBytes Prefix(uint8_t table, const uint256& head)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, head);
    return key;
}
} // namespace

sidechain::StoreOverlay& State::Writable() const { return *Assert(m_overlay); }
std::optional<Token> State::GetToken(const COutPoint& outpoint) const { return TOKENS.Get(*m_view, outpoint); }
std::optional<AssetRecord> State::GetAsset(const AssetId& asset) const { return ASSETS.Get(*m_view, asset); }
bool State::HasAsset(const AssetId& asset) const { return ASSETS.Contains(*m_view, asset); }

AssetHistory State::GetHistory(const AssetId& asset) const
{
    AssetHistory history;
    ForEachField([&](DataField field, auto, auto, auto history_field) {
        auto& entries{history.*history_field};
        using Entry = typename std::decay_t<decltype(entries)>::value_type;
        m_view->ForEach(HistoryPrefix(asset, field), [&](const sidechain::StoreBytes&, const sidechain::StoreBytes& value) {
            entries.push_back(sidechain::DecodeValue<Entry>(value));
            return true;
        });
    });
    return history;
}

std::optional<AssetData> State::DataAt(const AssetId& asset, int height) const
{
    const auto record{GetAsset(asset)};
    if (!record || height < record->height) return std::nullopt;
    return GetHistory(asset).AtHeight(record->height, height);
}

bool StoreLayoutCurrent(const sidechain::StoreView& view)
{
    if (LAYOUT.Get(view) == STORE_LAYOUT) return true;
    // Written with the first change to the assets: a store without it has none, or has them in an earlier layout.
    for (uint8_t table{0x30}; table <= 0x3e; ++table) {
        const sidechain::StoreBytes prefix{table};
        if (view.Next(prefix, prefix)) return false;
    }
    return true;
}
std::optional<uint256> State::GetReservation(const Txid& id) const { return RESERVATIONS.Get(*m_view, id.ToUint256()); }
std::optional<ReservationOrigin> State::GetReservationOrigin(const Txid& id) const { return ORIGINS.Get(*m_view, id.ToUint256()); }
std::optional<Pool> State::GetPool(const uint256& id) const { return POOLS.Get(*m_view, id); }
std::optional<Auction> State::GetAuction(const Txid& id) const { return AUCTIONS.Get(*m_view, id.ToUint256()); }
uint32_t State::NextSeq() const { return NEXT_SEQ.Get(*m_view); }
std::optional<Pool> State::FindPool(const AssetId& a, const AssetId& b) const { return GetPool(PoolId(a, b)); }
void State::ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const { TOKENS.ForEach(*m_view, fn); }
void State::ForEachAsset(const std::function<bool(const AssetId&, const AssetRecord&)>& fn) const { ASSETS.ForEach(*m_view, fn); }
void State::ForEachPool(const std::function<bool(const uint256&, const Pool&)>& fn) const { POOLS.ForEach(*m_view, fn); }
void State::ForEachAuction(const std::function<bool(const Txid&, const Auction&)>& fn) const
{
    AUCTIONS.ForEach(*m_view, [&](const uint256& id, const Auction& a) { return fn(Txid::FromUint256(id), a); });
}
void State::ForEachReservation(const std::function<bool(const Txid&, const uint256&)>& fn) const
{
    RESERVATIONS.ForEach(*m_view, [&](const uint256& id, const uint256& c) { return fn(Txid::FromUint256(id), c); });
}

void State::SetToken(const COutPoint& outpoint, const std::optional<Token>& token)
{
    sidechain::StoreOverlay& out{Writable()};
    if (const auto old{GetToken(outpoint)}) out.Erase(TokenIdKey(*old, outpoint));
    if (token) {
        TOKENS.Put(out, outpoint, *token);
        out.Put(TokenIdKey(*token, outpoint), {});
    } else {
        TOKENS.Erase(out, outpoint);
    }
}

void State::SetReservation(const Txid& id, const std::optional<uint256>& commitment)
{
    sidechain::StoreOverlay& out{Writable()};
    if (commitment) {
        RESERVATIONS.Put(out, id.ToUint256(), *commitment);
    } else {
        RESERVATIONS.Erase(out, id.ToUint256());
    }
}

void State::Count(const AssetId& asset, uint32_t AssetRecord::*count, int delta)
{
    if (delta == 0 || asset.IsNull()) return;
    auto record{GetAsset(asset)};
    if (!record) return;
    uint32_t& n{(*record).*count};
    // Kept in step with the pools and auctions, it never goes below zero nor wraps.
    n = delta < 0 ? n - std::min<uint32_t>(n, -delta) : n + delta;
    ASSETS.Put(Writable(), asset, *record);
}

void State::SetPool(const uint256& id, const std::optional<Pool>& pool)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetPool(id)};
    // The count of each asset's pools that have providers.
    const int provided{(pool && !amm::Abandoned(*pool) ? 1 : 0) - (old && !amm::Abandoned(*old) ? 1 : 0)};
    if (provided != 0) {
        const Pool& either{pool ? *pool : *old};
        Count(either.asset0, &AssetRecord::provided_pools, provided);
        Count(either.asset1, &AssetRecord::provided_pools, provided);
    }
    if (old) {
        out.Erase(Key2(POOLS_BY_ASSET, old->asset0, id));
        out.Erase(Key2(POOLS_BY_ASSET, old->asset1, id));
    }
    if (pool) {
        POOLS.Put(out, id, *pool);
        out.Put(Key2(POOLS_BY_ASSET, pool->asset0, id), {});
        out.Put(Key2(POOLS_BY_ASSET, pool->asset1, id), {});
    } else {
        POOLS.Erase(out, id);
    }
}

void State::SetAuction(const Txid& id, const std::optional<Auction>& auction)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetAuction(id)};
    // The count of the auctions that hold some of each of its assets: of the asset the auction was
    // made with (one retired since, and its name registered again, is another asset).
    if (old || auction) {
        const Auction& either{auction ? *auction : *old};
        for (const auto& [asset, registration] : {std::pair{either.base, either.base_registration}, std::pair{either.quote, either.quote_registration}}) {
            const int delta{(auction && auction->Holds(asset) ? 1 : 0) - (old && old->Holds(asset) ? 1 : 0)};
            if (delta == 0) continue;
            if (const auto record{GetAsset(asset)}; record && record->registration == registration) Count(asset, &AssetRecord::holding_auctions, delta);
        }
    }
    // Only auctions not closed are indexed: the rules look for those alone.
    if (old && !old->closed) {
        out.Erase(Key2(AUCTIONS_BY_ASSET, old->base, id.ToUint256()));
        out.Erase(Key2(AUCTIONS_BY_ASSET, old->quote, id.ToUint256()));
    }
    if (auction) {
        AUCTIONS.Put(out, id.ToUint256(), *auction);
        if (!auction->closed) {
            out.Put(Key2(AUCTIONS_BY_ASSET, auction->base, id.ToUint256()), {});
            out.Put(Key2(AUCTIONS_BY_ASSET, auction->quote, id.ToUint256()), {});
        }
    } else {
        AUCTIONS.Erase(out, id.ToUint256());
    }
}

std::optional<COutPoint> State::FirstTokenOf(const uint256& id, Token::Kind kind) const
{
    // The index has the kind before the outpoint: the first entry under both is the one.
    const sidechain::StoreBytes prefix{TokenIdPrefix(id, kind)};
    const auto first{m_view->Next(prefix, prefix)};
    if (!first) return std::nullopt;
    std::span<const unsigned char> in{first->first};
    in = in.subspan(prefix.size());
    return sidechain::KeyCodec<COutPoint>::Decode(in);
}

std::vector<std::pair<uint256, Pool>> State::PoolsOf(const AssetId& asset) const
{
    std::vector<std::pair<uint256, Pool>> pools;
    m_view->ForEach(Prefix(POOLS_BY_ASSET, asset), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> in{key};
        in = in.subspan(33);
        const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
        pools.emplace_back(id, *Assert(GetPool(id)));
        return true;
    });
    return pools;
}

size_t State::PoolCount() const
{
    size_t n{0};
    ForEachPool([&](const uint256&, const Pool&) { ++n; return true; });
    return n;
}

std::vector<std::pair<uint32_t, Token>> State::SpentTokens(const CTransaction& tx) const
{
    std::vector<std::pair<uint32_t, Token>> spent;
    if (tx.IsCoinBase()) return spent;
    for (uint32_t i{0}; i < tx.vin.size(); ++i) {
        if (const auto token{GetToken(tx.vin[i].prevout)}) spent.emplace_back(i, *token);
    }
    return spent;
}

std::optional<AssetId> State::AssetOfSeq(uint32_t seq) const
{
    const auto asset{SEQ.Get(*m_view, seq)};
    if (!asset) return std::nullopt;
    // A retired asset keeps its number; the name may since belong to an asset with another number.
    // The number names the asset only while that asset has it.
    const auto record{GetAsset(*asset)};
    if (!record || record->seq != seq) return std::nullopt;
    return asset;
}

bool State::Releasable(const AssetId& asset, std::string* why) const
{
    const auto no{[&](const char* reason) {
        if (why) *why = reason;
        return false;
    }};
    const auto record{GetAsset(asset)};
    if (!record) return no("no such asset");
    if (!record->fixed) return no("its supply is not fixed: its control coin exists");
    // What carries it, through the index of outputs by what they carry: one look per kind.
    if (FirstTokenOf(asset, Token::Kind::ASSET)) return no("someone holds some of it");
    if (FirstTokenOf(asset, Token::Kind::CONTROL)) return no("its control coin exists");
    // An auction that sells something else for it and has taken none of it in holds none of it: it
    // does not keep it alive (else a 1-unit auction that never starts would, for ever). Bids into it
    // are refused once the asset is gone (MakePlan). The record counts the others (Auction::Holds):
    // one look, however many auctions quote it.
    if (record->holding_auctions > 0) return no("an auction of it is not collected");
    if (record->provided_pools > 0) return no("a pool of it has liquidity providers");
    return true;
}

std::optional<State::Plan> State::MakePlan(const CTransaction& tx, int height, std::string& reject_reason, int reveal_depth) const
{
    const auto invalid{[&](const char* reason) -> std::optional<Plan> {
        reject_reason = reason;
        return std::nullopt;
    }};
    if (!CheckMarker(tx, reject_reason)) return std::nullopt;
    const auto marker{GetMarker(tx)};
    const auto spent{SpentTokens(tx)};
    if (!marker && spent.empty()) return Plan{};
    // Spending tokens without a marker would destroy them.
    if (!marker) return invalid("bad-ba-tokens-lost");

    const Txid txid{tx.GetHash()};
    const Operation* op{marker->operation ? &*marker->operation : nullptr};
    CAmount chn_in{0};
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (IsMarkerScript(tx.vout[i].scriptPubKey)) chn_in = tx.vout[i].nValue;
    }

    // The books: for each token, what comes in (spent, created) less what goes out (outputs, taken in). They must balance.
    using Key = std::pair<Token::Kind, uint256>;
    std::map<Key, __int128> books;
    const auto credit{[&](Token::Kind kind, const uint256& id, uint64_t amount) { books[{kind, id}] += amount; }};
    const auto debit{[&](Token::Kind kind, const uint256& id, uint64_t amount) { books[{kind, id}] -= amount; }};
    for (const auto& [n, token] : spent) credit(token.kind, token.id, token.amount);
    const auto spends{[&](Token::Kind kind, const uint256& id) {
        return std::any_of(spent.begin(), spent.end(), [&](const auto& s) { return s.second.kind == kind && s.second.id == id; });
    }};
    // A token named by a null id is the transaction's own reservation or receipt.
    std::vector<std::pair<uint32_t, Token>> explicit_outputs;
    size_t result_outputs{0};
    for (const MarkerOutput& out : marker->outputs) {
        if (!out.token) {
            ++result_outputs;
            continue;
        }
        Token token{*out.token};
        if (token.id.IsNull()) token.id = txid.ToUint256();
        debit(token.kind, token.id, token.amount);
        explicit_outputs.emplace_back(out.n, token);
    }

    Plan plan;
    plan.assets_tx = true;
    for (const auto& [n, token] : spent) plan.spent.push_back(tx.vin[n].prevout);
    // CHN the operation takes in, which the marker burns.
    uint64_t chn_taken{0};
    // What the operation takes in of an asset (CHN or a token).
    const auto take{[&](const AssetId& asset, uint64_t amount) {
        if (asset.IsNull()) {
            chn_taken += amount;
        } else {
            debit(Token::Kind::ASSET, asset, amount);
        }
    }};
    // Pools and auctions trade CHN and registered assets.
    const auto tradable{[&](const AssetId& asset) { return asset.IsNull() || HasAsset(asset); }};
    // The registration of an asset now (null for CHN, and for an asset not registered).
    const auto registration{[&](const AssetId& asset) {
        if (asset.IsNull()) return Txid{};
        const auto record{GetAsset(asset)};
        return record ? record->registration : Txid{};
    }};

    if (const auto* reserve{op ? std::get_if<Reserve>(op) : nullptr}) {
        credit(Token::Kind::RESERVATION, txid.ToUint256(), 1);
        plan.reservation_made = std::make_pair(txid, reserve->commitment);
        // Where it is made: the output that carries it (the books below hold it to exactly one).
        plan.reservation_origin.height = height;
        for (const auto& [n, token] : explicit_outputs) {
            if (token.kind == Token::Kind::RESERVATION && token.id == txid.ToUint256()) plan.reservation_origin.script = tx.vout[n].scriptPubKey;
        }
    } else if (const auto* reg{op ? std::get_if<Register>(op) : nullptr}) {
        // Assets registered before, in this block included, are taken.
        if (HasAsset(reg->name)) return invalid("bad-ba-name-taken");
        // A name that reads as another asset (CHN, a number, "0x...") would be mistaken for it.
        if ((reg->text && ReadsAsAnotherAsset(*reg->text)) || IsChnName(reg->name)) return invalid("bad-ba-name-reserved");
        // A reservation is bound to the script of its output: nobody but its maker can have made it,
        // so it reveals the name whatever other reservations there are.
        std::optional<Txid> reservation;
        std::optional<ReservationOrigin> origin;
        for (const auto& [n, token] : spent) {
            if (token.kind != Token::Kind::RESERVATION) continue;
            const Txid id{Txid::FromUint256(token.id)};
            const auto found{GetReservation(id)};
            if (!found) continue;
            const auto made{GetReservationOrigin(id)};
            if (made && *found == ReservationCommitment(reg->name, reg->nonce, made->script)) {
                reservation = id;
                origin = made;
                break;
            }
        }
        if (!reservation) return invalid("bad-ba-no-reservation");
        // Only once the reservation is deep enough: whoever makes this block saw the name and the
        // nonce of every registration waiting for it, and could have reserved the name under a nonce
        // of its own, in this very block, to register it first.
        if (int64_t{height} - origin->height < reveal_depth) return invalid("bad-ba-reservation-too-young");
        debit(Token::Kind::RESERVATION, reservation->ToUint256(), 1);
        plan.reservations_gone.push_back(*reservation);
        credit(Token::Kind::CONTROL, reg->name, 1);
        if (reg->supply > 0) credit(Token::Kind::ASSET, reg->name, reg->supply);
        AssetRecord record;
        record.seq = NextSeq();
        record.registration = txid;
        record.height = height;
        if (reg->text) record.text = *reg->text;
        record.decimals = reg->decimals;
        record.supply = record.minted = reg->supply;
        record.data = reg->data;
        // Each field starts its history with its value at the registration, set or not.
        ForEachField([&](DataField field, auto data_field, auto, auto) {
            using T = typename std::decay_t<decltype(reg->data.*data_field)>::value_type;
            plan.history.emplace_back(HistoryKey(reg->name, field, 0), sidechain::EncodeValue(Stamped<T>{reg->data.*data_field, txid, height}));
            record.changes[static_cast<size_t>(field)] = 1;
        });
        plan.assets.emplace_back(reg->name, std::move(record));
        plan.seq = std::make_pair(NextSeq(), reg->name);
    } else if (const auto* mint{op ? std::get_if<Mint>(op) : nullptr}) {
        const auto found{GetAsset(mint->asset)};
        if (!found) return invalid("bad-ba-asset-unknown");
        if (!spends(Token::Kind::CONTROL, mint->asset)) return invalid("bad-ba-no-control");
        AssetRecord record{*found};
        const auto supply{Add(record.supply, mint->amount)};
        const auto minted{Add(record.minted, mint->amount)};
        if (!supply || !minted) return invalid("bad-ba-supply");
        record.supply = *supply;
        record.minted = *minted;
        credit(Token::Kind::ASSET, mint->asset, mint->amount);
        plan.assets.emplace_back(mint->asset, std::move(record));
    } else if (const auto* update{op ? std::get_if<UpdateAsset>(op) : nullptr}) {
        const auto found{GetAsset(update->asset)};
        if (!found) return invalid("bad-ba-asset-unknown");
        // The control coin of this asset, not of any.
        if (!spends(Token::Kind::CONTROL, update->asset)) return invalid("bad-ba-no-control");
        AssetRecord record{*found};
        // A field set or deleted gets a new value (none, if deleted), at the end of its history.
        ForEachField([&](DataField field, auto data_field, auto update_field, auto) {
            const auto& change{update->updates.*update_field};
            if (change.kind == UpdateKind::RETAIN) return;
            auto& value{record.data.*data_field};
            value = change.kind == UpdateKind::SET ? change.value : std::nullopt;
            using T = typename std::decay_t<decltype(value)>::value_type;
            uint32_t& n{record.changes[static_cast<size_t>(field)]};
            plan.history.emplace_back(HistoryKey(update->asset, field, n), sidechain::EncodeValue(Stamped<T>{value, txid, height}));
            ++n;
        });
        plan.assets.emplace_back(update->asset, std::move(record));
    } else if (const auto* burn{op ? std::get_if<Burn>(op) : nullptr}) {
        std::map<AssetId, AssetRecord> changed;
        for (const Token& token : burn->tokens) {
            debit(token.kind, token.id, token.amount);
            if (token.kind == Token::Kind::RESERVATION) {
                if (!GetReservation(Txid::FromUint256(token.id))) return invalid("bad-ba-burn");
                plan.reservations_gone.push_back(Txid::FromUint256(token.id));
                continue;
            }
            auto it{changed.find(token.id)};
            if (it == changed.end()) {
                const auto known{GetAsset(token.id)};
                if (!known) return invalid("bad-ba-asset-unknown");
                it = changed.emplace(token.id, *known).first;
            }
            if (token.kind == Token::Kind::CONTROL) {
                it->second.fixed = true;
            } else {
                if (token.amount > it->second.supply) return invalid("bad-ba-burn");
                it->second.supply -= token.amount;
                const auto burned{Add(it->second.burned, token.amount)};
                if (!burned) return invalid("bad-ba-burn");
                it->second.burned = *burned;
            }
        }
        for (auto& [id, record] : changed) plan.assets.emplace_back(id, std::move(record));
    } else if (const auto* swap{op ? std::get_if<Swap>(op) : nullptr}) {
        const auto found{FindPool(swap->asset_in, swap->asset_out)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        // A pool nobody provides liquidity to is closed: what went in could only buy dust, and stay there.
        if (amm::Abandoned(pool)) return invalid("bad-ba-pool-closed");
        const bool zero_in{pool.asset0 == swap->asset_in};
        uint64_t& reserve_in{zero_in ? pool.reserve0 : pool.reserve1};
        uint64_t& reserve_out{zero_in ? pool.reserve1 : pool.reserve0};
        const uint64_t out{amm::SwapOut(reserve_in, reserve_out, swap->amount_in)};
        if (out == 0 || out < swap->min_out || out >= reserve_out) return invalid("bad-ba-swap-price");
        if (swap->asset_out.IsNull() && out < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        const auto new_in{Add(reserve_in, swap->amount_in)};
        if (!new_in) return invalid("bad-ba-swap");
        reserve_in = *new_in;
        reserve_out -= out;
        uint64_t& volume{zero_in ? pool.volume0 : pool.volume1};
        volume = Add(volume, swap->amount_in).value_or(MAX_AMOUNT);
        ++pool.swaps;
        take(swap->asset_in, swap->amount_in);
        plan.results.push_back({swap->asset_out, out});
        plan.pool = std::make_pair(PoolId(swap->asset_in, swap->asset_out), pool);
    } else if (const auto* add{op ? std::get_if<AddLiquidity>(op) : nullptr}) {
        if (!tradable(add->asset_a) || !tradable(add->asset_b)) return invalid("bad-ba-asset-unknown");
        const uint256 id{PoolId(add->asset_a, add->asset_b)};
        Pool pool;
        if (const auto found{GetPool(id)}) {
            pool = *found;
        } else {
            pool.asset0 = std::min(add->asset_a, add->asset_b);
            pool.asset1 = std::max(add->asset_a, add->asset_b);
            pool.creation = txid;
            pool.height = height;
        }
        const bool a_first{pool.asset0 == add->asset_a};
        const uint64_t amount0{a_first ? add->amount_a : add->amount_b};
        const uint64_t amount1{a_first ? add->amount_b : add->amount_a};
        // The amounts are the most that goes in: at the pool's price, the rest back; or they open
        // the pool (amm::Provide). Whoever opens a pool first sets its price: a deposit made for
        // another price goes in at it, or gives fewer shares than it asks for and is refused.
        if (!Add(pool.reserve0, amount0) || !Add(pool.reserve1, amount1)) return invalid("bad-ba-liquidity");
        const auto deposit{amm::Provide(pool, amount0, amount1)};
        if (!deposit || deposit->shares < add->min_shares) return invalid("bad-ba-liquidity-price");
        if (deposit->opens) {
            // A pool opens, or reopens, with a deposit worth trading against, not dust.
            const uint64_t chn_after{pool.asset0.IsNull() ? pool.reserve0 + amount0 : pool.asset1.IsNull() ? pool.reserve1 + amount1 : MIN_OPEN_CHN};
            if (deposit->shares < MIN_OPEN_SHARES || chn_after < MIN_OPEN_CHN) return invalid("bad-ba-pool-too-small");
            // The MIN_LIQUIDITY shares nobody held (of the pool everyone left) give way to those of the new one.
            pool.shares = deposit->shares + MIN_LIQUIDITY;
        } else {
            const auto total{Add(pool.shares, deposit->shares)};
            if (!total) return invalid("bad-ba-liquidity");
            pool.shares = *total;
        }
        pool.reserve0 += deposit->take0;
        pool.reserve1 += deposit->take1;
        // All of what the transaction offers is taken in; what goes back is a result.
        take(add->asset_a, add->amount_a);
        take(add->asset_b, add->amount_b);
        const uint64_t back0{amount0 - deposit->take0}, back1{amount1 - deposit->take1};
        // Only the side that gives more shares has any back.
        if (back0 > 0 && back1 > 0) return invalid("bad-ba-liquidity");
        plan.results.push_back({id, deposit->shares});
        plan.results.push_back(back0 > 0 ? Result{pool.asset0, back0} : Result{pool.asset1, back1});
        plan.returns = true;
        plan.pool = std::make_pair(id, pool);
    } else if (const auto* remove{op ? std::get_if<RemoveLiquidity>(op) : nullptr}) {
        const uint256 id{PoolId(remove->asset_a, remove->asset_b)};
        const auto found{GetPool(id)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        // The shares nobody holds stay.
        if (remove->shares > pool.shares - std::min(pool.shares, MIN_LIQUIDITY)) return invalid("bad-ba-liquidity");
        // A side that rounds to nothing (or to CHN dust, which stays in the pool) is left out, so that
        // a small provider can still leave; one side must give something.
        const auto [out0, out1]{amm::WithdrawPaid(pool, remove->shares)};
        const bool a_first{pool.asset0 == remove->asset_a};
        const uint64_t out_a{a_first ? out0 : out1}, out_b{a_first ? out1 : out0};
        if (out_a == 0 && out_b == 0) return invalid("bad-ba-liquidity-price");
        if (out_a < remove->min_a || out_b < remove->min_b) return invalid("bad-ba-liquidity-price");
        pool.reserve0 -= out0;
        pool.reserve1 -= out1;
        pool.shares -= remove->shares;
        debit(Token::Kind::LP, id, remove->shares);
        if (out_a > 0) plan.results.push_back({remove->asset_a, out_a});
        if (out_b > 0) plan.results.push_back({remove->asset_b, out_b});
        plan.pool = std::make_pair(id, pool);
    } else if (const auto* create{op ? std::get_if<CreateAuction>(op) : nullptr}) {
        if (!tradable(create->base) || !tradable(create->quote)) return invalid("bad-ba-asset-unknown");
        if (create->start_height < height) return invalid("bad-ba-auction-started");
        // An auction that never starts would hold what it sells, and the asset it quotes, for ever.
        if (create->start_height > height + MAX_AUCTION_DELAY) return invalid("bad-ba-auction-delay");
        Auction auction;
        auction.base = create->base;
        auction.base_amount = create->base_amount;
        auction.quote = create->quote;
        auction.start_price = create->start_price;
        auction.end_price = create->end_price;
        auction.start_height = create->start_height;
        auction.duration = create->duration;
        auction.created = height;
        auction.remaining = create->base_amount;
        auction.base_registration = registration(create->base);
        auction.quote_registration = registration(create->quote);
        take(create->base, create->base_amount);
        // Its receipt: the transaction must output it.
        credit(Token::Kind::RECEIPT, txid.ToUint256(), 1);
        plan.auction = std::make_pair(txid, auction);
    } else if (const auto* bid{op ? std::get_if<Bid>(op) : nullptr}) {
        const auto found{GetAuction(bid->auction)};
        if (!found) return invalid("bad-ba-no-auction");
        Auction auction{*found};
        if (!auction.OpenAt(height)) return invalid("bad-ba-auction-closed");
        // Its assets as they were: one retired (an auction that took none of what it quotes does not
        // keep that from being retired), or retired and registered again, is not what it trades.
        // Collect needs no such rule: what it pays of the asset it quotes is what bids paid in, none.
        if ((registration(auction.base) != auction.base_registration || registration(auction.quote) != auction.quote_registration)) {
            return invalid("bad-ba-auction-asset-gone");
        }
        const uint64_t bought{auction.BuysAt(height, bid->quote_amount)};
        if (bought == 0 || bought > auction.remaining || bought < bid->min_base) return invalid("bad-ba-bid-price");
        if (auction.base.IsNull() && bought < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        const auto proceeds{Add(auction.proceeds, bid->quote_amount)};
        if (!proceeds) return invalid("bad-ba-bid");
        auction.remaining -= bought;
        auction.proceeds = *proceeds;
        ++auction.bids;
        take(auction.quote, bid->quote_amount);
        plan.results.push_back({auction.base, bought});
        plan.auction = std::make_pair(bid->auction, auction);
    } else if (const auto* collect{op ? std::get_if<Collect>(op) : nullptr}) {
        const auto found{GetAuction(collect->auction)};
        if (!found) return invalid("bad-ba-no-auction");
        Auction auction{*found};
        if (!auction.CollectableAt(height)) return invalid("bad-ba-auction-running");
        debit(Token::Kind::RECEIPT, collect->auction.ToUint256(), 1);
        // CHN dust is not paid out: it was burned when it went in.
        const auto paid{[&](const AssetId& asset, uint64_t amount) { return amount > 0 && !(asset.IsNull() && amount < MIN_CHN_PAYOUT); }};
        if (paid(auction.base, auction.remaining)) plan.results.push_back({auction.base, auction.remaining});
        if (paid(auction.quote, auction.proceeds)) plan.results.push_back({auction.quote, auction.proceeds});
        auction.remaining = 0;
        auction.proceeds = 0;
        auction.closed = true;
        plan.auction = std::make_pair(collect->auction, auction);
    }

    if (const auto* release{op ? std::get_if<ReleaseAsset>(op) : nullptr}) {
        std::string why;
        if (!Releasable(release->asset, &why)) return invalid("bad-ba-release-alive");
        plan.asset_gone = release->asset;
        std::map<AssetId, AssetRecord> others;
        // Its pools, in the order of their ids, through the index of pools by asset.
        std::vector<std::pair<uint256, Pool>> pools;
        m_view->ForEach(Prefix(POOLS_BY_ASSET, release->asset), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            std::span<const unsigned char> in{key};
            in = in.subspan(33);
            const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
            pools.emplace_back(id, *Assert(GetPool(id)));
            return true;
        });
        for (const auto& [id, pool] : pools) {
            plan.pools_gone.push_back(id);
            // What the pool holds of the other asset: CHN for mainchain miners, any other asset burned.
            const bool zero_is_it{pool.asset0 == release->asset};
            const AssetId& other{zero_is_it ? pool.asset1 : pool.asset0};
            const uint64_t left{zero_is_it ? pool.reserve1 : pool.reserve0};
            if (other.IsNull()) {
                plan.released += static_cast<CAmount>(left);
                if (!MoneyRange(plan.released)) return invalid("bad-ba-release");
            } else if (left > 0) {
                auto o{others.find(other)};
                if (o == others.end()) o = others.emplace(other, *Assert(GetAsset(other))).first;
                o->second.supply -= std::min(o->second.supply, left);
                o->second.burned = Add(o->second.burned, left).value_or(MAX_AMOUNT);
            }
        }
        for (auto& [id, record] : others) plan.assets.emplace_back(id, std::move(record));
    }

    // The CHN taken in are what the marker burns: no more, no less.
    if (static_cast<uint64_t>(chn_in) != chn_taken) return invalid("bad-ba-marker-value");

    // The results: CHN by the coinbase, tokens to the result outputs, in order; what each result
    // output carries (nothing, for one that takes CHN back from an AddLiquidity, or nothing back).
    std::vector<std::optional<Token>> result_tokens;
    if (plan.returns) {
        if (result_outputs != 2 || plan.results.size() != 2) return invalid("bad-ba-results");
        result_tokens.push_back(Token{Token::Kind::LP, plan.results[0].asset, plan.results[0].amount});
        const Result& back{plan.results[1]};
        if (back.amount == 0) {
            result_tokens.emplace_back();
        } else if (back.asset.IsNull()) {
            // Paid by the coinbase to the script of the second result output (AddLiquidity names no
            // other): CHN that went in, burned by the marker, comes out.
            size_t seen{0};
            for (const MarkerOutput& out : marker->outputs) {
                if (!out.token && seen++ == 1) plan.payouts.emplace_back(static_cast<CAmount>(back.amount), tx.vout[out.n].scriptPubKey);
            }
            result_tokens.emplace_back();
        } else {
            result_tokens.push_back(Token{Token::Kind::ASSET, back.asset, back.amount});
        }
    }
    for (const Result& result : plan.returns ? std::vector<Result>{} : plan.results) {
        if (result.asset.IsNull()) {
            const CScript* chn_to{op ? ChnTo(*op) : nullptr};
            if (!chn_to || chn_to->empty()) return invalid("bad-ba-chn-to");
            if (result.amount > static_cast<uint64_t>(MAX_MONEY)) return invalid("bad-ba-chn-amount");
            plan.payouts.emplace_back(static_cast<CAmount>(result.amount), *chn_to);
            continue;
        }
        // An AddLiquidity's results are above; the others are asset coins.
        result_tokens.push_back(Token{Token::Kind::ASSET, result.asset, result.amount});
    }
    if (result_tokens.size() != result_outputs) return invalid("bad-ba-results");
    for (const auto& [key, balance] : books) {
        if (balance != 0) return invalid(balance > 0 ? "bad-ba-tokens-lost" : "bad-ba-tokens-unbacked");
    }

    for (const auto& [n, token] : explicit_outputs) plan.created.emplace_back(COutPoint{txid, n}, token);
    size_t next_result{0};
    for (const MarkerOutput& out : marker->outputs) {
        if (out.token) continue;
        if (const auto& token{result_tokens[next_result++]}) plan.created.emplace_back(COutPoint{txid, out.n}, *token);
    }
    return plan;
}

bool State::CheckTx(const CTransaction& tx, int height, std::string& reject_reason, std::vector<Result>* results, int reveal_depth) const
{
    auto plan{MakePlan(tx, height, reject_reason, reveal_depth)};
    if (!plan) return false;
    if (results) *results = std::move(plan->results);
    return true;
}

bool State::ApplyTx(const CTransaction& tx, int height, std::vector<CTxOut>& payouts, std::string& reject_reason, CAmount* released, int reveal_depth)
{
    const auto plan{MakePlan(tx, height, reject_reason, reveal_depth)};
    if (!plan) return false;
    sidechain::StoreOverlay& out{Writable()};
    // In the order the rules have always applied a plan in.
    for (const COutPoint& outpoint : plan->spent) SetToken(outpoint, std::nullopt);
    for (const auto& [outpoint, token] : plan->created) SetToken(outpoint, token);
    if (plan->reservation_made) {
        SetReservation(plan->reservation_made->first, plan->reservation_made->second);
        ORIGINS.Put(out, plan->reservation_made->first.ToUint256(), plan->reservation_origin);
    }
    for (const Txid& id : plan->reservations_gone) {
        SetReservation(id, std::nullopt);
        ORIGINS.Erase(out, id.ToUint256());
    }
    // The layout of the assets in the store, with their first change (StoreLayoutCurrent).
    if (plan->assets_tx && LAYOUT.Get(out) != STORE_LAYOUT) LAYOUT.Put(out, STORE_LAYOUT);
    for (const auto& [id, record] : plan->assets) ASSETS.Put(out, id, record);
    for (const auto& [key, value] : plan->history) out.Put(key, value);
    if (plan->seq) {
        SEQ.Put(out, plan->seq->first, plan->seq->second);
        NEXT_SEQ.Put(out, NextSeq() + 1);
    }
    if (plan->pool) SetPool(plan->pool->first, plan->pool->second);
    for (const uint256& id : plan->pools_gone) SetPool(id, std::nullopt);
    // The name is free again; its number is not given again (lists skip it). Its history goes with it.
    if (plan->asset_gone) {
        ASSETS.Erase(out, *plan->asset_gone);
        std::vector<sidechain::StoreBytes> history;
        out.ForEach(Prefix(HISTORY, *plan->asset_gone), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            history.push_back(key);
            return true;
        });
        for (const sidechain::StoreBytes& key : history) out.Erase(key);
    }
    if (released) *released = plan->released;
    if (plan->auction) SetAuction(plan->auction->first, plan->auction->second);
    payouts.insert(payouts.end(), plan->payouts.begin(), plan->payouts.end());
    return true;
}

} // namespace bitassets

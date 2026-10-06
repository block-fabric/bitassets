// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Standalone genesis block miner (SHA-256d). Builds the same coinbase
// transaction as CreateGenesisBlock() in src/kernel/chainparams.cpp and
// searches for a nonce satisfying nBits.
//
//   g++ -O2 -pthread mine_genesis.cpp -lcrypto -o mine_genesis
//   ./mine_genesis "<timestamp message>" <nTime> <nBits hex> <reward sats> <output script hex> [threads] [start nonce]

#include <openssl/sha.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Bytes = std::vector<uint8_t>;

static void Sha256d(const uint8_t* data, size_t len, uint8_t out[32])
{
    // Streaming API: the one-shot SHA256() takes a global lock in OpenSSL 3.
    uint8_t tmp[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(tmp, &ctx);
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, tmp, 32);
    SHA256_Final(out, &ctx);
}

static void PutLE(Bytes& b, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back((v >> (8 * i)) & 0xff); }

static void PutCompact(Bytes& b, uint64_t n)
{
    if (n < 253) { b.push_back(n); }
    else if (n <= 0xffff) { b.push_back(253); PutLE(b, n, 2); }
    else { b.push_back(254); PutLE(b, n, 4); }
}

// Minimal data push, as CScript::operator<<(std::vector<unsigned char>) does.
static void PutPush(Bytes& b, const Bytes& data)
{
    if (data.size() < 0x4c) { b.push_back(data.size()); }
    else if (data.size() <= 0xff) { b.push_back(0x4c); b.push_back(data.size()); }
    else { b.push_back(0x4d); PutLE(b, data.size(), 2); }
    b.insert(b.end(), data.begin(), data.end());
}

static Bytes FromHex(const std::string& s)
{
    Bytes out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(strtoul(s.substr(i, 2).c_str(), nullptr, 16));
    return out;
}

static std::string RevHex(const uint8_t h[32])
{
    char buf[65];
    for (int i = 0; i < 32; ++i) snprintf(buf + 2 * i, 3, "%02x", h[31 - i]);
    return buf;
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s \"<message>\" <nTime> <nBits hex> <reward sats> <output script hex> [threads] [start nonce]\n", argv[0]);
        return 1;
    }
    const std::string msg = argv[1];
    const uint32_t nTime = strtoul(argv[2], nullptr, 10);
    const uint32_t nBits = strtoul(argv[3], nullptr, 16);
    const uint64_t reward = strtoull(argv[4], nullptr, 10);
    const Bytes out_script = FromHex(argv[5]);
    const unsigned threads = argc > 6 ? atoi(argv[6]) : 8;
    const uint64_t start = argc > 7 ? strtoull(argv[7], nullptr, 10) : 0;

    // scriptSig: << 486604799 << CScriptNum(4) << message
    Bytes script_sig{0x04, 0xff, 0xff, 0x00, 0x1d, 0x01, 0x04};
    PutPush(script_sig, Bytes(msg.begin(), msg.end()));

    Bytes tx;
    PutLE(tx, 1, 4);                       // version
    PutCompact(tx, 1);                     // vin count
    tx.insert(tx.end(), 32, 0);            // null prevout hash
    PutLE(tx, 0xffffffff, 4);              // prevout index
    PutCompact(tx, script_sig.size());
    tx.insert(tx.end(), script_sig.begin(), script_sig.end());
    PutLE(tx, 0xffffffff, 4);              // sequence
    PutCompact(tx, 1);                     // vout count
    PutLE(tx, reward, 8);
    PutCompact(tx, out_script.size());
    tx.insert(tx.end(), out_script.begin(), out_script.end());
    PutLE(tx, 0, 4);                       // locktime

    uint8_t merkle[32];
    Sha256d(tx.data(), tx.size(), merkle);

    // Target from compact bits (big-endian, 32 bytes)
    uint8_t target[32] = {0};
    const int exponent = nBits >> 24;
    const uint32_t mantissa = nBits & 0x007fffff;
    for (int i = 0; i < 3; ++i) {
        const int pos = 32 - exponent + i;
        if (pos >= 0 && pos < 32) target[pos] = (mantissa >> (8 * (2 - i))) & 0xff;
    }

    Bytes header;
    PutLE(header, 1, 4);                   // version
    header.insert(header.end(), 32, 0);    // prev block
    header.insert(header.end(), merkle, merkle + 32);
    PutLE(header, nTime, 4);
    PutLE(header, nBits, 4);
    PutLE(header, 0, 4);                   // nonce

    std::atomic<bool> found{false};
    std::atomic<uint64_t> result{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            uint8_t h[80];
            memcpy(h, header.data(), 80);
            uint8_t hash[32], be[32];
            for (uint64_t n = start + t; n <= 0xffffffffULL && !found; n += threads) {
                const uint32_t nonce = n;
                memcpy(h + 76, &nonce, 4);
                Sha256d(h, 80, hash);
                if (hash[31] != 0) continue; // cheap reject
                for (int i = 0; i < 32; ++i) be[i] = hash[31 - i];
                if (memcmp(be, target, 32) <= 0) {
                    if (!found.exchange(true)) result = nonce;
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (!found) { fprintf(stderr, "no nonce found; change nTime and retry\n"); return 2; }

    uint8_t h[80], hash[32];
    memcpy(h, header.data(), 80);
    const uint32_t nonce = result;
    memcpy(h + 76, &nonce, 4);
    Sha256d(h, 80, hash);
    printf("nTime=%u nNonce=%u nBits=0x%08x\nhash=%s\nmerkle=%s\n", nTime, nonce, nBits, RevHex(hash).c_str(), RevHex(merkle).c_str());
    return 0;
}

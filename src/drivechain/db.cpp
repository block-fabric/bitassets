// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/db.h>

#include <serialize.h>
#include <span.h>

#include <ios>
#include <memory>
#include <utility>

namespace drivechain {

namespace {

//! What a block needs to be taken back (BlockUndo), for the blocks a reorg can reach.
constexpr uint8_t DB_UNDO{'R'};
//! What a block did (BlockRecord), for every block.
constexpr uint8_t DB_EVENTS{'E'};
//! The sidechain database of a chainstate (StateRecord).
constexpr uint8_t DB_STATE{'s'};
//! An escrow change (Deposit), by DepositKey.
constexpr uint8_t DB_DEPOSIT{'D'};
//! Where an escrow change is (DepositKey), by sidechain and txid.
constexpr uint8_t DB_DEPOSIT_TXID{'T'};
constexpr uint8_t DB_FORMAT_VERSION{'v'};

//! Version of the snapshot of the sidechain database; a snapshot of another one is not read.
constexpr uint32_t STATE_VERSION{3};

//! Size of the batches Wipe erases with.
constexpr size_t WIPE_BATCH_BYTES{1 << 20};

/** Key of an escrow change; big endian so that the database orders the changes of a sidechain by position in the chain. */
struct DepositKey {
    SidechainId slot{0};
    uint32_t height{0};
    uint32_t tx_index{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_DEPOSIT);
        ser_writedata32be(s, slot);
        ser_writedata32be(s, height);
        ser_writedata32be(s, tx_index);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (ser_readdata8(s) != DB_DEPOSIT) throw std::ios_base::failure("not a deposit key");
        slot = ser_readdata32be(s);
        height = ser_readdata32be(s);
        tx_index = ser_readdata32be(s);
    }

    friend bool operator==(const DepositKey&, const DepositKey&) = default;
};

/** Key of the position of an escrow change by its transaction. */
struct DepositTxidKey {
    SidechainId slot{0};
    uint256 txid;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_DEPOSIT_TXID);
        ser_writedata32be(s, slot);
        s << txid;
    }
};

/** What is stored for every block. */
struct BlockRecord {
    BlockEvents events;
    //! The index entries the block added.
    std::vector<DepositKey> deposits;

    SERIALIZE_METHODS(BlockRecord, obj) { READWRITE(obj.events.closed, obj.events.proposed, obj.deposits); }
};

/** The snapshot of a sidechain database, with the version of its format. */
struct StateRecord {
    SidechainDB& scdb;

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint32_t version;
        s >> version;
        if (version != STATE_VERSION) throw std::ios_base::failure("unknown version of the sidechain database");
        s >> scdb;
    }
};

struct StateWriteRecord {
    const SidechainDB& scdb;

    template <typename Stream>
    void Serialize(Stream& s) const { s << STATE_VERSION << scdb; }
};

/** A key as it is, whatever it holds: it reads all that is left. */
struct RawKey {
    std::vector<std::byte> bytes;

    template <typename Stream>
    void Serialize(Stream& s) const { s.write(std::span{bytes}); }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(std::span{bytes});
    }
};

} // namespace

Database::Database(const DBParams& params) : m_db{params}
{
    // A new database is in the current format (an older one, which has data, is not marked as such).
    if (m_db.IsEmpty()) WriteFormatVersion();
}

bool Database::IsCurrentFormat() const
{
    uint32_t version{0};
    return m_db.Read(DB_FORMAT_VERSION, version) && version == FORMAT_VERSION;
}

void Database::WriteFormatVersion()
{
    m_db.Write(DB_FORMAT_VERSION, FORMAT_VERSION, /*fSync=*/true);
}

void Database::Wipe()
{
    // One pass of one iterator, which reads the database as it was when it was made, erasing in
    // batches as it goes: a database that grew for long would not fit in memory as one batch, and
    // starting over from the first key after each batch would step over the tombstones of all the
    // keys erased before it, every time.
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    CDBBatch batch{m_db};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        RawKey key;
        if (!it->GetKey(key)) break;
        batch.Erase(key);
        if (batch.ApproximateSize() >= WIPE_BATCH_BYTES) {
            m_db.WriteBatch(batch);
            batch.Clear();
        }
    }
    m_db.WriteBatch(batch, /*fSync=*/true);
}

bool Database::WriteBlock(const uint256& block_hash, int height, const BlockUndo& undo, const std::vector<Deposit>& deposits, bool keep_undo)
{
    CDBBatch batch{m_db};
    BlockRecord record;
    record.events.closed = undo.closed;
    record.events.proposed = undo.proposed;
    for (const Deposit& deposit : deposits) {
        const DepositKey key{deposit.slot, static_cast<uint32_t>(height), deposit.tx_index};
        batch.Write(key, deposit);
        batch.Write(DepositTxidKey{deposit.slot, deposit.tx->GetHash().ToUint256()}, key);
        record.deposits.push_back(key);
    }
    batch.Write(std::make_pair(DB_EVENTS, block_hash), record);
    if (keep_undo) {
        batch.Write(std::make_pair(DB_UNDO, block_hash), undo);
    } else {
        batch.Erase(std::make_pair(DB_UNDO, block_hash));
    }
    m_db.WriteBatch(batch);
    return true;
}

bool Database::ReadBlockUndo(const uint256& block_hash, BlockUndo& undo) const
{
    return m_db.Read(std::make_pair(DB_UNDO, block_hash), undo);
}

bool Database::HasBlockUndo(const uint256& block_hash) const
{
    return m_db.Exists(std::make_pair(DB_UNDO, block_hash));
}

void Database::EraseBlockUndo(const uint256& block_hash)
{
    m_db.Erase(std::make_pair(DB_UNDO, block_hash));
}

bool Database::ReadBlockEvents(const uint256& block_hash, BlockEvents& events) const
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_EVENTS, block_hash), record)) return false;
    events = std::move(record.events);
    return true;
}

bool Database::EraseBlockDeposits(const uint256& block_hash)
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_EVENTS, block_hash), record)) return false;
    if (record.deposits.empty()) return true;
    CDBBatch batch{m_db};
    for (const DepositKey& key : record.deposits) {
        Deposit deposit;
        if (m_db.Read(key, deposit)) {
            // The txid entry goes with it, unless it is the entry of the transaction in another block.
            const DepositTxidKey txid_key{deposit.slot, deposit.tx->GetHash().ToUint256()};
            DepositKey indexed;
            if (m_db.Read(txid_key, indexed) && indexed == key) batch.Erase(txid_key);
        }
        batch.Erase(key);
    }
    m_db.WriteBatch(batch);
    return true;
}

bool Database::WriteState(const std::string& chainstate, const SidechainDB& scdb)
{
    m_db.Write(std::make_pair(DB_STATE, chainstate), StateWriteRecord{scdb}, /*fSync=*/true);
    return true;
}

bool Database::ReadState(const std::string& chainstate, SidechainDB& scdb) const
{
    SidechainDB read;
    StateRecord record{read};
    if (!m_db.Read(std::make_pair(DB_STATE, chainstate), record)) {
        // Whatever was read before the failure is not left behind.
        scdb = SidechainDB{};
        return false;
    }
    scdb = std::move(read);
    return true;
}

std::vector<Deposit> Database::ListBlockDeposits(SidechainId slot, int height) const
{
    std::vector<Deposit> deposits;
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(DepositKey{slot, static_cast<uint32_t>(height), 0}); it->Valid(); it->Next()) {
        DepositKey key;
        if (!it->GetKey(key) || key.slot != slot || key.height != static_cast<uint32_t>(height)) break;
        Deposit deposit;
        if (!it->GetValue(deposit)) break;
        deposits.push_back(std::move(deposit));
    }
    return deposits;
}

std::optional<std::vector<Deposit>> Database::ListDeposits(SidechainId slot, const std::optional<uint256>& after, size_t count) const
{
    std::vector<Deposit> deposits;
    DepositKey start{slot, 0, 0};
    if (after) {
        // Straight to the change the caller knows, by its txid: not through all those before it.
        if (!m_db.Read(DepositTxidKey{slot, *after}, start)) return std::nullopt;
        Deposit known;
        if (!m_db.Read(start, known) || known.slot != slot || known.tx->GetHash().ToUint256() != *after) return std::nullopt;
    }
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    it->Seek(start);
    if (after && it->Valid()) it->Next();
    for (; it->Valid(); it->Next()) {
        DepositKey key;
        if (!it->GetKey(key) || key.slot != slot) break;
        Deposit deposit;
        if (!it->GetValue(deposit)) break;
        deposits.push_back(std::move(deposit));
        if (count != 0 && deposits.size() >= count) break;
    }
    return deposits;
}

} // namespace drivechain

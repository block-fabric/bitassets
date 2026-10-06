// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/cryptotoolsdialog.h>

#include <base58.h>
#include <bech32.h>
#include <crypto/ripemd160.h>
#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <crypto/sha512.h>
#include <hash.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/noderpc.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QVBoxLayout>

#include <vector>

namespace {
template <typename Hasher>
QString Digest(std::span<const unsigned char> data)
{
    unsigned char out[Hasher::OUTPUT_SIZE];
    Hasher{}.Write(data.data(), data.size()).Finalize(out);
    return QString::fromStdString(HexStr(out));
}

QPlainTextEdit* Output(QWidget* parent, const char* name)
{
    auto* output{new QPlainTextEdit(parent)};
    output->setObjectName(name);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setFont(GUIUtil::fixedPitchFont());
    return output;
}
} // namespace

CryptoToolsDialog::CryptoToolsDialog(QWidget* parent) : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint)
{
    setWindowTitle(tr("Crypto Tools"));
    auto* layout{new QVBoxLayout(this)};
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs);

    // Hash calculator
    auto* hash_tab{new QWidget(m_tabs)};
    auto* hash_layout{new QVBoxLayout(hash_tab)};
    hash_layout->addWidget(new QLabel(tr("Data to hash:"), hash_tab));
    m_hash_input = new QPlainTextEdit(hash_tab);
    m_hash_input->setObjectName("hashInput");
    hash_layout->addWidget(m_hash_input, 1);
    m_hash_hex = new QCheckBox(tr("The data is hexadecimal (hash the bytes it stands for)"), hash_tab);
    m_hash_hex->setObjectName("hashHex");
    hash_layout->addWidget(m_hash_hex);
    m_hash_output = Output(hash_tab, "hashOutput");
    hash_layout->addWidget(m_hash_output, 2);
    m_tabs->addTab(hash_tab, tr("Hash Calculator"));

    // Merkle tree
    auto* merkle_tab{new QWidget(m_tabs)};
    auto* merkle_layout{new QVBoxLayout(merkle_tab)};
    auto* merkle_top{new QHBoxLayout};
    m_merkle_block = new QLineEdit(merkle_tab);
    m_merkle_block->setObjectName("merkleBlock");
    m_merkle_block->setPlaceholderText(tr("Block height or hash"));
    auto* load{new QPushButton(tr("Load transactions of block"), merkle_tab)};
    load->setObjectName("merkleLoad");
    merkle_top->addWidget(m_merkle_block, 1);
    merkle_top->addWidget(load);
    merkle_layout->addLayout(merkle_top);
    merkle_layout->addWidget(new QLabel(tr("Transaction ids, one per line:"), merkle_tab));
    m_merkle_input = new QPlainTextEdit(merkle_tab);
    m_merkle_input->setObjectName("merkleInput");
    m_merkle_input->setFont(GUIUtil::fixedPitchFont());
    merkle_layout->addWidget(m_merkle_input, 1);
    m_merkle_output = Output(merkle_tab, "merkleOutput");
    merkle_layout->addWidget(m_merkle_output, 2);
    m_tabs->addTab(merkle_tab, tr("Merkle Tree"));

    // Address decoder
    auto* address_tab{new QWidget(m_tabs)};
    auto* address_layout{new QVBoxLayout(address_tab)};
    auto* address_top{new QHBoxLayout};
    m_address = new QLineEdit(address_tab);
    m_address->setObjectName("decodeAddress");
    m_address->setPlaceholderText(tr("Address, or any Base58Check or Bech32 string"));
    auto* decode{new QPushButton(tr("Decode"), address_tab)};
    address_top->addWidget(m_address, 1);
    address_top->addWidget(decode);
    address_layout->addLayout(address_top);
    m_address_output = Output(address_tab, "decodeOutput");
    address_layout->addWidget(m_address_output, 1);
    m_tabs->addTab(address_tab, tr("Address Decoder"));

    connect(m_hash_input, &QPlainTextEdit::textChanged, this, &CryptoToolsDialog::updateHashes);
    connect(m_hash_hex, &QCheckBox::toggled, this, &CryptoToolsDialog::updateHashes);
    connect(load, &QPushButton::clicked, this, &CryptoToolsDialog::loadBlock);
    connect(m_merkle_block, &QLineEdit::returnPressed, this, &CryptoToolsDialog::loadBlock);
    connect(m_merkle_input, &QPlainTextEdit::textChanged, this, &CryptoToolsDialog::updateMerkleTree);
    connect(decode, &QPushButton::clicked, this, &CryptoToolsDialog::decodeAddress);
    connect(m_address, &QLineEdit::returnPressed, this, &CryptoToolsDialog::decodeAddress);

    updateHashes();
    resize(820, 620);
    GUIUtil::handleCloseWindowShortcut(this);
}

void CryptoToolsDialog::showTab(Tab tab)
{
    m_tabs->setCurrentIndex(tab);
    GUIUtil::bringToFront(this);
}

void CryptoToolsDialog::updateHashes()
{
    std::vector<unsigned char> data;
    if (m_hash_hex->isChecked()) {
        const std::string hex{m_hash_input->toPlainText().remove(QRegularExpression{QStringLiteral("\\s")}).toStdString()};
        const auto bytes{TryParseHex<unsigned char>(hex)};
        if (!bytes) {
            m_hash_output->setPlainText(tr("This is not hexadecimal data."));
            return;
        }
        data = *bytes;
    } else {
        const QByteArray utf8{m_hash_input->toPlainText().toUtf8()};
        data.assign(utf8.begin(), utf8.end());
    }
    const uint256 sha256d{Hash(data)};
    QString text;
    text += QStringLiteral("SHA-256:           %1\n").arg(Digest<CSHA256>(data));
    text += QStringLiteral("SHA-256d:          %1\n").arg(QString::fromStdString(HexStr(sha256d)));
    text += QStringLiteral("SHA-256d (as id):  %1\n").arg(QString::fromStdString(sha256d.GetHex()));
    text += QStringLiteral("RIPEMD-160:        %1\n").arg(Digest<CRIPEMD160>(data));
    text += QStringLiteral("Hash160:           %1\n").arg(QString::fromStdString(HexStr(Hash160(data))));
    text += QStringLiteral("SHA-512:           %1\n").arg(Digest<CSHA512>(data));
    text += QStringLiteral("SHA-1:             %1\n").arg(Digest<CSHA1>(data));
    text += QStringLiteral("\n") + tr("Bytes hashed: %1. Block hashes and transaction ids are SHA-256d shown with the bytes reversed (\"as id\").").arg(data.size());
    m_hash_output->setPlainText(text);
}

void CryptoToolsDialog::loadBlock()
{
    QString error;
    QString hash{m_merkle_block->text().trimmed()};
    bool is_height;
    const int height{hash.toInt(&is_height)};
    if (is_height) {
        UniValue params(UniValue::VARR);
        params.push_back(height);
        const auto result{NodeRpc::Call(m_client_model, "getblockhash", params, error)};
        hash = result ? NodeRpc::Text(*result) : QString{};
    }
    std::optional<UniValue> block;
    if (!hash.isEmpty()) {
        UniValue params(UniValue::VARR);
        params.push_back(hash.toStdString());
        params.push_back(1);
        block = NodeRpc::Call(m_client_model, "getblock", params, error);
    }
    if (!block) {
        m_merkle_output->setPlainText(error);
        return;
    }
    QStringList txids;
    for (const UniValue& txid : (*block)["tx"].getValues()) txids << NodeRpc::Text(txid);
    m_merkle_input->setPlainText(txids.join('\n'));
    m_merkle_output->appendPlainText(QStringLiteral("\n") + tr("Merkle root in the block header: %1").arg(NodeRpc::Text((*block)["merkleroot"])));
}

void CryptoToolsDialog::updateMerkleTree()
{
    std::vector<uint256> level;
    const QStringList lines{m_merkle_input->toPlainText().split('\n', Qt::SkipEmptyParts)};
    for (const QString& line : lines) {
        const auto hash{uint256::FromHex(line.trimmed().toStdString())};
        if (!hash) {
            m_merkle_output->setPlainText(tr("\"%1\" is not a transaction id (64 hexadecimal characters).").arg(line.trimmed()));
            return;
        }
        level.push_back(*hash);
    }
    if (level.empty()) {
        m_merkle_output->clear();
        return;
    }
    QString text;
    for (int depth{0};; ++depth) {
        text += level.size() == 1 ? tr("Merkle root:") : depth == 0 ? tr("Level 0, the transaction ids (%1):").arg(level.size()) : tr("Level %1 (%2 hashes):").arg(depth).arg(level.size());
        text += '\n';
        for (const uint256& hash : level) text += QStringLiteral("  %1\n").arg(QString::fromStdString(hash.GetHex()));
        if (level.size() == 1) break;
        // A level with an odd number of hashes has its last hash paired with itself.
        if (level.size() % 2 == 1) level.push_back(level.back());
        std::vector<uint256> next;
        for (size_t i{0}; i < level.size(); i += 2) next.push_back(Hash(level[i], level[i + 1]));
        level = std::move(next);
        text += '\n';
    }
    m_merkle_output->setPlainText(text);
}

void CryptoToolsDialog::decodeAddress()
{
    const QString input{m_address->text().trimmed()};
    if (input.isEmpty()) return;
    QString text;

    QString error;
    UniValue params(UniValue::VARR);
    params.push_back(input.toStdString());
    if (const auto info{NodeRpc::Call(m_client_model, "validateaddress", params, error)}) {
        if ((*info)["isvalid"].get_bool()) {
            text += tr("A valid address of this network.") + '\n';
            for (const auto& key : info->getKeys()) {
                if (key == "isvalid" || key == "address") continue;
                text += QStringLiteral("  %1: %2\n").arg(QString::fromStdString(key), NodeRpc::Text((*info)[key]));
            }
        } else {
            text += tr("Not a valid address of this network: %1").arg(info->exists("error") ? NodeRpc::Text((*info)["error"]) : QString{}) + '\n';
        }
        text += '\n';
    }

    std::vector<unsigned char> payload;
    if (DecodeBase58Check(input.toStdString(), payload, 100) && !payload.empty()) {
        text += tr("Base58Check encoding:") + '\n';
        text += QStringLiteral("  %1 %2 (0x%3)\n").arg(tr("version byte:")).arg(payload[0]).arg(payload[0], 2, 16, QLatin1Char('0'));
        text += QStringLiteral("  %1 %2\n").arg(tr("payload:"), QString::fromStdString(HexStr(std::span{payload}.subspan(1))));
    }
    const auto bech{bech32::Decode(input.toStdString())};
    if (bech.encoding != bech32::Encoding::INVALID) {
        text += (bech.encoding == bech32::Encoding::BECH32M ? tr("Bech32m encoding:") : tr("Bech32 encoding:")) + '\n';
        text += QStringLiteral("  %1 %2\n").arg(tr("prefix:"), QString::fromStdString(bech.hrp));
        if (!bech.data.empty()) {
            text += QStringLiteral("  %1 %2\n").arg(tr("witness version:")).arg(bech.data[0]);
            std::vector<unsigned char> program;
            if (ConvertBits<5, 8, false>([&](unsigned char c) { program.push_back(c); }, bech.data.begin() + 1, bech.data.end())) {
                text += QStringLiteral("  %1 %2\n").arg(tr("witness program:"), QString::fromStdString(HexStr(program)));
            }
        }
    }
    if (text.isEmpty()) text = error;
    m_address_output->setPlainText(text);
}

// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/timestampdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
using NodeRpc::Args;
using NodeRpc::Text;

//! What the data of a timestamp starts with.
const QByteArray PREFIX{"STAMP"};
} // namespace

TimestampDialog::TimestampDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinimizeButtonHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Timestamp File"));
    auto* layout{new QVBoxLayout(this)};
    auto* intro{new QLabel(tr("Stamping a file publishes its fingerprint (SHA-256 hash) in a transaction, which proves to anyone, "
                              "at any later time, that the file existed when the block was mined. The file itself stays on "
                              "this computer. Stamping costs a transaction fee."), this)};
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form{new QFormLayout};
    auto* file_row{new QHBoxLayout};
    m_path = new QLineEdit(this);
    m_path->setObjectName("timestampPath");
    m_path->setReadOnly(true);
    auto* browse{new QPushButton(tr("Choose file…"), this)};
    file_row->addWidget(m_path, 1);
    file_row->addWidget(browse);
    form->addRow(tr("File:"), file_row);
    m_hash_label = new QLabel(QStringLiteral("-"), this);
    m_hash_label->setObjectName("timestampHash");
    m_hash_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hash_label->setFont(GUIUtil::fixedPitchFont());
    form->addRow(tr("Fingerprint:"), m_hash_label);
    m_blocks = new QSpinBox(this);
    m_blocks->setRange(1, 100000000);
    m_blocks->setValue(100000);
    m_blocks->setToolTip(tr("How far back to look for the stamp of the file."));
    form->addRow(tr("Blocks to search:"), m_blocks);
    layout->addLayout(form);

    auto* buttons{new QHBoxLayout};
    auto* stamp_button{new QPushButton(tr("Stamp this file"), this)};
    auto* verify_button{new QPushButton(tr("Find the stamp of this file"), this)};
    buttons->addWidget(stamp_button);
    buttons->addWidget(verify_button);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_result = new QPlainTextEdit(this);
    m_result->setObjectName("timestampResult");
    m_result->setReadOnly(true);
    layout->addWidget(m_result, 1);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path{QFileDialog::getOpenFileName(this, tr("Choose a file"))};
        if (!path.isEmpty()) setFile(path);
    });
    connect(stamp_button, &QPushButton::clicked, this, &TimestampDialog::stamp);
    connect(verify_button, &QPushButton::clicked, this, &TimestampDialog::verify);

    resize(720, 420);
    GUIUtil::handleCloseWindowShortcut(this);
}

QString TimestampDialog::Payload(const QByteArray& sha256)
{
    return QString::fromLatin1(QByteArray{PREFIX + sha256}.toHex());
}

void TimestampDialog::setFile(const QString& path)
{
    m_hash.clear();
    m_path->setText(path);
    m_result->clear();
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        m_hash_label->setText(tr("The file cannot be read."));
        return;
    }
    QCryptographicHash hasher{QCryptographicHash::Sha256};
    if (!hasher.addData(&file)) {
        m_hash_label->setText(tr("The file cannot be read."));
        return;
    }
    m_hash = hasher.result();
    m_hash_label->setText(QString::fromLatin1(m_hash.toHex()));
}

void TimestampDialog::stamp()
{
    if (m_hash.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Choose a file first."));
        return;
    }
    const auto wallet{m_wallet_name()};
    if (!wallet) {
        QMessageBox::information(this, windowTitle(), tr("Open or create a wallet first; stamping costs a transaction fee."));
        return;
    }
    if (QMessageBox::question(this, windowTitle(), tr("Publish the fingerprint of this file on the chain?")) != QMessageBox::Yes) return;

    UniValue output(UniValue::VOBJ);
    output.pushKV("data", Payload(m_hash).toStdString());
    UniValue outputs(UniValue::VARR);
    outputs.push_back(output);
    QString error;
    const auto result{NodeRpc::Call(m_client_model, "send", Args({outputs}), error, wallet)};
    if (!result || !result->exists("txid")) {
        QMessageBox::warning(this, windowTitle(), result ? tr("The wallet could not sign the transaction.") : error);
        return;
    }
    m_result->setPlainText(tr("The fingerprint was published in transaction\n%1\n\nThe stamp counts from the block that includes this transaction. "
                              "Keep the file exactly as it is: any change gives it another fingerprint.").arg(Text((*result)["txid"])));
}

void TimestampDialog::verify()
{
    if (m_hash.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Choose a file first."));
        return;
    }
    QString error;
    const auto tip{NodeRpc::Call(m_client_model, "getbestblockhash", Args({}), error)};
    if (!tip) return;
    const std::string payload{Payload(m_hash).toStdString()};

    QProgressDialog progress{tr("Searching the chain…"), tr("Stop"), 0, m_blocks->value(), this};
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);

    // The oldest stamp is the one that counts, so the search goes on after a find.
    QString found;
    int searched{0};
    QString hash{Text(*tip)};
    for (; searched < m_blocks->value() && !hash.isEmpty(); ++searched) {
        if (searched % 16 == 0) {
            progress.setValue(searched);
            QCoreApplication::processEvents();
            if (progress.wasCanceled()) break;
        }
        const auto block{NodeRpc::Call(m_client_model, "getblock", Args({hash.toStdString(), 2}), error)};
        if (!block) break;
        for (const UniValue& tx : (*block)["tx"].getValues()) {
            for (const UniValue& out : tx["vout"].getValues()) {
                const UniValue& script{out["scriptPubKey"]};
                if (Text(script["type"]) != "nulldata" || !script["hex"].get_str().ends_with(payload)) continue;
                found = tr("This file was stamped in block %1, mined on %2.\n\nTransaction: %3\nBlock: %4")
                            .arg(Text((*block)["height"]),
                                 QLocale().toString(QDateTime::fromSecsSinceEpoch((*block)["time"].getInt<int64_t>()), QLocale::LongFormat),
                                 Text(tx["txid"]), hash);
            }
        }
        hash = block->exists("previousblockhash") ? Text((*block)["previousblockhash"]) : QString{};
    }
    progress.setValue(m_blocks->value());
    if (found.isEmpty()) {
        found = tr("No stamp of this file was found in the last %1 blocks. Either it was never stamped, it was changed since, "
                   "or its stamp is not mined yet.").arg(searched);
    }
    m_result->setPlainText(found);
}

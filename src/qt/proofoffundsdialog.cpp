// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/proofoffundsdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>

#include <QApplication>
#include <QDate>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <set>

namespace {
using NodeRpc::Args;
using NodeRpc::Text;

const QString BEGIN{QStringLiteral("-----BEGIN CHAINS PROOF OF FUNDS-----")};
const QString SIGNATURES{QStringLiteral("-----SIGNATURES-----")};
const QString END{QStringLiteral("-----END CHAINS PROOF OF FUNDS-----")};
} // namespace

ProofOfFundsDialog::ProofOfFundsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Proof of Funds"));
    auto* layout{new QVBoxLayout(this)};
    auto* tabs{new QTabWidget(this)};
    layout->addWidget(tabs);

    auto* prove_tab{new QWidget(tabs)};
    auto* prove_layout{new QVBoxLayout(prove_tab)};
    auto* prove_intro{new QLabel(tr("Sign a statement with every address of the open wallet that holds coins. Whoever you give the "
                                    "proof to can check that the holder of those coins made the statement. It moves no coins and "
                                    "costs nothing, but it reveals which addresses are yours."), prove_tab)};
    prove_intro->setWordWrap(true);
    prove_layout->addWidget(prove_intro);
    auto* statement_row{new QHBoxLayout};
    m_statement = new QLineEdit(prove_tab);
    m_statement->setObjectName("proofStatement");
    m_statement->setText(tr("These funds are mine. %1").arg(QLocale().toString(QDate::currentDate(), QLocale::LongFormat)));
    auto* prove_button{new QPushButton(tr("Create proof"), prove_tab)};
    statement_row->addWidget(new QLabel(tr("Statement:"), prove_tab));
    statement_row->addWidget(m_statement, 1);
    statement_row->addWidget(prove_button);
    prove_layout->addLayout(statement_row);
    m_proof = new QPlainTextEdit(prove_tab);
    m_proof->setObjectName("proofOutput");
    m_proof->setReadOnly(true);
    m_proof->setFont(GUIUtil::fixedPitchFont());
    prove_layout->addWidget(m_proof, 1);
    auto* copy{new QPushButton(tr("Copy proof"), prove_tab)};
    prove_layout->addWidget(copy, 0, Qt::AlignLeft);
    tabs->addTab(prove_tab, tr("Prove"));

    auto* verify_tab{new QWidget(tabs)};
    auto* verify_layout{new QVBoxLayout(verify_tab)};
    verify_layout->addWidget(new QLabel(tr("Paste a proof to check it against the chain as it is now:"), verify_tab));
    m_input = new QPlainTextEdit(verify_tab);
    m_input->setObjectName("proofInput");
    m_input->setFont(GUIUtil::fixedPitchFont());
    verify_layout->addWidget(m_input, 1);
    auto* verify_button{new QPushButton(tr("Check proof"), verify_tab)};
    verify_layout->addWidget(verify_button, 0, Qt::AlignLeft);
    m_verdict = new QPlainTextEdit(verify_tab);
    m_verdict->setObjectName("proofVerdict");
    m_verdict->setReadOnly(true);
    verify_layout->addWidget(m_verdict, 1);
    tabs->addTab(verify_tab, tr("Verify"));

    connect(prove_button, &QPushButton::clicked, this, &ProofOfFundsDialog::prove);
    connect(copy, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_proof->toPlainText()); });
    connect(verify_button, &QPushButton::clicked, this, &ProofOfFundsDialog::verify);

    resize(820, 560);
    GUIUtil::handleCloseWindowShortcut(this);
}

void ProofOfFundsDialog::prove()
{
    const QString statement{m_statement->text().simplified()};
    if (statement.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Enter the statement to sign."));
        return;
    }
    const auto wallet{m_wallet_name()};
    if (!wallet) {
        QMessageBox::information(this, windowTitle(), tr("Open or create a wallet first."));
        return;
    }
    QString error;
    // Whoever checks the proof looks the coins up on the chain, so it does not matter here whether they are confirmed.
    const auto unspent{NodeRpc::Call(m_client_model, "listunspent", Args({0}), error, wallet)};
    if (!unspent) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    std::set<std::string> addresses;
    for (const UniValue& coin : unspent->getValues()) {
        if (coin.exists("address")) addresses.insert(coin["address"].get_str());
    }

    QString lines;
    int unsigned_addresses{0};
    QString last_error;
    for (const std::string& address : addresses) {
        const auto signature{NodeRpc::Call(m_client_model, "signmessage", Args({address, statement.toStdString()}), error, wallet)};
        if (signature) {
            lines += QStringLiteral("%1 %2\n").arg(QString::fromStdString(address), Text(*signature));
        } else {
            ++unsigned_addresses;
            last_error = error;
        }
    }
    if (lines.isEmpty()) {
        m_proof->clear();
        QMessageBox::warning(this, windowTitle(), addresses.empty() ? tr("The wallet holds no coins on an address.") : tr("None of the addresses could sign: %1").arg(last_error));
        return;
    }
    m_proof->setPlainText(BEGIN + '\n' + statement + '\n' + SIGNATURES + '\n' + lines + END + '\n');
    if (unsigned_addresses > 0) {
        QMessageBox::information(this, windowTitle(), tr("%1 address(es) holding coins are left out of the proof, because they cannot sign a message "
                                                         "(only addresses that stand for a single key can): %2").arg(unsigned_addresses).arg(last_error));
    }
}

void ProofOfFundsDialog::verify()
{
    const QStringList lines{m_input->toPlainText().split('\n', Qt::SkipEmptyParts)};
    const int begin{static_cast<int>(lines.indexOf(BEGIN))};
    const int signatures{static_cast<int>(lines.indexOf(SIGNATURES))};
    const int end{static_cast<int>(lines.indexOf(END))};
    if (begin < 0 || signatures != begin + 2 || end <= signatures) {
        m_verdict->setPlainText(tr("This is not a proof of funds."));
        return;
    }
    const std::string statement{lines[begin + 1].toStdString()};

    QString report;
    QString error;
    std::set<std::string> valid;
    int invalid{0};
    for (int i{signatures + 1}; i < end; ++i) {
        const QStringList parts{lines[i].simplified().split(' ')};
        const auto ok{parts.size() == 2 ? NodeRpc::Call(m_client_model, "verifymessage", Args({parts[0].toStdString(), parts[1].toStdString(), statement}), error) : std::nullopt};
        if (ok && ok->get_bool()) {
            valid.insert(parts[0].toStdString());
        } else {
            ++invalid;
            report += tr("INVALID signature for %1").arg(parts.value(0)) + '\n';
        }
    }

    QString total{QStringLiteral("0")};
    if (!valid.empty()) {
        UniValue descriptors(UniValue::VARR);
        for (const std::string& address : valid) descriptors.push_back("addr(" + address + ")");
        // Looking through all coins can take a while on a long chain.
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto scan{NodeRpc::Call(m_client_model, "scantxoutset", Args({"start", descriptors}), error)};
        QApplication::restoreOverrideCursor();
        if (!scan) {
            m_verdict->setPlainText(error);
            return;
        }
        total = Text((*scan)["total_amount"]);
        report += tr("Coins on these addresses now, at block %1: %2 in %3 output(s)").arg(Text((*scan)["height"]), total).arg((*scan)["unspents"].size()) + '\n';
    }
    const QString verdict{invalid == 0 && !valid.empty()
                              ? tr("VALID. The holder of %1 address(es), with %2 CHN on them now, signed this statement:").arg(valid.size()).arg(total)
                              : valid.empty() ? tr("NOT VALID. No signature of the proof is good. The statement was:")
                                              : tr("PARTLY VALID. %1 signature(s) are good, covering %2 CHN now, and %3 are not. The statement was:").arg(valid.size()).arg(total).arg(invalid)};
    m_verdict->setPlainText(verdict + "\n\n    " + QString::fromStdString(statement) + "\n\n" + report);
}

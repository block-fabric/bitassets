// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sidechainpage.h>

#include <core_io.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/util.h>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QTableWidgetItem* Item(const QString& text)
{
    auto* item{new QTableWidgetItem(text)};
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString Text(const UniValue& value)
{
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString::fromStdString(value.getValStr());
}

QString YesNo(const UniValue& value) { return value.get_bool() ? QObject::tr("yes") : QObject::tr("no"); }

} // namespace

SidechainPage::SidechainPage(const PlatformStyle* platform_style, QWidget* parent) : QWidget(parent)
{
    auto* layout{new QVBoxLayout(this)};

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("sidechainTabs");
    m_tabs->addTab(createSidechainsTab(), tr("Sidechains"));
    m_tabs->addTab(createProposalsTab(), tr("Proposals"));
    m_tabs->addTab(createWithdrawalsTab(), tr("Withdrawals"));
    layout->addWidget(m_tabs);

    auto* buttons{new QHBoxLayout()};
    auto* refresh_button{new QPushButton(tr("&Refresh"), this)};
    buttons->addStretch();
    buttons->addWidget(refresh_button);
    layout->addLayout(buttons);
    connect(refresh_button, &QPushButton::clicked, this, &SidechainPage::refresh);
}

QTableWidget* SidechainPage::createTable(const QStringList& headers, QWidget* parent)
{
    auto* table{new QTableWidget(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QString SidechainPage::selected(const QTableWidget* table, int column)
{
    const int row{table->currentRow()};
    if (row < 0 || !table->item(row, column)) return {};
    return table->item(row, column)->text();
}

QWidget* SidechainPage::createSidechainsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};

    m_sidechains = createTable({tr("Slot"), tr("Title"), tr("In escrow"), tr("Pending withdrawals"), tr("Active since block"), tr("Description")}, tab);
    m_sidechains->setObjectName("sidechains");
    layout->addWidget(m_sidechains);
    connect(m_sidechains, &QTableWidget::itemSelectionChanged, this, &SidechainPage::updateDepositTarget);

    auto* box{new QGroupBox(tr("Deposit to the selected sidechain"), tab)};
    auto* form{new QFormLayout(box)};
    m_deposit_target = new QLabel(box);
    m_deposit_destination = new QLineEdit(box);
    m_deposit_destination->setPlaceholderText(tr("The deposit address from the wallet of the sidechain: s<slot>_<address>_<checksum>"));
    m_deposit_destination->setMaxLength(static_cast<int>(drivechain::MAX_DEPOSIT_DESTINATION_SIZE));
    // A deposit address says which sidechain it is for: select it, or say what is wrong with the address.
    connect(m_deposit_destination, &QLineEdit::textChanged, this, [this](const QString& text) {
        drivechain::DepositAddress deposit_address;
        const auto kind{drivechain::ParseDepositAddress(text.trimmed().toStdString(), deposit_address)};
        m_deposit_destination->setStyleSheet(kind == drivechain::DepositAddressKind::INVALID ? QStringLiteral("QLineEdit { color: red; }") : QString{});
        m_deposit_destination->setToolTip(kind == drivechain::DepositAddressKind::INVALID ? tr("This deposit address has a typing error in it.") : QString{});
        if (kind != drivechain::DepositAddressKind::VALID) return;
        for (int row{0}; row < m_sidechains->rowCount(); ++row) {
            if (m_sidechains->item(row, 0)->text().toUInt() == deposit_address.slot) {
                m_sidechains->selectRow(row);
                return;
            }
        }
        m_deposit_destination->setToolTip(tr("This deposit address is for slot %1, which has no active sidechain.").arg(deposit_address.slot));
    });
    m_deposit_amount = new BitcoinAmountField(box);
    m_deposit_button = new QPushButton(tr("&Deposit"), box);
    m_deposit_button->setObjectName("depositButton");
    m_deposit_destination->setObjectName("depositDestination");
    m_deposit_amount->setObjectName("depositAmount");
    form->addRow(tr("Sidechain:"), m_deposit_target);
    form->addRow(tr("Destination:"), m_deposit_destination);
    form->addRow(tr("Amount:"), m_deposit_amount);
    form->addRow(QString(), m_deposit_button);
    layout->addWidget(box);
    connect(m_deposit_button, &QPushButton::clicked, this, &SidechainPage::deposit);

    updateDepositTarget();
    return tab;
}

QWidget* SidechainPage::createProposalsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};

    layout->addWidget(new QLabel(tr("Proposals in the chain. A proposal activates if enough blocks ack it; blocks this node mines ack the proposals marked below."), tab));
    m_proposals = createTable({tr("Slot"), tr("Title"), tr("Age"), tr("Acks"), tr("Failures"), tr("Blocks left"), tr("Replaces a sidechain"), tr("Acked by this node"), tr("Proposal hash")}, tab);
    m_proposals->setObjectName("proposals");
    layout->addWidget(m_proposals);
    auto* ack_buttons{new QHBoxLayout()};
    auto* ack_button{new QPushButton(tr("&Ack"), tab)};
    auto* nack_button{new QPushButton(tr("&Stop acking"), tab)};
    nack_button->setObjectName("nackButton");
    ack_buttons->addWidget(ack_button);
    ack_buttons->addWidget(nack_button);
    ack_buttons->addStretch();
    layout->addLayout(ack_buttons);
    connect(ack_button, &QPushButton::clicked, this, [this] { setAck(true); });
    connect(nack_button, &QPushButton::clicked, this, [this] { setAck(false); });

    layout->addWidget(new QLabel(tr("Proposals this node will make in the next block it mines:"), tab));
    m_queued = createTable({tr("Slot"), tr("Title"), tr("Description"), tr("Proposal hash")}, tab);
    m_queued->setObjectName("queuedProposals");
    m_queued->setMaximumHeight(110);
    layout->addWidget(m_queued);
    auto* queued_buttons{new QHBoxLayout()};
    auto* remove_button{new QPushButton(tr("Re&move"), tab)};
    queued_buttons->addWidget(remove_button);
    queued_buttons->addStretch();
    layout->addLayout(queued_buttons);
    connect(remove_button, &QPushButton::clicked, this, &SidechainPage::removeQueuedProposal);

    auto* box{new QGroupBox(tr("Propose a sidechain"), tab)};
    auto* form{new QFormLayout(box)};
    m_proposal_slot = new QSpinBox(box);
    m_proposal_slot->setRange(0, 511);
    m_proposal_title = new QLineEdit(box);
    m_proposal_title->setMaxLength(255);
    m_proposal_description = new QLineEdit(box);
    m_proposal_description->setMaxLength(1024);
    m_proposal_hash1 = new QLineEdit(box);
    m_proposal_hash1->setPlaceholderText(tr("64 hexadecimal characters (optional)"));
    m_proposal_hash1->setMaxLength(64);
    m_proposal_hash2 = new QLineEdit(box);
    m_proposal_hash2->setPlaceholderText(tr("40 hexadecimal characters (optional)"));
    m_proposal_hash2->setMaxLength(40);
    auto* propose_button{new QPushButton(tr("&Propose"), box)};
    propose_button->setObjectName("proposeButton");
    m_proposal_slot->setObjectName("proposalSlot");
    m_proposal_title->setObjectName("proposalTitle");
    m_proposal_description->setObjectName("proposalDescription");
    form->addRow(tr("Slot:"), m_proposal_slot);
    form->addRow(tr("Title:"), m_proposal_title);
    form->addRow(tr("Description:"), m_proposal_description);
    form->addRow(tr("Release tarball hash:"), m_proposal_hash1);
    form->addRow(tr("Build commit hash:"), m_proposal_hash2);
    form->addRow(QString(), propose_button);
    layout->addWidget(box);
    connect(propose_button, &QPushButton::clicked, this, &SidechainPage::propose);

    return tab;
}

QWidget* SidechainPage::createWithdrawalsTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};

    layout->addWidget(new QLabel(tr("Withdrawal bundles that miners are voting on. A bundle is paid out once its score reaches the minimum; upvoting one bundle of a sidechain downvotes its others."), tab));
    m_bundles = createTable({tr("Slot"), tr("Score"), tr("Blocks left"), tr("Payable"), tr("Transaction known"), tr("Vote of this node"), tr("Bundle hash")}, tab);
    m_bundles->setObjectName("bundles");
    layout->addWidget(m_bundles);

    auto* buttons{new QHBoxLayout()};
    auto* upvote{new QPushButton(tr("&Upvote"), tab)};
    upvote->setObjectName("upvoteButton");
    auto* downvote{new QPushButton(tr("&Downvote sidechain"), tab)};
    downvote->setObjectName("downvoteButton");
    auto* abstain{new QPushButton(tr("&Abstain"), tab)};
    auto* default_vote{new QPushButton(tr("Use de&fault"), tab)};
    buttons->addWidget(upvote);
    buttons->addWidget(downvote);
    buttons->addWidget(abstain);
    buttons->addWidget(default_vote);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(upvote, &QPushButton::clicked, this, [this] { setVote(QStringLiteral("upvote")); });
    connect(downvote, &QPushButton::clicked, this, [this] { setVote(QStringLiteral("downvote")); });
    connect(abstain, &QPushButton::clicked, this, [this] { setVote(QStringLiteral("abstain")); });
    connect(default_vote, &QPushButton::clicked, this, [this] { setVote(QStringLiteral("default")); });

    return tab;
}

void SidechainPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) return;
    connect(m_client_model, &ClientModel::numBlocksChanged, this, [this] {
        // Reloading takes a few RPC calls; only do it for a page someone is looking at.
        if (isVisible()) refresh();
    });
    if (OptionsModel* options{m_client_model->getOptionsModel()}) {
        m_deposit_amount->setDisplayUnit(options->getDisplayUnit());
        connect(options, &OptionsModel::displayUnitChanged, this, [this](BitcoinUnit unit) {
            m_deposit_amount->setDisplayUnit(unit);
            if (isVisible()) refresh();
        });
    }
}

void SidechainPage::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
}

void SidechainPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

std::optional<UniValue> SidechainPage::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    if (!m_client_model) return std::nullopt;
    std::string uri{"/"};
    if (wallet) {
        if (!m_wallet_model) return std::nullopt;
        uri = "/wallet/" + QString::fromUtf8(QUrl::toPercentEncoding(m_wallet_model->getWalletName())).toStdString();
    }
    QString error;
    try {
        return m_client_model->node().executeRpc(method, params, uri);
    } catch (const UniValue& rpc_error) {
        error = rpc_error.isObject() && rpc_error.exists("message") ? Text(rpc_error["message"]) : QString::fromStdString(rpc_error.write());
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
    }
    if (!quiet) QMessageBox::warning(this, tr("Sidechains"), error);
    return std::nullopt;
}

void SidechainPage::updateDepositTarget()
{
    const QString slot{selected(m_sidechains, 0)};
    const bool has_selection{!slot.isEmpty()};
    m_deposit_target->setText(has_selection ? tr("%1 (slot %2)").arg(selected(m_sidechains, 1), slot) : tr("Select a sidechain in the list above"));
    m_deposit_button->setEnabled(has_selection);
}

void SidechainPage::refresh()
{
    if (!m_client_model) return;
    const BitcoinUnit unit{m_client_model->getOptionsModel() ? m_client_model->getOptionsModel()->getDisplayUnit() : BitcoinUnit::BTC};
    const auto format_amount = [&](const UniValue& value) {
        return BitcoinUnits::formatWithUnit(unit, AmountFromValue(value), false, BitcoinUnits::SeparatorStyle::ALWAYS);
    };

    const UniValue no_params{UniValue::VARR};
    const auto info{call("getdrivechaininfo", no_params, /*wallet=*/false, /*quiet=*/true)};
    if (!info) {
        m_summary->setText(tr("The drivechain state is not available yet."));
        return;
    }
    m_min_score = (*info)["withdrawalminscore"].getInt<int>();
    m_proposal_slot->setMaximum((*info)["maxsidechains"].getInt<int>() - 1);
    m_summary->setText(tr("%1 of %2 sidechain slots in use, holding %3 in escrow. A proposal needs %4 blocks to activate and is rejected after %5 blocks without an ack. "
                          "A withdrawal has %6 blocks to reach a score of %7.")
                           .arg(Text((*info)["activesidechains"]), Text((*info)["maxsidechains"]), format_amount((*info)["escrowtotal"]),
                                Text((*info)["activationperiod"]), Text((*info)["activationmaxfailures"]),
                                Text((*info)["withdrawalperiod"]), Text((*info)["withdrawalminscore"])));

    // Keep the selection across the reload, by the content of a column.
    const auto reload = [](QTableWidget* table, int key_column, const std::function<void()>& fill) {
        const QString key{selected(table, key_column)};
        const QSignalBlocker blocker{table};
        table->setRowCount(0);
        fill();
        for (int row{0}; row < table->rowCount(); ++row) {
            if (!key.isEmpty() && table->item(row, key_column)->text() == key) table->setCurrentCell(row, 0);
        }
        table->resizeColumnsToContents();
        // Resizing to the contents undoes the stretching of the last column.
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setStretchLastSection(true);
    };

    if (const auto sidechains{call("listactivesidechains", no_params, false, true)}) {
        reload(m_sidechains, 0, [&] {
            for (const UniValue& sidechain : sidechains->getValues()) {
                const int row{m_sidechains->rowCount()};
                m_sidechains->insertRow(row);
                m_sidechains->setItem(row, 0, Item(Text(sidechain["slot"])));
                m_sidechains->setItem(row, 1, Item(Text(sidechain["title"])));
                m_sidechains->setItem(row, 2, Item(sidechain.exists("escrow") ? format_amount(sidechain["escrow"]["amount"]) : tr("no deposits yet")));
                m_sidechains->setItem(row, 3, Item(Text(sidechain["pendingbundles"])));
                m_sidechains->setItem(row, 4, Item(Text(sidechain["activationheight"])));
                m_sidechains->setItem(row, 5, Item(Text(sidechain["description"])));
            }
        });
        updateDepositTarget();
    }

    if (const auto proposals{call("listsidechainproposals", no_params, false, true)}) {
        reload(m_proposals, 8, [&] {
            for (const UniValue& proposal : (*proposals)["pending"].getValues()) {
                const int row{m_proposals->rowCount()};
                m_proposals->insertRow(row);
                m_proposals->setItem(row, 0, Item(Text(proposal["slot"])));
                m_proposals->setItem(row, 1, Item(Text(proposal["title"])));
                m_proposals->setItem(row, 2, Item(Text(proposal["age"])));
                m_proposals->setItem(row, 3, Item(Text(proposal["acks"])));
                m_proposals->setItem(row, 4, Item(Text(proposal["failures"])));
                m_proposals->setItem(row, 5, Item(Text(proposal["blocksleft"])));
                m_proposals->setItem(row, 6, Item(YesNo(proposal["replacement"])));
                m_proposals->setItem(row, 7, Item(YesNo(proposal["ack"])));
                m_proposals->setItem(row, 8, Item(Text(proposal["proposalhash"])));
            }
        });
        reload(m_queued, 3, [&] {
            for (const UniValue& proposal : (*proposals)["queued"].getValues()) {
                const int row{m_queued->rowCount()};
                m_queued->insertRow(row);
                m_queued->setItem(row, 0, Item(Text(proposal["slot"])));
                m_queued->setItem(row, 1, Item(Text(proposal["title"])));
                m_queued->setItem(row, 2, Item(Text(proposal["description"])));
                m_queued->setItem(row, 3, Item(Text(proposal["proposalhash"])));
            }
        });
    }

    if (const auto bundles{call("listwithdrawalbundles", no_params, false, true)}) {
        reload(m_bundles, 6, [&] {
            for (const UniValue& bundle : bundles->getValues()) {
                const int row{m_bundles->rowCount()};
                m_bundles->insertRow(row);
                m_bundles->setItem(row, 0, Item(Text(bundle["slot"])));
                m_bundles->setItem(row, 1, Item(tr("%1 of %2").arg(Text(bundle["score"])).arg(m_min_score)));
                m_bundles->setItem(row, 2, Item(Text(bundle["blocksleft"])));
                m_bundles->setItem(row, 3, Item(YesNo(bundle["payable"])));
                m_bundles->setItem(row, 4, Item(YesNo(bundle["known"])));
                m_bundles->setItem(row, 5, Item(Text(bundle["vote"])));
                m_bundles->setItem(row, 6, Item(Text(bundle["hash"])));
            }
        });
    }
}

void SidechainPage::deposit()
{
    if (!m_wallet_model) return;
    const QString slot{selected(m_sidechains, 0)};
    if (slot.isEmpty()) return;
    const QString destination{m_deposit_destination->text().trimmed()};
    if (destination.isEmpty()) {
        QMessageBox::warning(this, tr("Sidechains"), tr("Enter the deposit address that the wallet of the sidechain gives."));
        return;
    }
    drivechain::DepositAddress deposit_address;
    const auto kind{drivechain::ParseDepositAddress(destination.toStdString(), deposit_address)};
    if (kind == drivechain::DepositAddressKind::INVALID) {
        QMessageBox::warning(this, tr("Sidechains"), tr("This deposit address has a typing error in it. Copy it again from the wallet of the sidechain."));
        return;
    }
    if (kind == drivechain::DepositAddressKind::VALID && deposit_address.slot != slot.toUInt()) {
        QMessageBox::warning(this, tr("Sidechains"), tr("This deposit address is for the sidechain in slot %1, and the sidechain selected is the one in slot %2.").arg(deposit_address.slot).arg(slot));
        return;
    }
    // Without the checks of a deposit address, a destination the sidechain does not understand is lost there.
    if (kind == drivechain::DepositAddressKind::PLAIN &&
        QMessageBox::question(this, tr("Sidechains"), tr("\"%1\" is not a deposit address (s<slot>_<address>_<checksum>), so it cannot be checked. If the sidechain does not understand it, the coins are lost. Deposit anyway?").arg(destination),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    if (!m_deposit_amount->validate() || m_deposit_amount->value() <= 0) {
        QMessageBox::warning(this, tr("Sidechains"), tr("Enter the amount to deposit."));
        return;
    }
    const CAmount amount{m_deposit_amount->value()};
    const BitcoinUnit unit{m_client_model && m_client_model->getOptionsModel() ? m_client_model->getOptionsModel()->getDisplayUnit() : BitcoinUnit::BTC};
    const QString question{tr("Deposit %1 to the sidechain \"%2\" (slot %3), to be credited there to:\n\n%4\n\nCoins deposited to a sidechain can only come back through a withdrawal approved by the miners.")
                               .arg(BitcoinUnits::formatWithUnit(unit, amount), selected(m_sidechains, 1), slot, destination)};
    if (QMessageBox::question(this, tr("Confirm sidechain deposit"), question, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;

    WalletModel::UnlockContext unlock{m_wallet_model->requestUnlock()};
    if (!unlock.isValid()) return;

    UniValue params{UniValue::VARR};
    params.push_back(slot.toInt());
    params.push_back(destination.toStdString());
    params.push_back(ValueFromAmount(amount));
    if (const auto result{call("createsidechaindeposit", params, /*wallet=*/true)}) {
        QMessageBox::information(this, tr("Sidechains"), tr("Deposit sent in transaction %1.").arg(Text((*result)["txid"])));
        m_deposit_destination->clear();
        m_deposit_amount->clear();
        refresh();
    }
}

void SidechainPage::propose()
{
    UniValue params{UniValue::VARR};
    params.push_back(m_proposal_slot->value());
    params.push_back(m_proposal_title->text().trimmed().toStdString());
    params.push_back(m_proposal_description->text().trimmed().toStdString());
    const QString hash1{m_proposal_hash1->text().trimmed()};
    const QString hash2{m_proposal_hash2->text().trimmed()};
    params.push_back(hash1.isEmpty() ? std::string(64, '0') : hash1.toStdString());
    params.push_back(hash2.isEmpty() ? std::string(40, '0') : hash2.toStdString());
    if (call("createsidechainproposal", params)) {
        m_proposal_title->clear();
        m_proposal_description->clear();
        m_proposal_hash1->clear();
        m_proposal_hash2->clear();
        refresh();
    }
}

void SidechainPage::setAck(bool ack)
{
    const QString hash{selected(m_proposals, 8)};
    if (hash.isEmpty()) return;
    UniValue params{UniValue::VARR};
    params.push_back(hash.toStdString());
    params.push_back(ack);
    if (call("acksidechain", params)) refresh();
}

void SidechainPage::removeQueuedProposal()
{
    const QString hash{selected(m_queued, 3)};
    if (hash.isEmpty()) return;
    UniValue params{UniValue::VARR};
    params.push_back(hash.toStdString());
    if (call("removesidechainproposal", params)) refresh();
}

void SidechainPage::setVote(const QString& vote)
{
    const QString slot{selected(m_bundles, 0)};
    if (slot.isEmpty()) return;
    UniValue params{UniValue::VARR};
    params.push_back(slot.toInt());
    params.push_back(vote.toStdString());
    if (vote == QStringLiteral("upvote")) params.push_back(selected(m_bundles, 6).toStdString());
    if (call("setwithdrawalvote", params)) refresh();
}

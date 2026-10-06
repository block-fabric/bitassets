// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/miningdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr int REFRESH_INTERVAL_MS{2000};
const char* const SETTING_ADDRESS{"MiningAddress"};
const char* const SETTING_THREADS{"MiningThreads"};
} // namespace

MiningDialog::MiningDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinimizeButtonHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Solo Mine"));
    auto* layout{new QVBoxLayout(this)};

    auto* intro{new QLabel(tr("Mine blocks with the processor of this computer. Rewards are paid to the address below, "
                              "and can be spent once they have matured. Mining with a processor only finds blocks "
                              "while the difficulty of the network is low."), this)};
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* setup{new QGroupBox(tr("Miner"), this)};
    auto* setup_form{new QFormLayout(setup)};
    auto* address_row{new QHBoxLayout};
    m_address = new QLineEdit(setup);
    m_address->setObjectName("miningAddress");
    m_address->setPlaceholderText(tr("Address that receives the block rewards"));
    m_new_address = new QPushButton(tr("New address"), setup);
    m_new_address->setToolTip(tr("Take a new address from the open wallet"));
    address_row->addWidget(m_address);
    address_row->addWidget(m_new_address);
    setup_form->addRow(tr("Pay to:"), address_row);
    m_threads = new QSpinBox(setup);
    m_threads->setObjectName("miningThreads");
    m_threads->setRange(1, 1024);
    setup_form->addRow(tr("Threads:"), m_threads);
    auto* buttons{new QHBoxLayout};
    m_start = new QPushButton(tr("Start mining"), setup);
    m_start->setObjectName("miningStart");
    m_stop = new QPushButton(tr("Stop mining"), setup);
    m_stop->setObjectName("miningStop");
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addStretch();
    setup_form->addRow(buttons);
    layout->addWidget(setup);

    const auto label{[this](QFormLayout* form, const QString& title, const char* name) {
        auto* value{new QLabel(this)};
        value->setObjectName(name);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(title, value);
        return value;
    }};

    auto* miner{new QGroupBox(tr("This miner"), this)};
    auto* miner_form{new QFormLayout(miner)};
    m_status = label(miner_form, tr("Status:"), "miningStatus");
    m_hash_rate = label(miner_form, tr("Hash rate:"), "miningHashRate");
    m_expected = label(miner_form, tr("Expected time per block:"), "miningExpected");
    m_found = label(miner_form, tr("Blocks found:"), "miningFound");
    layout->addWidget(miner);

    auto* network{new QGroupBox(tr("Network"), this)};
    auto* network_form{new QFormLayout(network)};
    m_chain = label(network_form, tr("Chain:"), "miningChain");
    m_height = label(network_form, tr("Block height:"), "miningHeight");
    m_difficulty = label(network_form, tr("Difficulty of the next block:"), "miningDifficulty");
    m_network_rate = label(network_form, tr("Network hash rate:"), "miningNetworkRate");
    m_mempool = label(network_form, tr("Transactions waiting:"), "miningMempool");
    layout->addWidget(network);
    layout->addStretch();

    QSettings settings;
    m_address->setText(settings.value(SETTING_ADDRESS).toString());
    m_threads->setValue(settings.value(SETTING_THREADS, 1).toInt());

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MiningDialog::refresh);
    connect(m_start, &QPushButton::clicked, this, &MiningDialog::start);
    connect(m_stop, &QPushButton::clicked, this, &MiningDialog::stop);
    connect(m_new_address, &QPushButton::clicked, this, &MiningDialog::newAddress);

    resize(560, 480);
    GUIUtil::handleCloseWindowShortcut(this);
}

void MiningDialog::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) m_timer->stop();
}

void MiningDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
    // Keeps running while the window is hidden, as the calls are cheap and the miner may be running.
    m_timer->start(REFRESH_INTERVAL_MS);
}

void MiningDialog::newAddress()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) {
        QMessageBox::information(this, windowTitle(), tr("Open or create a wallet first, or enter an address."));
        return;
    }
    QString error;
    UniValue params(UniValue::VARR);
    params.push_back("mining");
    const auto address{NodeRpc::Call(m_client_model, "getnewaddress", params, error, wallet)};
    if (!address) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    m_address->setText(NodeRpc::Text(*address));
}

void MiningDialog::start()
{
    if (m_address->text().trimmed().isEmpty()) newAddress();
    const QString address{m_address->text().trimmed()};
    if (address.isEmpty()) return;

    QString error;
    UniValue params(UniValue::VARR);
    params.push_back(true);
    params.push_back(address.toStdString());
    params.push_back(m_threads->value());
    if (!NodeRpc::Call(m_client_model, "setgenerate", params, error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QSettings settings;
    settings.setValue(SETTING_ADDRESS, address);
    settings.setValue(SETTING_THREADS, m_threads->value());
    refresh();
}

void MiningDialog::stop()
{
    QString error;
    UniValue params(UniValue::VARR);
    params.push_back(false);
    if (!NodeRpc::Call(m_client_model, "setgenerate", params, error)) QMessageBox::warning(this, windowTitle(), error);
    refresh();
}

void MiningDialog::refresh()
{
    QString error;
    const auto generate{NodeRpc::Call(m_client_model, "getgenerate", UniValue{UniValue::VARR}, error)};
    const auto info{NodeRpc::Call(m_client_model, "getmininginfo", UniValue{UniValue::VARR}, error)};
    if (!generate || !info) {
        m_status->setText(error);
        return;
    }
    const bool running{(*generate)["generate"].get_bool()};
    const double rate{(*generate)["hashespersec"].get_real()};
    const double difficulty{(*info)["next"]["difficulty"].get_real()};

    m_threads->setMaximum(std::max<int>(1, (*generate)["cores"].getInt<int>()));
    m_start->setEnabled(!running);
    m_stop->setEnabled(running);
    m_address->setEnabled(!running);
    m_new_address->setEnabled(!running);
    m_threads->setEnabled(!running);
    if (running) m_threads->setValue((*generate)["threads"].getInt<int>());

    m_status->setText(running ? tr("Mining, threads: %1").arg((*generate)["threads"].getInt<int>()) : tr("Not mining"));
    m_hash_rate->setText(running ? NodeRpc::HashRate(rate) : QStringLiteral("-"));
    if (running && rate > 0) {
        // A block takes difficulty * 2^32 hashes on average.
        const double seconds{difficulty * 4294967296.0 / rate};
        m_expected->setText(seconds < 1 ? tr("less than a second") : GUIUtil::formatDurationStr(std::chrono::seconds{static_cast<int64_t>(std::min(seconds, 1e15))}));
    } else {
        m_expected->setText(QStringLiteral("-"));
    }
    const uint64_t rejected{(*generate)["blocksrejected"].getInt<uint64_t>()};
    m_found->setText(rejected == 0 ? NodeRpc::Text((*generate)["blocksfound"]) : tr("%1 (and %2 not accepted)").arg(NodeRpc::Text((*generate)["blocksfound"])).arg(rejected));

    m_chain->setText(NodeRpc::Text((*info)["chain"]));
    m_height->setText(NodeRpc::Text((*info)["blocks"]));
    m_difficulty->setText(QString::number(difficulty, 'g', 10));
    m_network_rate->setText(NodeRpc::HashRate((*info)["networkhashps"].get_real()));
    m_mempool->setText(NodeRpc::Text((*info)["pooledtx"]));
}

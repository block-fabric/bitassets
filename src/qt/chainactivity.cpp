// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/chainactivity.h>

#include <qt/clientmodel.h>
#include <qt/noderpc.h>

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

constexpr int ROWS{8};
constexpr int REFRESH_DELAY_MS{1000};

QTableWidget* Table(const QStringList& headers, QWidget* parent, const char* name)
{
    auto* table{new QTableWidget(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setToolTip(QObject::tr("Double-click a row to see it in the block explorer."));
    return table;
}

QString Time(const UniValue& time)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(time.getInt<int64_t>()), QLocale::ShortFormat);
}

QWidget* Titled(const QString& title, QTableWidget* table, QWidget* parent)
{
    auto* box{new QWidget(parent)};
    auto* layout{new QVBoxLayout(box)};
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label{new QLabel(QStringLiteral("<b>%1</b>").arg(title), box)};
    layout->addWidget(label);
    layout->addWidget(table);
    return box;
}
} // namespace

ChainActivity::ChainActivity(QWidget* parent) : QWidget(parent)
{
    auto* layout{new QHBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    m_transactions = Table({tr("Received"), tr("Fee"), tr("Size"), tr("Transaction id")}, this, "latestTransactions");
    m_blocks = Table({tr("Height"), tr("Time"), tr("Transactions"), tr("Hash")}, this, "latestBlocks");
    layout->addWidget(Titled(tr("Latest transactions"), m_transactions, this), 1);
    layout->addWidget(Titled(tr("Latest blocks"), m_blocks, this), 1);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ChainActivity::refresh);
    connect(m_blocks, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { Q_EMIT detailsRequested(m_blocks->item(row, 3)->text()); });
    connect(m_transactions, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { Q_EMIT detailsRequested(m_transactions->item(row, 3)->text()); });
}

void ChainActivity::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) {
        m_timer->stop();
        return;
    }
    const auto schedule{[this] {
        if (isVisible() && !m_timer->isActive()) m_timer->start(REFRESH_DELAY_MS);
    }};
    connect(client_model, &ClientModel::numBlocksChanged, this, schedule);
    connect(client_model, &ClientModel::mempoolSizeChanged, this, schedule);
}

void ChainActivity::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void ChainActivity::refresh()
{
    if (!m_client_model) return;
    QString error;

    int row{0};
    auto hash{NodeRpc::Call(m_client_model, "getbestblockhash", Args({}), error)};
    QString next{hash ? Text(*hash) : QString{}};
    m_blocks->setRowCount(ROWS);
    for (; row < ROWS && !next.isEmpty(); ++row) {
        const auto header{NodeRpc::Call(m_client_model, "getblockheader", Args({next.toStdString()}), error)};
        if (!header) break;
        m_blocks->setItem(row, 0, Item(Text((*header)["height"])));
        m_blocks->setItem(row, 1, Item(Time((*header)["time"])));
        m_blocks->setItem(row, 2, Item(Text((*header)["nTx"])));
        m_blocks->setItem(row, 3, Item(next));
        next = header->exists("previousblockhash") ? Text((*header)["previousblockhash"]) : QString{};
    }
    m_blocks->setRowCount(row);
    m_blocks->resizeColumnsToContents();

    const auto mempool{NodeRpc::Call(m_client_model, "getrawmempool", Args({true}), error)};
    if (!mempool) return;
    // The newest first.
    std::vector<std::pair<int64_t, std::string>> newest;
    for (const std::string& txid : mempool->getKeys()) newest.emplace_back((*mempool)[txid]["time"].getInt<int64_t>(), txid);
    const size_t rows{std::min<size_t>(newest.size(), ROWS)};
    std::partial_sort(newest.begin(), newest.begin() + rows, newest.end(), std::greater<>{});
    m_transactions->setRowCount(rows);
    for (size_t i{0}; i < rows; ++i) {
        const UniValue& entry{(*mempool)[newest[i].second]};
        m_transactions->setItem(i, 0, Item(Time(entry["time"])));
        m_transactions->setItem(i, 1, Item(QString::number(entry["fees"]["base"].get_real(), 'f', 8)));
        m_transactions->setItem(i, 2, Item(Text(entry["vsize"])));
        m_transactions->setItem(i, 3, Item(QString::fromStdString(newest[i].second)));
    }
    m_transactions->resizeColumnsToContents();
}

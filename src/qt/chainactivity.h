// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CHAINACTIVITY_H
#define BITCOIN_QT_CHAINACTIVITY_H

#include <QWidget>

class ClientModel;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/** The latest blocks and the latest transactions waiting to be mined, for the overview page. */
class ChainActivity : public QWidget
{
    Q_OBJECT

public:
    explicit ChainActivity(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    /** The user asked for the details of a block or of a transaction. */
    void detailsRequested(const QString& hash);

protected:
    void showEvent(QShowEvent* event) override;

private:
    ClientModel* m_client_model{nullptr};
    QTableWidget* m_blocks;
    QTableWidget* m_transactions;
    //! Limits how often the tables are refreshed while blocks come in fast.
    QTimer* m_timer;
};

#endif // BITCOIN_QT_CHAINACTIVITY_H

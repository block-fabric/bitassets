// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MININGDIALOG_H
#define BITCOIN_QT_MININGDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
QT_END_NAMESPACE

/** Window to mine with the processor of this computer. */
class MiningDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MiningDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void start();
    void stop();
    void newAddress();

private:
    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QTimer* m_timer;
    QLineEdit* m_address;
    QPushButton* m_new_address;
    QSpinBox* m_threads;
    QPushButton* m_start;
    QPushButton* m_stop;
    QLabel* m_status;
    QLabel* m_hash_rate;
    QLabel* m_expected;
    QLabel* m_found;
    QLabel* m_height;
    QLabel* m_difficulty;
    QLabel* m_network_rate;
    QLabel* m_mempool;
    QLabel* m_chain;
};

#endif // BITCOIN_QT_MININGDIALOG_H

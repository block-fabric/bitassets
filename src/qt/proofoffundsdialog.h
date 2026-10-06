// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROOFOFFUNDSDIALOG_H
#define BITCOIN_QT_PROOFOFFUNDSDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPlainTextEdit;
class QTabWidget;
QT_END_NAMESPACE

/**
 * Window to prove to someone else that this wallet holds funds, by signing a
 * statement with the addresses that hold them, and to check such a proof.
 */
class ProofOfFundsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProofOfFundsDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model) { m_client_model = client_model; }

public Q_SLOTS:
    void prove();
    void verify();

private:
    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QLineEdit* m_statement;
    QPlainTextEdit* m_proof;
    QPlainTextEdit* m_input;
    QPlainTextEdit* m_verdict;
};

#endif // BITCOIN_QT_PROOFOFFUNDSDIALOG_H

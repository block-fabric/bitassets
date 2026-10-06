// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TIMESTAMPDIALOG_H
#define BITCOIN_QT_TIMESTAMPDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
QT_END_NAMESPACE

/**
 * Window to prove that a file existed at some time, by publishing its hash on
 * the chain, and to look such a proof up.
 */
class TimestampDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TimestampDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model) { m_client_model = client_model; }
    /** Choose the file to stamp or to verify. */
    void setFile(const QString& path);

    /** The data published for a file with the given SHA-256 hash, in hexadecimal. */
    static QString Payload(const QByteArray& sha256);

public Q_SLOTS:
    void stamp();
    void verify();

private:
    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QLineEdit* m_path;
    QLabel* m_hash_label;
    QSpinBox* m_blocks;
    QPlainTextEdit* m_result;
    //! SHA-256 of the chosen file; empty if there is none.
    QByteArray m_hash;
};

#endif // BITCOIN_QT_TIMESTAMPDIALOG_H

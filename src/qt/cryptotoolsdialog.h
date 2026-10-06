// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CRYPTOTOOLSDIALOG_H
#define BITCOIN_QT_CRYPTOTOOLSDIALOG_H

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QTabWidget;
QT_END_NAMESPACE

/** Window with a hash calculator, a merkle tree viewer and an address decoder. */
class CryptoToolsDialog : public QDialog
{
    Q_OBJECT

public:
    enum Tab { HASH, MERKLE, ADDRESS };

    explicit CryptoToolsDialog(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model) { m_client_model = client_model; }
    void showTab(Tab tab);

private Q_SLOTS:
    void updateHashes();
    void loadBlock();
    void updateMerkleTree();
    void decodeAddress();

private:
    ClientModel* m_client_model{nullptr};
    QTabWidget* m_tabs;
    QPlainTextEdit* m_hash_input;
    QCheckBox* m_hash_hex;
    QPlainTextEdit* m_hash_output;
    QLineEdit* m_merkle_block;
    QPlainTextEdit* m_merkle_input;
    QPlainTextEdit* m_merkle_output;
    QLineEdit* m_address;
    QPlainTextEdit* m_address_output;
};

#endif // BITCOIN_QT_CRYPTOTOOLSDIALOG_H

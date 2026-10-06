// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGDIALOG_H
#define BITCOIN_QT_MULTISIGDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/**
 * Window to share coins with partners: addresses that need the signatures of
 * several keys to spend from, and the transactions that spend from them.
 *
 * No private key leaves a wallet. Partners exchange public keys, and pass a
 * partially signed transaction (PSBT) around for each of them to sign with
 * their own wallet.
 */
class MultisigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MultisigDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model) { m_client_model = client_model; }

public Q_SLOTS:
    /** Add a key of the open wallet to the list of keys. */
    void addOwnKey();
    void addPartnerKey();
    void removeKey();
    /** Make a shared address of the keys that are ticked. */
    void createAddress();
    /** Add a shared address made by a partner, from its descriptor. */
    void importAddress();
    void removeAddress();
    void refreshBalances();
    /** Make the transaction that spends from the chosen shared address. */
    void createTransaction();
    void sign();
    void broadcast();

protected:
    void showEvent(QShowEvent* event) override;

private:
    //! Keys and shared addresses are settings of this user, kept as (name, value) pairs.
    using Entries = QList<QPair<QString, QString>>;
    static Entries Load(const char* setting);
    static void Store(const char* setting, const Entries& entries);

    void reload();
    bool addAddress(const QString& name, const QString& descriptor);
    void updateSignatureCount();
    void warn(const QString& text);

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QTabWidget* m_tabs;
    QTableWidget* m_keys;
    QLineEdit* m_key_name;
    QLineEdit* m_key;
    QListWidget* m_signers;
    QSpinBox* m_required;
    QLineEdit* m_address_name;
    QTableWidget* m_addresses;
    QComboBox* m_from;
    QLineEdit* m_destination;
    QLineEdit* m_amount;
    QLineEdit* m_fee;
    QPlainTextEdit* m_psbt;
    QLabel* m_psbt_status;
};

#endif // BITCOIN_QT_MULTISIGDIALOG_H

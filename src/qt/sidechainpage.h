// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SIDECHAINPAGE_H
#define BITCOIN_QT_SIDECHAINPAGE_H

#include <univalue.h>

#include <QPointer>
#include <QWidget>

#include <optional>
#include <string>

class BitcoinAmountField;
class ClientModel;
class PlatformStyle;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/**
 * Page for drivechains: the active sidechains and deposits to them, sidechain
 * proposals and whether this node acks them, and withdrawal bundles and how
 * this node votes on them.
 *
 * The page works through the drivechain RPC commands of the node.
 */
class SidechainPage : public QWidget
{
    Q_OBJECT

public:
    explicit SidechainPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

public Q_SLOTS:
    /** Reload everything shown from the node. */
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void deposit();
    void propose();
    void setAck(bool ack);
    void removeQueuedProposal();
    void setVote(const QString& vote);
    void updateDepositTarget();

private:
    /**
     * Run an RPC command. Returns nothing after showing the error to the user
     * if the command fails.
     */
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet = false, bool quiet = false);

    QWidget* createSidechainsTab();
    QWidget* createProposalsTab();
    QWidget* createWithdrawalsTab();
    static QTableWidget* createTable(const QStringList& headers, QWidget* parent);
    /** Text of a column of the selected row, or an empty string. */
    static QString selected(const QTableWidget* table, int column);

    ClientModel* m_client_model{nullptr};
    //! Guarded: the wallet may be unloaded while a message box of this page is shown.
    QPointer<WalletModel> m_wallet_model;

    QLabel* m_summary{nullptr};
    QTabWidget* m_tabs{nullptr};

    QTableWidget* m_sidechains{nullptr};
    QLabel* m_deposit_target{nullptr};
    QLineEdit* m_deposit_destination{nullptr};
    BitcoinAmountField* m_deposit_amount{nullptr};
    QPushButton* m_deposit_button{nullptr};

    QTableWidget* m_proposals{nullptr};
    QTableWidget* m_queued{nullptr};
    QSpinBox* m_proposal_slot{nullptr};
    QLineEdit* m_proposal_title{nullptr};
    QLineEdit* m_proposal_description{nullptr};
    QLineEdit* m_proposal_hash1{nullptr};
    QLineEdit* m_proposal_hash2{nullptr};

    QTableWidget* m_bundles{nullptr};
    int m_min_score{0};
};

#endif // BITCOIN_QT_SIDECHAINPAGE_H

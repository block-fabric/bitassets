// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BITASSETSPAGE_H
#define BITCOIN_QT_BITASSETSPAGE_H

#include <qt/noderpc.h>

#include <QWidget>

#include <map>

class ClientModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;
QT_END_NAMESPACE

/**
 * The assets (BitAssets), a page of the wallet:
 *  - My assets: what the wallet holds, in a list, and what is picked in a panel next to it: send,
 *    mint, change the data, give or burn the control coin; pool shares to take out; auctions to collect;
 *    and creating an asset.
 *  - Trade: swap one asset for another in their pool; the pools, and putting liquidity in.
 *  - Auctions: those running, bidding on them; selling by auction.
 *  - Explore: every asset. Activity: the wallet's asset transactions.
 * The lists update in place every few seconds; a panel is rebuilt only when something else is
 * picked or after an action, never while it is used.
 */
class BitAssetsPage : public QWidget
{
    Q_OBJECT

public:
    explicit BitAssetsPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* createMineTab();
    QWidget* createTradeTab();
    QWidget* createAuctionsTab();
    QWidget* createExploreTab();
    QWidget* createActivityTab();

    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet, bool quiet = false);
    void say(const QString& text, bool error = false);
    /** Ask before doing something: nothing is rebuilt while the question is open. */
    bool confirm(const QString& title, const QString& text, bool danger = false);

    void refresh();
    void refreshInfo();
    void refreshMine();
    void refreshTrade();
    void refreshAuctions();
    void refreshExplore();
    void refreshActivity();
    void registerPending();
    /** The assets there are, for the lists to pick from: "CHN" first. */
    void refreshAssetChoices();

    void showMineDetail();
    void showCreatePanel();
    /** Sending any asset held, and the address to receive them at. */
    void showTransferPanel();
    void showAssetPanel(const UniValue& holding);
    void showReservationPanel(const UniValue& reservation);
    void showLiquidityPanel(const UniValue& position);
    void showReceiptPanel(const QString& auction);
    void showAuctionDetail();
    void showExploreDetail(const QString& asset);
    void setPanel(QScrollArea* area, QWidget* panel);

    void updateQuote();
    void updateLiquidityQuote();
    /** Go to Trade with this asset ("0x" and its hash) to buy or sell for CHN. */
    void tradeAsset(const QString& asset);

    NodeRpc::WalletNameFn m_wallet_name;
    ClientModel* m_client_model{nullptr};
    QTabWidget* m_tabs{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_summary{nullptr};
    bool m_busy{false};
    int m_height{0};

    // My assets.
    QListWidget* m_mine{nullptr};
    QScrollArea* m_mine_detail{nullptr};
    QString m_mine_shown;
    //! The address shown to receive assets at.
    QString m_receive_address;
    //! The name of what is shown, to follow it as it changes (reserved, then registered).
    QString m_mine_shown_name;
    std::string m_mine_listed;
    UniValue m_holdings;

    // Trade.
    QComboBox* m_pay_asset{nullptr};
    QLineEdit* m_pay_amount{nullptr};
    QComboBox* m_get_asset{nullptr};
    QLabel* m_get_amount{nullptr};
    QLabel* m_quote_line{nullptr};
    QDoubleSpinBox* m_slippage{nullptr};
    QPushButton* m_swap{nullptr};
    QTimer* m_quote_timer{nullptr};
    //! The price impact of the trade quoted last, in percent.
    double m_last_impact{0};
    QTableWidget* m_pools{nullptr};
    std::string m_pools_listed;
    QComboBox* m_lq_a{nullptr};
    QLineEdit* m_lq_amount_a{nullptr};
    QComboBox* m_lq_b{nullptr};
    QLineEdit* m_lq_amount_b{nullptr};
    QLabel* m_lq_line{nullptr};
    QTimer* m_lq_timer{nullptr};

    // Auctions.
    QTableWidget* m_auctions{nullptr};
    QScrollArea* m_auction_detail{nullptr};
    QString m_auction_shown;
    std::string m_auctions_listed;
    QComboBox* m_sell_asset{nullptr};
    QLineEdit* m_sell_amount{nullptr};
    QComboBox* m_sell_for{nullptr};
    QLineEdit* m_sell_start{nullptr};
    QLineEdit* m_sell_end{nullptr};
    QSpinBox* m_sell_duration{nullptr};
    QSpinBox* m_sell_start_in{nullptr};

    // Explore and activity.
    QLineEdit* m_search{nullptr};
    QListWidget* m_all{nullptr};
    QScrollArea* m_all_detail{nullptr};
    std::string m_all_listed;
    QTableWidget* m_activity{nullptr};
    std::string m_activity_listed;

    //! The assets there are, "CHN" first: their labels, how the commands name them ("0x" and the
    //! hash), and their decimals by that (labels can look alike: "CHN", or a number of another).
    QStringList m_asset_labels;
    QStringList m_asset_args;
    std::map<QString, int> m_decimals;
};

#endif // BITCOIN_QT_BITASSETSPAGE_H

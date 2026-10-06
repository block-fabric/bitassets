// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SIDECHAINNODES_H
#define BITCOIN_QT_SIDECHAINNODES_H

#include <qt/noderpc.h>

#include <QDialog>
#include <QMap>
#include <QStringList>

#include <functional>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/**
 * Window to run the nodes of the sidechains of this chain from here: tell it
 * where the programs of a sidechain are, or have it download them, and it
 * starts and stops the node, shows how it is doing, and turns its merged
 * mining on and off.
 *
 * A sidechain node follows a mainchain node over RPC. The ones started here
 * are pointed at this node, so this node has to accept RPC connections.
 */
class SidechainNodesDialog : public QDialog
{
    Q_OBJECT

public:
    /** A sidechain this window knows how to run. */
    struct Sidechain {
        QString name;
        //! Stem of the names of its programs: <stem>d, <stem>-cli, <stem>-qt.
        QString stem;
        int slot{0};
        QString description;
    };

    explicit SidechainNodesDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

    /** The sidechains listed: the ones built in and the ones the user added. */
    QList<Sidechain> sidechains() const;
    /** Tell the window where the programs of a sidechain are. */
    void setBinDir(const QString& stem, const QString& dir);
    QString binDir(const QString& stem) const;
    /** Where the node of a sidechain that is run from here keeps its data. */
    QString dataDir(const Sidechain& sidechain) const;
    /** The arguments a program of a sidechain is run with to reach its node on the network of this node. */
    QStringList networkArgs(const Sidechain& sidechain) const;
    /** The arguments its node is started with on top of those. */
    QStringList nodeArgs(const Sidechain& sidechain) const;

public Q_SLOTS:
    void refresh();
    void locate();
    void download();
    void addSidechain();
    void removeSidechain();
    void startNode();
    void startWallet();
    void stopNode();
    void toggleMining();
    void mineOnce();
    void showLog();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /** Run a command of the node of a sidechain and hand its answer, or nothing if it failed, to `then`. */
    void runCli(const Sidechain& sidechain, const QStringList& command, std::function<void(const std::optional<UniValue>&, const QString& error)> then);
    std::optional<Sidechain> selected() const;
    QString program(const Sidechain& sidechain, const QString& suffix) const;
    void setCell(int row, int column, const QString& text);
    void say(const QString& text);

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QTimer* m_timer;
    QLabel* m_notice;
    QTableWidget* m_table;
    QPlainTextEdit* m_log;
    //! Whether the node of each sidechain answered the last time, and whether it mines.
    QMap<QString, bool> m_running;
    QMap<QString, bool> m_mining;
};

#endif // BITCOIN_QT_SIDECHAINNODES_H

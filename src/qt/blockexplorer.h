// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BLOCKEXPLORER_H
#define BITCOIN_QT_BLOCKEXPLORER_H

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/** Window to browse the blocks of the chain and the transactions waiting to be mined. */
class BlockExplorer : public QDialog
{
    Q_OBJECT

public:
    explicit BlockExplorer(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();
    /** Show a block, given its height or hash, or a transaction, given its id. */
    void search(const QString& query);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshBlocks();
    void refreshMempool();
    bool showBlock(const QString& hash);
    bool showTransaction(const QString& txid, const QString& block_hash);

    ClientModel* m_client_model{nullptr};
    QLineEdit* m_search;
    QSpinBox* m_count;
    QTabWidget* m_tabs;
    QTableWidget* m_blocks;
    QTableWidget* m_mempool;
    QLabel* m_details_title;
    QListWidget* m_block_txs;
    QPlainTextEdit* m_details;
    //! Hash of the block whose transactions are listed.
    QString m_shown_block;
};

#endif // BITCOIN_QT_BLOCKEXPLORER_H

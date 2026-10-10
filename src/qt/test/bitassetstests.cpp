// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/bitassetstests.h>

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <node/miner.h>
#include <pow.h>
#include <qt/bitassetspage.h>
#include <qt/clientmodel.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <rpc/server.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>

#include <functional>
#include <memory>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

/**
 * A regtest sidechain whose mainchain is a record the test writes: each block is committed to by a
 * mainchain block of its own, which may also carry deposits.
 */
struct SideChain {
    TestingSetup& test;
    int main_blocks{0};

    /** Add a mainchain block that commits to `bmm` (if given) and pays `deposits`. */
    bool AppendMain(std::optional<uint256> bmm, std::vector<sidechain::MainDeposit> deposits = {})
    {
        sidechain::Mainchain& record{*Assert(test.m_node.chainman->m_mainchain)};
        sidechain::MainBlock block;
        block.prev_hash = record.TipHash();
        block.hash = uint256{static_cast<uint8_t>(++main_blocks)};
        block.hash.data()[31] = 0x4d;
        block.time = 1'700'000'000 + main_blocks * 600;
        block.bmm = bmm;
        block.deposits = std::move(deposits);
        return record.Append(block);
    }

    /** Mine a block with the mempool, which the next mainchain block commits to. */
    bool Mine(const CScript& script)
    {
        auto mining{interfaces::MakeMining(test.m_node)};
        auto block_template{mining->createNewBlock({.use_mempool = true, .coinbase_output_script = script}, /*cooldown=*/false)};
        if (!block_template) return false;
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, test.m_node.chainman->GetConsensus())) ++block.nNonce;
        if (!AppendMain(block.GetHash())) return false;
        bool new_block{false};
        if (!test.m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) return false;
        test.m_node.validation_signals->SyncWithValidationInterfaceQueue();
        return WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash();
    }
};

/**
 * Press a button of every message box that pops up while `action` runs.
 * @return the texts of the message boxes
 */
QStringList WithMessageBoxes(QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [button, &texts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (!box || !box->isVisible()) continue;
            texts << box->text();
            QAbstractButton* choice{box->button(button)};
            if (!choice) choice = box->button(QMessageBox::Ok);
            if (choice) choice->click();
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

/** The panels a page replaced go (deleteLater), and what was queued runs. */
void Settle()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QPushButton* Button(QWidget* root, const QString& text)
{
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QLabel* LabelWith(QWidget* root, const QString& part)
{
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text().contains(part)) return label;
    }
    return nullptr;
}

/** A label of a name (objectName), such as the title of a panel, with this text. */
bool Shows(QWidget* root, const char* name, const QString& text)
{
    for (QLabel* label : root->findChildren<QLabel*>(name)) {
        if (label->text() == text) return true;
    }
    return false;
}

QLineEdit* EditWithPlaceholder(QWidget* root, const QString& placeholder)
{
    for (QLineEdit* edit : root->findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == placeholder) return edit;
    }
    return nullptr;
}

/** The field made right before or after another (a row of a form). */
QLineEdit* EditNextTo(QWidget* root, QLineEdit* edit, int offset)
{
    const QList<QLineEdit*> edits{root->findChildren<QLineEdit*>()};
    const qsizetype at{edits.indexOf(edit)};
    if (at < 0 || at + offset < 0 || at + offset >= edits.size()) return nullptr;
    return edits.at(at + offset);
}

QTableWidget* TableOf(QWidget* root, const QString& first_header)
{
    for (QTableWidget* table : root->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) && table->horizontalHeaderItem(0)->text() == first_header) return table;
    }
    return nullptr;
}

QListWidgetItem* ItemWith(QListWidget* list, const QString& part)
{
    for (int i{0}; i < list->count(); ++i) {
        if (list->item(i)->text().contains(part)) return list->item(i);
    }
    return nullptr;
}

bool TableHas(QTableWidget* table, int column, const QString& part)
{
    for (int row{0}; row < table->rowCount(); ++row) {
        if (table->item(row, column) && table->item(row, column)->text().contains(part)) return true;
    }
    return false;
}

void Pick(QComboBox* combo, const QString& arg)
{
    const int index{combo->findData(arg)};
    QVERIFY2(index >= 0, qPrintable(arg));
    combo->setCurrentIndex(index);
}

/** Show a tab, refreshed: a page refreshes each time its tab changes. */
void Refresh(QTabWidget* tabs, int index)
{
    if (tabs->currentIndex() == index) tabs->setCurrentIndex((index + 1) % tabs->count());
    tabs->setCurrentIndex(index);
    Settle();
}

void TestBitAssetsPage(interfaces::Node& node)
{
    TestingSetup test{ChainType::REGTEST, {.extra_args = {"-sidechainslot=9"}}};
    QVERIFY(test.m_node.chainman->GetConsensus().sidechain.enabled);
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    // The page works through RPC, including the wallet commands.
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    // What the page registers by itself is kept in the settings: none from an earlier run.
    QSettings{}.remove("BitAssets");

    // An empty wallet, which follows the chain.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip())};
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    wallet->m_chain_notifications_handler = wallet->chain().handleNotifications(wallet);
    wallet->SetBroadcastTransactions(true);
    // There is no fee estimate on the test chain.
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    // Whatever check fails, the wallet goes before the node, and the settings are left clean.
    struct Unload {
        WalletContext& context;
        std::shared_ptr<CWallet>& wallet;
        ~Unload()
        {
            QSettings{}.remove("BitAssets");
            wallet->m_chain_notifications_handler.reset();
            RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
        }
    } unload{context, wallet};

    // Coins come to a sidechain by deposits: one to the wallet, paid by the block after the mainchain made it.
    SideChain chain{test};
    const CScript miner{CScript() << OP_TRUE};
    QVERIFY(chain.AppendMain(std::nullopt));
    sidechain::MainDeposit deposit;
    deposit.destination = wallet::getnewaddress(*wallet);
    deposit.amount = 50 * COIN;
    deposit.txid = uint256{0xde};
    QVERIFY(chain.AppendMain(std::nullopt, {deposit}));
    QVERIFY(chain.Mine(miner));
    QCOMPARE(wallet::GetBalance(*wallet).m_mine_trusted, 50 * COIN);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());
    const auto rpc{[&](const std::string& method, const UniValue& params, bool of_wallet) {
        QString rpc_error;
        const auto result{NodeRpc::Call(&client_model, method, params, rpc_error, of_wallet ? std::optional<QString>{QString{}} : std::nullopt)};
        if (!result) qWarning("%s: %s", method.c_str(), qPrintable(rpc_error));
        return result;
    }};
    const auto mine{[&] { return chain.Mine(miner); }};

    // The page, offscreen.
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    BitAssetsPage page(wallet_name);
    page.resize(1200, 800);
    page.setClientModel(&client_model);
    page.show();
    Settle();
    auto* tabs{page.findChild<QTabWidget*>()};
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 5);
    // The title, the summary, then the status line.
    const QList<QLabel*> top{page.findChildren<QLabel*>(Qt::FindDirectChildrenOnly)};
    QCOMPARE(top.size(), 3);
    QLabel* summary{top.at(1)};
    QLabel* status{top.at(2)};
    QVERIFY2(summary->text().startsWith("0 assets · 0 pools · 0 auctions · next block"), qPrintable(summary->text()));

    // My assets: nothing yet, and the panel to create one.
    QWidget* mine_tab{tabs->widget(0)};
    auto* mine_list{mine_tab->findChild<QListWidget*>("pickList")};
    QVERIFY(mine_list);
    QVERIFY(ItemWith(mine_list, "Nothing yet"));
    auto* name{mine_tab->findChild<QLineEdit*>("nameSearch")};
    QVERIFY(name);
    auto* create{Button(mine_tab, "Create")};
    QVERIFY(create);
    // A name that reads as another asset, refused before anything is asked of the node.
    name->setText("CHN");
    QTRY_VERIFY(LabelWith(mine_tab, "does not read as another asset"));
    QVERIFY(!create->isEnabled());
    name->setText("GOLD");
    QTRY_VERIFY(create->isEnabled());
    QVERIFY(LabelWith(mine_tab, "GOLD is free"));
    EditWithPlaceholder(mine_tab, "0: none yet; mint later")->setText("1000");
    EditWithPlaceholder(mine_tab, "What it is: a description, a link (optional)")->setText("Gold, by the gram");
    // Clicked, as with the mouse: the button has the focus, and the panel is rebuilt with what was
    // created (the panel of Create stayed, its button ready to reserve the name again).
    QTest::mouseClick(create, Qt::LeftButton);
    Settle();
    QVERIFY2(status->text().startsWith("GOLD is reserved"), qPrintable(status->text()));
    QCOMPARE(QSettings{}.value("BitAssets/pending/").toStringList().size(), 1);
    QVERIFY(ItemWith(mine_list, "GOLD — being created"));
    QVERIFY(Shows(mine_tab, "panelTitle", "GOLD"));
    QVERIFY(!Button(mine_tab, "Create"));

    // The page registers it once the reservation is deep enough; then it is held.
    bool waited{false};
    for (int i{0}; i < 6 && !ItemWith(mine_list, "1,000.00  GOLD"); ++i) {
        QVERIFY(mine());
        Refresh(tabs, 0);
        waited |= ItemWith(mine_list, "GOLD — registering in 1 block") != nullptr;
    }
    QVERIFY(waited);
    QVERIFY(ItemWith(mine_list, "1,000.00  GOLD   ★"));
    QVERIFY(QSettings{}.value("BitAssets/pending/").toStringList().isEmpty());
    const auto gold{rpc("getasset", NodeRpc::Args({"GOLD"}), false)};
    QVERIFY(gold);
    QCOMPARE((*gold)["data"]["info"].get_str(), std::string{"Gold, by the gram"});
    const QString gold_arg{QStringLiteral("0x") + QString::fromStdString((*gold)["asset"].get_str())};
    // What was being created is shown as what it became: the asset.
    QVERIFY(Shows(mine_tab, "panelTitle", "GOLD"));
    QVERIFY(Shows(mine_tab, "bigNumber", "1,000.00 GOLD"));
    QVERIFY(LabelWith(mine_tab, "No pool trades it for CHN yet."));
    QVERIFY2(summary->text().startsWith("1 assets"), qPrintable(summary->text()));

    // Mint, to this wallet.
    auto* mint_to{EditWithPlaceholder(mine_tab, "To: this wallet, or an address")};
    QVERIFY(mint_to);
    EditNextTo(mine_tab, mint_to, -1)->setText("50");
    Button(mine_tab, "Mint")->click();
    QVERIFY2(status->text().startsWith("Minting 50 GOLD"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QVERIFY(ItemWith(mine_list, "1,050.00  GOLD"));

    // Its description and commitment.
    auto* commitment{EditWithPlaceholder(mine_tab, "A hash (64 hex digits) of documents kept elsewhere")};
    QVERIFY(commitment);
    auto* info{EditNextTo(mine_tab, commitment, -1)};
    QCOMPARE(info->text(), QString{"Gold, by the gram"});
    Button(mine_tab, "Save")->click();
    QCOMPARE(status->text(), QString{"Nothing changed."});
    info->setText("Gold, by the ounce");
    commitment->setText(QString(64, 'a'));
    Button(mine_tab, "Save")->click();
    QVERIFY2(status->text().startsWith("Saved: the description of GOLD"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    const auto described{rpc("getasset", NodeRpc::Args({"GOLD"}), false)};
    QCOMPARE((*described)["data"]["info"].get_str(), std::string{"Gold, by the ounce"});
    QCOMPARE((*described)["data"]["commitment"].get_str(), std::string(64, 'a'));

    // Send some, then burn some.
    const auto own_address{rpc("getnewaddress", NodeRpc::Args({}), true)};
    QVERIFY(own_address);
    auto* send_to{EditWithPlaceholder(mine_tab, "Address")};
    QVERIFY(send_to);
    send_to->setText(QString::fromStdString(own_address->get_str()));
    EditNextTo(mine_tab, send_to, 1)->setText("10");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(mine_tab, "Send")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Sent 10 GOLD"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    auto* burn{Button(mine_tab, "Burn")};
    QVERIFY(burn);
    burn->parentWidget()->findChild<QLineEdit*>()->setText("5");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { burn->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Burning 5 GOLD"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QVERIFY(ItemWith(mine_list, "1,045.00  GOLD"));

    // Sending and receiving, of any asset held.
    Button(mine_tab, "⇄  Send && receive")->click();
    Settle();
    QVERIFY(Shows(mine_tab, "panelTitle", "Send && receive assets"));
    QLineEdit* receive{nullptr};
    for (QLineEdit* edit : mine_tab->findChildren<QLineEdit*>()) {
        if (edit->isReadOnly()) receive = edit;
    }
    QVERIFY(receive && !receive->text().isEmpty());
    const QString first_address{receive->text()};
    Button(mine_tab, "New address")->click();
    QVERIFY(receive->text() != first_address);
    Button(mine_tab, "Copy")->click();
    QCOMPARE(status->text(), QString{"The address is copied."});
    QCOMPARE(mine_tab->findChild<QComboBox*>()->currentText(), QString{"GOLD"});
    QVERIFY(LabelWith(mine_tab, "You hold 1,045.00 GOLD"));
    auto* transfer_amount{EditWithPlaceholder(mine_tab, "Amount")};
    Button(mine_tab, "All")->click();
    QCOMPARE(transfer_amount->text(), QString{"1045.00"});
    transfer_amount->setText("1");
    Button(mine_tab, "Send")->click();
    QCOMPARE(status->text(), QString{"Give an address and an amount."});
    EditWithPlaceholder(mine_tab, "Their address")->setText(first_address);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(mine_tab, "Send")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Sent 1 GOLD"), qPrintable(status->text()));
    QVERIFY(mine());

    // Trade: a pool made, then a swap in it.
    Refresh(tabs, 1);
    QWidget* trade_tab{tabs->widget(1)};
    const QList<QComboBox*> trade_combos{trade_tab->findChildren<QComboBox*>()};
    QCOMPARE(trade_combos.size(), 4);
    QComboBox* pay{trade_combos.at(0)};
    QComboBox* get{trade_combos.at(1)};
    QComboBox* lq_a{trade_combos.at(2)};
    QComboBox* lq_b{trade_combos.at(3)};
    auto* pools{TableOf(trade_tab, "Pool")};
    QVERIFY(pools);
    QVERIFY(TableHas(pools, 0, "No pools yet"));
    Pick(lq_a, gold_arg);
    Pick(lq_b, "CHN");
    QTRY_VERIFY(LabelWith(trade_tab, "A new pool"));
    const QList<QLineEdit*> lq_amounts{[&] {
        QList<QLineEdit*> edits;
        for (QLineEdit* edit : trade_tab->findChildren<QLineEdit*>()) {
            if (edit->placeholderText() == "Amount") edits << edit;
        }
        return edits;
    }()};
    QCOMPARE(lq_amounts.size(), 2);
    lq_amounts.at(0)->setText("100");
    Button(trade_tab, "Add")->click();
    QVERIFY2(status->text().startsWith("A new pool: give both amounts"), qPrintable(status->text()));
    lq_amounts.at(1)->setText("1");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(trade_tab, "Add")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Making the GOLD / CHN pool with 100 GOLD and 1 CHN"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 1);
    QVERIFY(TableHas(pools, 0, "GOLD / CHN"));
    QVERIFY(TableHas(pools, 2, "1 GOLD = 0.01 CHN"));
    // Picking a pool picks it for a swap and for liquidity.
    Pick(pay, gold_arg);
    Pick(get, gold_arg);
    Q_EMIT pools->cellClicked(0, 0);
    QCOMPARE(pay->currentData().toString(), QString{"CHN"});
    QCOMPARE(get->currentData().toString(), gold_arg);
    // A quote, then the swap.
    auto* pay_amount{EditWithPlaceholder(trade_tab, "0.0")};
    auto* swap{Button(trade_tab, "Swap")};
    auto* get_amount{trade_tab->findChild<QLabel*>("getAmount")};
    pay_amount->setText("0.05");
    QTRY_VERIFY(swap->isEnabled());
    QVERIFY(get_amount->text() != "—");
    QVERIFY(LabelWith(trade_tab, "price impact"));
    // The other way round, and back.
    Button(trade_tab, "⇅")->click();
    QCOMPARE(pay->currentData().toString(), gold_arg);
    Button(trade_tab, "⇅")->click();
    QCOMPARE(pay->currentData().toString(), QString{"CHN"});
    QTRY_VERIFY(swap->isEnabled());
    // A price impact of more than 2% is asked about.
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { swap->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Swapping 0.05 CHN for about"), qPrintable(status->text()));
    QVERIFY(pay_amount->text().isEmpty());
    QVERIFY(mine());
    Refresh(tabs, 1);
    QVERIFY(TableHas(pools, 4, "1"));
    // CHN dust out of a pool: not offered.
    Button(trade_tab, "⇅")->click();
    pay_amount->setText("0.01");
    QTRY_VERIFY(LabelWith(trade_tab, "Too little: the least CHN a pool pays out"));
    QVERIFY(!swap->isEnabled());
    pay_amount->clear();
    // More liquidity, at the pool's price: the second amount is worked out.
    lq_amounts.at(0)->setText("10");
    Q_EMIT lq_amounts.at(0)->textEdited("10");
    QTRY_VERIFY(lq_amounts.at(1)->isReadOnly() && !lq_amounts.at(1)->text().isEmpty());
    Button(trade_tab, "Add")->click();
    QVERIFY2(status->text().startsWith("Adding 10.00 GOLD and"), qPrintable(status->text()));
    QVERIFY(mine());

    // The pool shares, in My assets.
    Refresh(tabs, 0);
    QListWidgetItem* shares{ItemWith(mine_list, "GOLD / CHN")};
    QVERIFY(shares);
    mine_list->setCurrentItem(shares);
    Settle();
    QVERIFY(Shows(mine_tab, "panelTitle", "GOLD / CHN pool"));
    mine_tab->findChild<QSpinBox*>()->setValue(50);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(mine_tab, "Take out")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("Taking out about"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    Button(mine_tab, "Add more")->click();
    Settle();
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(lq_a->currentData().toString(), gold_arg);

    // Auctions: sell 10 GOLD, bid on it, buy what is left, collect.
    Refresh(tabs, 2);
    QWidget* auctions_tab{tabs->widget(2)};
    const QList<QComboBox*> sell_combos{auctions_tab->findChildren<QComboBox*>()};
    QCOMPARE(sell_combos.size(), 2);
    Pick(sell_combos.at(0), gold_arg);
    Pick(sell_combos.at(1), "CHN");
    EditWithPlaceholder(auctions_tab, "How much")->setText("10");
    EditWithPlaceholder(auctions_tab, "for all of it, at the start")->setText("0.5");
    EditWithPlaceholder(auctions_tab, "for all of it, at the end: the least you take")->setText("1");
    Button(auctions_tab, "Start the auction")->click();
    QCOMPARE(status->text(), QString{"The price falls: the end price is at most the start price."});
    EditWithPlaceholder(auctions_tab, "for all of it, at the start")->setText("1");
    EditWithPlaceholder(auctions_tab, "for all of it, at the end: the least you take")->setText("0.5");
    auctions_tab->findChildren<QSpinBox*>().at(0)->setValue(5);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(auctions_tab, "Start the auction")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("The auction starts after the next block"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 2);
    auto* auctions{TableOf(auctions_tab, "Selling")};
    QVERIFY(auctions);
    QCOMPARE(auctions->item(0, 0)->text(), QString{"10.00 of 10.00 GOLD"});
    QCOMPARE(auctions->item(0, 3)->text(), QString{"open"});
    QCOMPARE(auctions->item(0, 2)->text(), QString{"0.1 CHN each"});
    auctions->setCurrentCell(0, 0);
    Settle();
    QVERIFY(Shows(auctions_tab, "panelTitle", "10.00 GOLD for CHN"));
    auto* bid_amount{EditWithPlaceholder(auctions_tab, "What you pay, in CHN")};
    QVERIFY(bid_amount);
    bid_amount->setText("0.1");
    QVERIFY(LabelWith(auctions_tab, "buys 1.00 GOLD in the next block"));
    Button(auctions_tab, "Bid")->click();
    QVERIFY2(status->text().startsWith("Bidding 0.10000000 CHN for at least 1.00 GOLD"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 2);
    QCOMPARE(auctions->item(0, 0)->text().left(13), QString{"9.00 of 10.00"});
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(auctions_tab, "Buy all that is left")->click(); }).size(), 1);
    QVERIFY2(status->text().contains("for the 9.00 GOLD left"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 2);
    QVERIFY(TableHas(auctions, 3, "sold out"));
    // Its receipt, in My assets: collected.
    Refresh(tabs, 0);
    QListWidgetItem* receipt{ItemWith(mine_list, "Auction ")};
    QVERIFY(receipt);
    mine_list->setCurrentItem(receipt);
    Settle();
    QVERIFY(Shows(mine_tab, "panelTitle", "Your auction of GOLD"));
    QVERIFY(LabelWith(mine_tab, "SOLD OUT"));
    auto* collect{Button(mine_tab, "Collect")};
    QVERIFY(collect && collect->isEnabled());
    collect->click();
    QVERIFY2(status->text().startsWith("Collecting with the next block"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QVERIFY(!ItemWith(mine_list, "Auction "));

    // A reservation made elsewhere, released from its panel.
    QVERIFY(rpc("reserveasset", NodeRpc::Args({"SPARE"}), true));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QListWidgetItem* spare{ItemWith(mine_list, "SPARE — registering")};
    QVERIFY(spare);
    mine_list->setCurrentItem(spare);
    Settle();
    QVERIFY(Shows(mine_tab, "panelTitle", "SPARE"));
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(mine_tab, "Release this reservation")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("The reservation of SPARE is released"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QVERIFY(!ItemWith(mine_list, "SPARE"));

    // An asset that dies: all of it burned and its supply fixed from its panel, retired from Explore.
    QVERIFY(rpc("reserveasset", NodeRpc::Args({"PEBBLES"}), true));
    QVERIFY(mine());
    QVERIFY(mine());
    QVERIFY(rpc("registerasset", NodeRpc::Args({"PEBBLES", 5}), true));
    QVERIFY(mine());
    Refresh(tabs, 0);
    QListWidgetItem* pebbles{ItemWith(mine_list, "5  PEBBLES")};
    QVERIFY(pebbles);
    mine_list->setCurrentItem(pebbles);
    Settle();
    QVERIFY(Shows(mine_tab, "panelTitle", "PEBBLES"));
    auto* burn_pebbles{Button(mine_tab, "Burn")};
    burn_pebbles->parentWidget()->findChild<QLineEdit*>()->setText("5");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { burn_pebbles->click(); }).size(), 1);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(mine_tab, "Fix the supply for good")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("The supply of PEBBLES is fixed"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 3);
    QWidget* explore_tab{tabs->widget(3)};
    auto* all_list{explore_tab->findChild<QListWidget*>("pickList")};
    QVERIFY(all_list);
    QListWidgetItem* dead{ItemWith(all_list, "PEBBLES")};
    QVERIFY(dead);
    all_list->setCurrentItem(dead);
    Settle();
    QVERIFY(LabelWith(explore_tab, "This asset is dead"));
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { Button(explore_tab, "Retire PEBBLES")->click(); }).size(), 1);
    QVERIFY2(status->text().startsWith("PEBBLES is retired"), qPrintable(status->text()));
    QVERIFY(mine());
    Refresh(tabs, 3);
    QVERIFY(!ItemWith(all_list, "PEBBLES"));

    // Explore: GOLD, its market and the history of its description; found by search.
    QListWidgetItem* gold_item{ItemWith(all_list, "GOLD   #")};
    QVERIFY(gold_item);
    all_list->setCurrentItem(gold_item);
    Settle();
    QVERIFY(Shows(explore_tab, "panelTitle", "GOLD"));
    QVERIFY(LabelWith(explore_tab, "Controlled by "));
    QVERIFY(LabelWith(explore_tab, "pool of"));
    auto* history{TableOf(explore_tab, "Field")};
    QVERIFY(history);
    QVERIFY(TableHas(history, 1, "Gold, by the ounce"));
    QVERIFY(TableHas(history, 1, "Gold, by the gram"));
    QVERIFY(TableHas(history, 1, QString(64, 'a')));
    auto* search{EditWithPlaceholder(explore_tab, "Find an asset by its name, number (1739-0029) or hash (0x…)")};
    QVERIFY(search);
    search->setText("zzz");
    QVERIFY(gold_item->isHidden());
    search->setText("gol");
    QVERIFY(!gold_item->isHidden());
    search->setText("GOLD");
    Q_EMIT search->returnPressed();
    Settle();
    QVERIFY(Shows(explore_tab, "panelTitle", "GOLD"));
    Button(explore_tab, "Trade")->click();
    Settle();
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(get->currentData().toString(), gold_arg);

    // Activity: what the wallet did, in words.
    Refresh(tabs, 4);
    auto* activity{TableOf(tabs->widget(4), "When")};
    QVERIFY(activity);
    QVERIFY(activity->rowCount() >= 15);
    QVERIFY(TableHas(activity, 1, "Registered GOLD with 1000.00 GOLD"));
    QVERIFY(TableHas(activity, 1, "Put 10.00 GOLD up for auction"));
    QVERIFY(TableHas(activity, 1, "Retired a dead asset"));
    QVERIFY2(summary->text().startsWith("1 assets · 1 pools · 0 auctions"), qPrintable(summary->text()));
    page.hide();

    // The page in the wallet's window: the Assets page of the wallet's view.
    WalletFrame frame(platform_style.get(), nullptr);
    frame.setClientModel(&client_model);
    auto* view{new WalletView(&wallet_model, platform_style.get(), &frame)};
    QVERIFY(frame.addView(view));
    frame.setCurrentWallet(&wallet_model);
    frame.gotoAssetsPage();
    auto* assets_page{qobject_cast<BitAssetsPage*>(view->currentWidget())};
    QVERIFY(assets_page);
    frame.resize(1200, 800);
    frame.show();
    Settle();
    // It lists what this wallet holds: it knows the wallet by its model.
    auto* view_tabs{assets_page->findChild<QTabWidget*>()};
    QVERIFY(view_tabs);
    QVERIFY(ItemWith(view_tabs->widget(0)->findChild<QListWidget*>("pickList"), "  GOLD   ★"));
    frame.hide();
}

} // namespace

void BitAssetsTests::bitAssetsTests()
{
    // Earlier tests in this binary leave single-shot timers behind (see ConfirmMessage() in
    // qt/test/util.cpp) that click on whatever message box is open when they fire: let them fire
    // now, while there is no message box.
    QTest::qWait(500);
    TestBitAssetsPage(m_node);
}

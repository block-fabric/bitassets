// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/sidechaintests.h>

#include <addresstype.h>
#include <common/args.h>
#include <key.h>
#include <outputtype.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <interfaces/mining.h>
#include <node/cpuminer.h>
#include <qt/bitcoinamountfield.h>
#include <qt/blockexplorer.h>
#include <qt/chainactivity.h>
#include <qt/cryptotoolsdialog.h>
#include <qt/miningdialog.h>
#include <qt/multisigdialog.h>
#include <qt/noderpc.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sidechainnodes.h>
#include <qt/signverifymessagedialog.h>
#include <qt/themedframe.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletcontroller.h>
#include <qt/platformstyle.h>
#include <qt/proofoffundsdialog.h>
#include <qt/sidechainpage.h>
#include <qt/theme.h>
#include <qt/timestampdialog.h>
#include <qt/walletmodel.h>
#include <rpc/server.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CHAINS_TSAN_BUILD
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CHAINS_TSAN_BUILD
#endif

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QInputDialog>
#include <QMainWindow>
#include <QProcess>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>
#include <QFontDatabase>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDir>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>

#include <map>
#include <memory>
#include <atomic>
#include <thread>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::GetWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

//! Regtest drivechain parameters, see CRegTestParams.
constexpr int ACTIVATION_PERIOD{20};
constexpr int WITHDRAWAL_MIN_SCORE{30};

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
            // An information box only has OK.
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

/** Mine a block with the transactions in the mempool. */
void MineBlock(TestChain100Setup& test)
{
    std::vector<CMutableTransaction> txs;
    for (const auto& info : test.m_node.mempool->infoAll()) txs.emplace_back(*info.tx);
    test.CreateAndProcessBlock(txs, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
}

void SaveScreenshot(QWidget& widget, const QString& name)
{
    const QString dir{qEnvironmentVariable("CHAINS_GUI_SCREENSHOT_DIR")};
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    widget.grab().save(QDir(dir).filePath(name + ".png"));
}

/** The windows of the Tools menu. */
void TestTools(TestChain100Setup& test, ClientModel& client_model)
{
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};

    // Hash calculator, merkle tree and address decoder
    CryptoToolsDialog tools;
    tools.setClientModel(&client_model);
    tools.findChild<QPlainTextEdit*>("hashInput")->setPlainText("abc");
    auto* hashes{tools.findChild<QPlainTextEdit*>("hashOutput")};
    QVERIFY(hashes->toPlainText().contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QVERIFY(hashes->toPlainText().contains("4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358"));
    tools.findChild<QCheckBox*>("hashHex")->setChecked(true);
    tools.findChild<QPlainTextEdit*>("hashInput")->setPlainText("616263");
    QVERIFY(hashes->toPlainText().contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const CBlockIndex* tip{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip())};
    tools.findChild<QLineEdit*>("merkleBlock")->setText(QString::number(tip->nHeight));
    tools.findChild<QPushButton*>("merkleLoad")->click();
    const QString root{QString::fromStdString(tip->hashMerkleRoot.GetHex())};
    const QString merkle{tools.findChild<QPlainTextEdit*>("merkleOutput")->toPlainText()};
    QVERIFY(merkle.contains("Merkle root:\n  " + root));
    QVERIFY(merkle.contains("Merkle root in the block header: " + root));

    const QString address{QString::fromStdString(EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}))};
    auto* decode{tools.findChild<QLineEdit*>("decodeAddress")};
    decode->setText(address);
    Q_EMIT decode->returnPressed();
    const QString decoded{tools.findChild<QPlainTextEdit*>("decodeOutput")->toPlainText()};
    QVERIFY(decoded.contains("A valid address"));
    QVERIFY(decoded.contains("prefix: rchn"));
    QVERIFY(decoded.contains(QString::fromStdString(HexStr(test.coinbaseKey.GetPubKey().GetID()))));
    SaveScreenshot(tools, "tools-address");

    // A transaction for the explorer to find
    QString send_error;
    const auto explorer_tx{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({address.toStdString(), 1}), send_error, QString{})};
    QVERIFY2(explorer_tx, qPrintable(send_error));
    const QString sent_txid{QString::fromStdString(explorer_tx->get_str())};
    MineBlock(test);

    // Block explorer
    BlockExplorer explorer;
    explorer.setClientModel(&client_model);
    explorer.refresh();
    auto* blocks{explorer.findChild<QTableWidget*>("explorerBlocks")};
    tip = WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip());
    QCOMPARE(blocks->rowCount(), 50);
    QCOMPARE(blocks->item(0, 0)->text(), QString::number(tip->nHeight));
    QCOMPARE(blocks->item(0, 6)->text(), QString::fromStdString(tip->GetBlockHash().GetHex()));
    QCOMPARE(blocks->item(49, 0)->text(), QString::number(tip->nHeight - 49));
    explorer.search(QString::number(tip->nHeight));
    auto* block_txs{explorer.findChild<QListWidget*>("explorerBlockTxs")};
    QCOMPARE(block_txs->count(), 2);
    QCOMPARE(block_txs->item(1)->text(), sent_txid);
    QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(tip->nHeight)));
    Q_EMIT block_txs->itemClicked(block_txs->item(1));
    QCOMPARE(explorer.findChild<QLabel*>("explorerTitle")->text(), "Transaction " + sent_txid);
    QVERIFY(explorer.findChild<QPlainTextEdit*>("explorerDetails")->toPlainText().contains("\"witness_v0_keyhash\""));
    explorer.search(blocks->item(3, 6)->text());
    QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(tip->nHeight - 3)));
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { explorer.search("not a block"); }).size(), 1);
    SaveScreenshot(explorer, "explorer");

    // Mining
    test.m_node.mining = interfaces::MakeMining(test.m_node);
    test.m_node.cpu_miner = std::make_unique<node::CpuMiner>(test.m_node);
    MiningDialog mining(wallet_name);
    mining.setClientModel(&client_model);
    mining.refresh();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Not mining"));
    QCOMPARE(mining.findChild<QLabel*>("miningHeight")->text(), QString::number(tip->nHeight));
    QVERIFY(!mining.findChild<QPushButton*>("miningStop")->isEnabled());
    mining.findChild<QLineEdit*>("miningAddress")->setText("not an address");
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { mining.findChild<QPushButton*>("miningStart")->click(); }).size(), 1);
    mining.findChild<QLineEdit*>("miningAddress")->setText(address);
    mining.findChild<QSpinBox*>("miningThreads")->setValue(1);
    mining.findChild<QPushButton*>("miningStart")->click();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Mining, threads: 1"));
    QVERIFY(mining.findChild<QPushButton*>("miningStop")->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()) >= tip->nHeight + 2, 30000);
    // The miner counts a block once ProcessNewBlock returns, a moment after the chain shows it.
    QTRY_VERIFY_WITH_TIMEOUT((mining.refresh(), mining.findChild<QLabel*>("miningFound")->text().toInt() >= 2), 10000);
    SaveScreenshot(mining, "mining");
    mining.findChild<QPushButton*>("miningStop")->click();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Not mining"));
    const int height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};
    QTest::qWait(1500);
    QCOMPARE(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()), height);
    {
        // The miner paid the address it was given.
        LOCK(::cs_main);
        const CBlockIndex* mined{test.m_node.chainman->ActiveChain().Tip()};
        CBlock block;
        QVERIFY(test.m_node.chainman->m_blockman.ReadBlock(block, *mined));
        QCOMPARE(block.vtx[0]->vout[0].scriptPubKey, GetScriptForDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}));
    }
    test.m_node.cpu_miner.reset();

    // Timestamp a file, then find its stamp
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write("chains");
    file.close();
    TimestampDialog stamps(wallet_name);
    stamps.setClientModel(&client_model);
    stamps.setFile(file.fileName());
    QCOMPARE(stamps.findChild<QLabel*>("timestampHash")->text(), QString::fromLatin1(QCryptographicHash::hash("chains", QCryptographicHash::Sha256).toHex()));
    auto* stamp_result{stamps.findChild<QPlainTextEdit*>("timestampResult")};
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { stamps.stamp(); }).size(), 1);
    QVERIFY2(stamp_result->toPlainText().startsWith("The fingerprint was published in transaction"), qPrintable(stamp_result->toPlainText()));
    stamps.verify();
    QVERIFY(stamp_result->toPlainText().startsWith("No stamp of this file was found"));
    MineBlock(test);
    stamps.verify();
    const int stamp_height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};
    QVERIFY2(stamp_result->toPlainText().startsWith(QString("This file was stamped in block %1,").arg(stamp_height)), qPrintable(stamp_result->toPlainText()));
    SaveScreenshot(stamps, "timestamp");

    // Proof of funds
    ProofOfFundsDialog proof(wallet_name);
    proof.setClientModel(&client_model);
    proof.findChild<QLineEdit*>("proofStatement")->setText("These coins are mine");
    WithMessageBoxes(QMessageBox::Ok, [&] { proof.prove(); });
    const QString proof_text{proof.findChild<QPlainTextEdit*>("proofOutput")->toPlainText()};
    QVERIFY2(proof_text.startsWith("-----BEGIN CHAINS PROOF OF FUNDS-----\nThese coins are mine\n-----SIGNATURES-----\nrchn1"), qPrintable(proof_text));
    auto* proof_input{proof.findChild<QPlainTextEdit*>("proofInput")};
    auto* verdict{proof.findChild<QPlainTextEdit*>("proofVerdict")};
    proof_input->setPlainText(proof_text);
    proof.verify();
    QVERIFY2(verdict->toPlainText().startsWith("VALID. The holder of"), qPrintable(verdict->toPlainText()));
    QVERIFY(!verdict->toPlainText().contains("with 0 CHN") && !verdict->toPlainText().contains("with 0.00000000 CHN"));
    SaveScreenshot(proof, "proof");
    proof_input->setPlainText(QString{proof_text}.replace("These coins are mine", "These coins are yours"));
    proof.verify();
    QVERIFY2(verdict->toPlainText().startsWith("NOT VALID."), qPrintable(verdict->toPlainText()));
    proof_input->setPlainText("hello");
    proof.verify();
    QCOMPARE(verdict->toPlainText(), QString("This is not a proof of funds."));

    // Multisig: a 2-of-2 address of two keys of the wallet, funded and spent from
    QSettings().remove("MultisigKeys");
    QSettings().remove("MultisigAddresses");
    MultisigDialog multisig(wallet_name);
    multisig.setClientModel(&client_model);
    multisig.addOwnKey();
    multisig.findChild<QLineEdit*>("multisigKeyName")->setText("Second key");
    multisig.addOwnKey();
    auto* keys{multisig.findChild<QTableWidget*>("multisigKeys")};
    QCOMPARE(keys->rowCount(), 2);
    QCOMPARE(keys->item(1, 0)->text(), QString("Second key"));
    QVERIFY(keys->item(0, 1)->text().startsWith("["));
    auto* signers{multisig.findChild<QListWidget*>("multisigSigners")};
    QCOMPARE(signers->count(), 2);
    auto* create_button{multisig.findChild<QPushButton*>("multisigCreate")};
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { create_button->click(); }).size(), 1); // no key ticked
    signers->item(0)->setCheckState(Qt::Checked);
    signers->item(1)->setCheckState(Qt::Checked);
    multisig.findChild<QLineEdit*>("multisigAddressName")->setText("Joint account");
    create_button->click();
    auto* shared{multisig.findChild<QTableWidget*>("multisigAddresses")};
    QCOMPARE(shared->rowCount(), 1);
    QCOMPARE(shared->item(0, 1)->text(), QString("2 of 2"));
    const QString shared_address{shared->item(0, 3)->text()};
    QVERIFY(shared_address.startsWith("rchn1q") && shared_address.size() == 64);

    QString rpc_error;
    QVERIFY2(NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({shared_address.toStdString(), 3}), rpc_error, QString{}), qPrintable(rpc_error));
    MineBlock(test);
    multisig.refreshBalances();
    QCOMPARE(shared->item(0, 2)->text(), QString("3.00000000"));

    multisig.findChild<QLineEdit*>("multisigDestination")->setText(address);
    multisig.findChild<QLineEdit*>("multisigAmount")->setText("1");
    multisig.findChild<QPushButton*>("multisigPrepare")->click();
    auto* psbt_status{multisig.findChild<QLabel*>("multisigPsbtStatus")};
    QVERIFY2(psbt_status->text().startsWith("Signatures: 0 of the 2 needed. Pays 1.00000000 CHN to " + address + ", and 1.99990000 CHN to " + shared_address + ". Fee: 0.00010000 CHN."), qPrintable(psbt_status->text()));
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { multisig.broadcast(); }).size(), 1); // not signed yet
    multisig.sign();
    QVERIFY2(psbt_status->text().startsWith("Signatures: 2 of the 2 needed."), qPrintable(psbt_status->text()));
    SaveScreenshot(multisig, "multisig-keys");
    const QStringList sent{WithMessageBoxes(QMessageBox::Yes, [&] { multisig.broadcast(); })};
    QCOMPARE(sent.size(), 2);
    QVERIFY2(sent.at(1).startsWith("Sent as transaction"), qPrintable(sent.at(1)));
    MineBlock(test);
    multisig.refreshBalances();
    QCOMPARE(shared->item(0, 2)->text(), QString("1.99990000"));
    QSettings().remove("MultisigKeys");
    QSettings().remove("MultisigAddresses");

    // Themes
    // By default the text is a little larger than on the desktop, which takes a style sheet.
    QCOMPARE(Theme::SavedFontSizeAdjustment(), Theme::DEFAULT_FONT_SIZE_ADJUSTMENT);
    Theme::SaveFont({}, 0);
    const QPalette original_palette{QApplication::palette()};
    QCOMPARE(Theme::Available().size(), 18);
    Theme::Apply("midnight");
    QCOMPARE(QApplication::palette().color(QPalette::Window).name(), QString("#0d1117"));
    multisig.findChild<QTabWidget*>("multisigTabs")->setCurrentIndex(2);
    QApplication::processEvents();
    SaveScreenshot(multisig, "theme-midnight");
    Theme::Apply("solar");
    QApplication::processEvents();
    SaveScreenshot(explorer, "theme-gold");
    Theme::Apply("system");
    QCOMPARE(QApplication::palette().color(QPalette::Window), original_palette.color(QPalette::Window));
    QVERIFY(qApp->styleSheet().isEmpty());

    // Looks: the colours of a theme with a font, picked together
    const QList<Theme::Info> looks{Theme::Looks()};
    QCOMPARE(looks.size(), 18);
    QCOMPARE(looks.at(0).id, QString("system"));
    int look_changes{0};
    const auto counting{QObject::connect(Theme::Changes(), &Theme::Signals::changed, [&] { ++look_changes; })};
    Theme::SaveLook("matrix");
    QCOMPARE(look_changes, 1);
    QCOMPARE(Theme::Saved(), QString("matrix"));
    QCOMPARE(Theme::SavedLook(), QString("matrix"));
    QCOMPARE(QApplication::palette().color(QPalette::Window).name(), QString("#000000"));
    // The font of a look is one this computer has, or the one of the desktop.
    QVERIFY(Theme::SavedFontFamily().isEmpty() || QFontDatabase::families().contains(Theme::SavedFontFamily()));
    // Other colours with the same font are a combination of one's own, unless the fonts of both looks are the same here.
    Theme::Save("rose");
    QCOMPARE(look_changes, 2);
    // The size of the text is set apart from the look, and a look leaves it as it is.
    Theme::SaveLook("rose");
    QCOMPARE(Theme::SavedLook(), QString("rose"));
    Theme::SaveFont(Theme::SavedFontFamily(), 5);
    QCOMPARE(Theme::SavedLook(), QString("rose"));
    Theme::SaveLook("matrix");
    QCOMPARE(Theme::SavedFontSizeAdjustment(), 5);
    Theme::SaveFont(Theme::SavedFontFamily(), 0);
    Theme::SaveLook("no such look");
    QCOMPARE(look_changes, 6);
    Theme::SaveLook("system");
    QCOMPARE(Theme::SavedLook(), QString("system"));
    QCOMPARE(Theme::SavedFontFamily(), QString{});
    QCOMPARE(Theme::SavedFontSizeAdjustment(), 0);
    QVERIFY(qApp->styleSheet().isEmpty());
    QObject::disconnect(counting);
}

/** A push button of `parent` by its text. */
QPushButton* Button(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

/**
 * Answer the dialogs that pop up while `action` runs: input dialogs with `answers` in turn (one
 * with none left is cancelled), message boxes with `button`. @return the texts of the message boxes
 */
QStringList AnswerDialogs(QStringList answers, QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible()) continue;
            if (auto* input{qobject_cast<QInputDialog*>(widget)}) {
                if (answers.isEmpty()) {
                    input->reject();
                    continue;
                }
                const QString answer{answers.takeFirst()};
                if (input->inputMode() == QInputDialog::IntInput) {
                    input->setIntValue(answer.toInt());
                } else {
                    input->setTextValue(answer);
                }
                input->accept();
            } else if (auto* box{qobject_cast<QMessageBox*>(widget)}) {
                texts << box->text();
                QAbstractButton* choice{box->button(button)};
                if (!choice) choice = box->button(QMessageBox::Ok);
                if (choice) choice->click();
            }
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

/** The windows and models around the sidechain page: the parts the tests above leave out. */
void TestMoreWindows(TestChain100Setup& test, ClientModel& client_model, WalletModel& wallet_model, OptionsModel& options_model, const PlatformStyle* platform_style)
{
    const int height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};

    // Calls off the GUI thread: the answer comes back on it, to a receiver that is still there.
    {
        QObject receiver;
        bool ran{false};
        NodeRpc::RunAsync(&receiver, [&]() -> std::function<void()> { return [&] { ran = true; }; });
        QTRY_VERIFY_WITH_TIMEOUT(ran, 10000);
        std::optional<UniValue> result;
        QString error;
        bool done{false};
        const auto keep{[&](std::optional<UniValue> r, const QString& e) {
            result = std::move(r);
            error = e;
            done = true;
        }};
        NodeRpc::CallAsync(&receiver, &client_model, "getblockcount", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(result);
        QCOMPARE(result->getInt<int>(), height);
        done = false;
        NodeRpc::CallAsync(&receiver, &client_model, "nosuchcommand", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(!result);
        QCOMPARE(error, QString("Method not found"));
        // Without a node, the answer comes at once.
        done = false;
        NodeRpc::CallAsync(&receiver, nullptr, "getblockcount", UniValue{UniValue::VARR}, keep);
        QVERIFY(done);
        QCOMPARE(error, QString("The node is not available."));
        // A receiver gone before the answer hears nothing.
        bool called{false};
        auto* gone{new QObject};
        NodeRpc::CallAsync(gone, &client_model, "getblockcount", UniValue{UniValue::VARR}, [&](std::optional<UniValue>, const QString&) { called = true; });
        delete gone;
        // The next call is answered after it: by then the first was dropped.
        done = false;
        NodeRpc::CallAsync(&receiver, &client_model, "getblockcount", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(!called);
    }

    // The latest blocks and transactions, fetched off the GUI thread when the window shows.
    {
        ChainActivity activity;
        activity.setClientModel(&client_model);
        auto* blocks{activity.findChild<QTableWidget*>("latestBlocks")};
        auto* transactions{activity.findChild<QTableWidget*>("latestTransactions")};
        activity.show();
        QTRY_VERIFY_WITH_TIMEOUT(blocks->rowCount() > 0, 10000);
        QCOMPARE(blocks->item(0, 0)->text(), QString::number(height));
        QString details;
        QObject::connect(&activity, &ChainActivity::detailsRequested, [&](const QString& hash) { details = hash; });
        Q_EMIT blocks->cellDoubleClicked(0, 1);
        QCOMPARE(details, blocks->item(0, 3)->text());
        // A transaction in the mempool shows on the next refresh; a refresh asked for while one is under way follows it.
        QString send_error;
        const auto sent{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}), 1}), send_error, QString{})};
        QVERIFY2(sent, qPrintable(send_error));
        activity.refresh();
        activity.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(transactions->rowCount() == 1, 10000);
        QCOMPARE(transactions->item(0, 3)->text(), QString::fromStdString(sent->get_str()));
        Q_EMIT transactions->cellDoubleClicked(0, 0);
        QCOMPARE(details, QString::fromStdString(sent->get_str()));
        MineBlock(test);
        activity.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(transactions->rowCount() == 0 && blocks->item(0, 0)->text() == QString::number(height + 1), 10000);
        activity.setClientModel(nullptr);
        activity.refresh();
    }

    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    const NodeRpc::WalletNameFn no_wallet{[] { return std::optional<QString>{}; }};

    // The block explorer fills itself when it shows; a double click on a block or a transaction shows it.
    {
        BlockExplorer explorer;
        explorer.setClientModel(&client_model);
        auto* blocks{explorer.findChild<QTableWidget*>("explorerBlocks")};
        QCOMPARE(blocks->rowCount(), 0);
        explorer.show();
        QVERIFY(blocks->rowCount() > 0);
        Q_EMIT blocks->cellDoubleClicked(1, 0);
        QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(height)));
        QString send_error;
        const auto sent{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}), 1}), send_error, QString{})};
        QVERIFY2(sent, qPrintable(send_error));
        explorer.findChild<QLineEdit*>("explorerSearch")->setText(QString::number(height));
        Q_EMIT explorer.findChild<QLineEdit*>("explorerSearch")->returnPressed();
        QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(height)));
        Button(explorer, "Refresh")->click();
        auto* mempool{explorer.findChild<QTableWidget*>("explorerMempool")};
        QCOMPARE(mempool->rowCount(), 1);
        Q_EMIT mempool->cellDoubleClicked(0, 0);
        QCOMPARE(explorer.findChild<QLabel*>("explorerTitle")->text(), "Transaction " + QString::fromStdString(sent->get_str()));
        QCOMPARE(explorer.findChild<QListWidget*>("explorerBlockTxs")->count(), 0);
        Button(explorer, "Search")->click();
        explorer.hide();
        MineBlock(test);
    }

    // A new address for the miner, from the open wallet; none without one.
    {
        MiningDialog mining(wallet_name);
        mining.setClientModel(&client_model);
        mining.show();
        auto* address{mining.findChild<QLineEdit*>("miningAddress")};
        address->clear();
        Button(mining, "New address")->click();
        QVERIFY2(address->text().startsWith("rchn1"), qPrintable(address->text()));
        MiningDialog without(no_wallet);
        without.setClientModel(&client_model);
        QCOMPARE(AnswerDialogs({}, QMessageBox::Ok, [&] { Button(without, "New address")->click(); }), QStringList{"Open or create a wallet first, or enter an address."});
        mining.setClientModel(nullptr);
    }

    // The tabs of the crypto tools, each opened from the menu.
    {
        CryptoToolsDialog tools;
        auto* tabs{tools.findChild<QTabWidget*>()};
        for (const auto tab : {CryptoToolsDialog::MERKLE, CryptoToolsDialog::ADDRESS, CryptoToolsDialog::HASH}) {
            tools.showTab(tab);
            QCOMPARE(tabs->currentIndex(), int{tab});
            QVERIFY(tools.isVisible());
        }
        tools.hide();
    }

    // Copying a proof of funds.
    {
        ProofOfFundsDialog proof(wallet_name);
        proof.findChild<QPlainTextEdit*>("proofOutput")->setPlainText("a proof");
        Button(proof, "Copy proof")->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("a proof"));
    }

    // Multisig: keys of partners, addresses from a descriptor, removing either, copying them.
    {
        QSettings().remove("MultisigKeys");
        QSettings().remove("MultisigAddresses");
        MultisigDialog multisig(wallet_name);
        multisig.setClientModel(&client_model);
        multisig.show();
        auto* keys{multisig.findChild<QTableWidget*>("multisigKeys")};
        auto* name{multisig.findChild<QLineEdit*>("multisigKeyName")};
        auto* key{multisig.findChild<QLineEdit*>("multisigKey")};
        QCOMPARE(AnswerDialogs({}, QMessageBox::Ok, [&] { Button(multisig, "Add partner")->click(); }), QStringList{"Enter the name of the partner and their public key."});
        name->setText("Alice");
        key->setText("not a key");
        const QStringList refused{AnswerDialogs({}, QMessageBox::Ok, [&] { Button(multisig, "Add partner")->click(); })};
        QCOMPARE(refused.size(), 1);
        QVERIFY2(refused.at(0).startsWith("This is not a public key:"), qPrintable(refused.at(0)));
        const QString alice{QString::fromStdString(HexStr(GenerateRandomKey().GetPubKey()))};
        key->setText(alice);
        Button(multisig, "Add partner")->click();
        QCOMPARE(keys->rowCount(), 1);
        QCOMPARE(keys->item(0, 0)->text(), QString("Alice"));
        QVERIFY(name->text().isEmpty() && key->text().isEmpty());
        multisig.addOwnKey();
        QCOMPARE(keys->rowCount(), 2);
        keys->setCurrentCell(0, 0);
        Button(multisig, "Copy key")->click();
        QCOMPARE(QApplication::clipboard()->text(), alice);
        // (Both tabs have a Remove button: the one of the keys.)
        QPushButton* remove_key{Button(*keys->parentWidget(), "Remove")};
        QVERIFY(remove_key);
        remove_key->click();
        QCOMPARE(keys->rowCount(), 1);
        QCOMPARE(keys->item(0, 0)->text(), QString("Me (default wallet)"));
        keys->setCurrentCell(-1, -1);
        remove_key->click();
        QCOMPARE(keys->rowCount(), 1);

        auto* addresses{multisig.findChild<QTableWidget*>("multisigAddresses")};
        const QString bob{QString::fromStdString(HexStr(GenerateRandomKey().GetPubKey()))};
        const QString descriptor{QStringLiteral("wsh(sortedmulti(1,%1,%2))").arg(alice, bob)};
        // Not a descriptor of this window; cancelled at the name; then added; then once more.
        QCOMPARE(AnswerDialogs({"pkh(" + alice + ")"}, QMessageBox::Ok, [&] { multisig.importAddress(); }), QStringList{"This is not the descriptor of a shared address made in this window."});
        QVERIFY(AnswerDialogs({descriptor}, QMessageBox::Ok, [&] { multisig.importAddress(); }).isEmpty());
        QCOMPARE(addresses->rowCount(), 0);
        QVERIFY(AnswerDialogs({descriptor, "Imported"}, QMessageBox::Ok, [&] { multisig.importAddress(); }).isEmpty());
        QCOMPARE(addresses->rowCount(), 1);
        QCOMPARE(addresses->item(0, 0)->text(), QString("Imported"));
        const QStringList again{AnswerDialogs({descriptor, "Again"}, QMessageBox::Ok, [&] { multisig.importAddress(); })};
        QCOMPARE(again, QStringList{"This shared address is already in the list, as \"Imported\"."});
        addresses->setCurrentCell(0, 0);
        Button(multisig, "Copy address")->click();
        QCOMPARE(QApplication::clipboard()->text(), addresses->item(0, 3)->text());
        Button(multisig, "Copy descriptor")->click();
        QVERIFY(QApplication::clipboard()->text().startsWith(descriptor + "#"));
        multisig.findChild<QPlainTextEdit*>("multisigPsbt")->setPlainText("cHNidP8B");
        Button(multisig, "Copy")->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("cHNidP8B"));
        // Removing asks first.
        QCOMPARE(AnswerDialogs({}, QMessageBox::No, [&] { multisig.removeAddress(); }).size(), 1);
        QCOMPARE(addresses->rowCount(), 1);
        AnswerDialogs({}, QMessageBox::Yes, [&] { multisig.removeAddress(); });
        QCOMPARE(addresses->rowCount(), 0);
        multisig.removeAddress();
        QSettings().remove("MultisigKeys");
        QSettings().remove("MultisigAddresses");
    }

    // The transaction list filters only on what is asked for.
    {
        TransactionFilterProxy filter;
        filter.setSourceModel(wallet_model.getTransactionTableModel());
        const int all{filter.rowCount()};
        QVERIFY(all > 1);
        filter.setShowInactive(false);
        QCOMPARE(filter.rowCount(), all);
        filter.setShowInactive(true);
        const QString txid{filter.index(0, 0).data(TransactionTableModel::TxHashRole).toString()};
        filter.setSearchString(txid.toUpper());
        QVERIFY(filter.rowCount() >= 1 && filter.rowCount() < all);
        filter.setSearchString("no such thing");
        QCOMPARE(filter.rowCount(), 0);
        filter.setSearchString({});
        QCOMPARE(filter.rowCount(), all);
        filter.setDateRange(QDateTime::currentDateTime().addYears(10), std::nullopt);
        QCOMPARE(filter.rowCount(), 0);
        filter.setDateRange(std::nullopt, QDateTime::fromSecsSinceEpoch(0));
        QCOMPARE(filter.rowCount(), 0);
        filter.setDateRange(QDateTime::fromSecsSinceEpoch(0), std::nullopt);
        QCOMPARE(filter.rowCount(), all);
    }

    // Messages signed with a P2WPKH address in the window, and checked.
    {
        SignVerifyMessageDialog sign(platform_style, nullptr);
        sign.setModel(&wallet_model);
        const auto destination{wallet_model.wallet().getNewDestination(OutputType::BECH32, "")};
        QVERIFY(destination);
        const QString address{QString::fromStdString(EncodeDestination(*destination))};
        sign.findChild<QValidatedLineEdit*>("addressIn_SM")->setText(address);
        sign.findChild<QPlainTextEdit*>("messageIn_SM")->setPlainText("signed in the window");
        sign.findChild<QPushButton*>("signMessageButton_SM")->click();
        QCOMPARE(sign.findChild<QLabel*>("statusLabel_SM")->text(), QString("<nobr>Message signed.</nobr>"));
        const QString signature{sign.findChild<QLineEdit*>("signatureOut_SM")->text()};
        QVERIFY(!signature.isEmpty());
        sign.findChild<QValidatedLineEdit*>("addressIn_VM")->setText(address);
        sign.findChild<QPlainTextEdit*>("messageIn_VM")->setPlainText("signed in the window");
        sign.findChild<QValidatedLineEdit*>("signatureIn_VM")->setText(signature);
        sign.findChild<QPushButton*>("verifyMessageButton_VM")->click();
        QCOMPARE(sign.findChild<QLabel*>("statusLabel_VM")->text(), QString("<nobr>Message verified.</nobr>"));
        // An address of scripts stands for no single key, to sign or to verify with.
        const auto taproot{wallet_model.wallet().getNewDestination(OutputType::BECH32M, "")};
        QVERIFY(taproot);
        sign.findChild<QValidatedLineEdit*>("addressIn_SM")->setText(QString::fromStdString(EncodeDestination(*taproot)));
        sign.findChild<QPushButton*>("signMessageButton_SM")->click();
        QVERIFY(sign.findChild<QLabel*>("statusLabel_SM")->text().startsWith("The entered address does not refer to a single key."));
        sign.findChild<QValidatedLineEdit*>("addressIn_VM")->setText(QString::fromStdString(EncodeDestination(*taproot)));
        sign.findChild<QPushButton*>("verifyMessageButton_VM")->click();
        QVERIFY2(!sign.findChild<QLabel*>("statusLabel_VM")->text().contains("Message verified"), qPrintable(sign.findChild<QLabel*>("statusLabel_VM")->text()));
    }

    // The look, the font and the frame, set in the options.
    {
        const bool frame{Theme::SavedFrame()};
        OptionsDialog options(nullptr, /*enableWallet=*/true);
        options.setModel(&options_model);
        options.setCurrentTab(OptionsDialog::TAB_DISPLAY);
        auto* theme{options.findChild<QComboBox*>("theme")};
        theme->setCurrentIndex(theme->findData("midnight"));
        options.findChild<QSpinBox*>("fontSize")->setValue(3);
        options.findChild<QCheckBox*>("themedFrame")->setChecked(!frame);
        WithMessageBoxes(QMessageBox::Ok, [&] { options.findChild<QPushButton*>("okButton")->click(); });
        QCOMPARE(Theme::Saved(), QString("midnight"));
        QCOMPARE(Theme::SavedFontSizeAdjustment(), 3);
        QCOMPARE(Theme::SavedFrame(), !frame);

        // The frame of the main window in the colours of the theme: its button maximizes and restores the window.
        QMainWindow window;
        ThemedFrame themed(&window);
        Theme::SaveFrame(true);
        themed.apply();
        window.show();
        QToolButton* maximize{nullptr};
        for (QToolButton* button : window.findChildren<QToolButton*>()) {
            if (button->toolTip() == "Maximize") maximize = button;
        }
        QVERIFY(maximize);
        maximize->click();
        QTRY_VERIFY(window.isMaximized());
        maximize->click();
        QTRY_VERIFY(!window.isMaximized());
        window.hide();

        Theme::SaveFrame(frame);
        Theme::SaveFont({}, 0);
        Theme::Apply("system");
        Theme::Save("system");
    }
}

/** Run `work` on a thread of its own while the GUI thread goes on with its events (which the work may wait for). */
void OffGuiThread(const std::function<void()>& work)
{
    std::atomic<bool> finished{false};
    std::thread thread{[&] {
        work();
        finished = true;
    }};
    while (!finished) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    thread.join();
}

/** WalletController::getOrCreateWallet from the GUI thread and from others; the models it made. */
void TestWalletController(interfaces::Node& node, ClientModel& client_model, const PlatformStyle* platform_style, WalletContext& context, const std::shared_ptr<CWallet>& wallet)
{
    const auto make_wallet{[&](const std::string& name) {
        auto other{std::make_shared<CWallet>(node.context()->chain.get(), name, CreateMockableWalletDatabase())};
        {
            LOCK(other->cs_wallet);
            other->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            other->SetupDescriptorScriptPubKeyMans();
            const CBlockIndex* tip{WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip())};
            other->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        }
        AddWallet(context, other);
        return other;
    }};
    std::vector<WalletModel*> announced;
    QPointer<WalletModel> late;
    std::shared_ptr<CWallet> third;
    {
        WalletController controller(client_model, platform_style, nullptr);
        QObject::connect(&controller, &WalletController::walletAdded, [&](WalletModel* model) { announced.push_back(model); });
        // The wallet loaded already has its model, the same whichever thread asks.
        const QPointer<WalletModel> first{controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet))};
        QVERIFY(first);
        QCOMPARE(first->parent(), &controller);
        QCOMPARE(controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet)).data(), first.data());
        QPointer<WalletModel> from_thread;
        OffGuiThread([&] { from_thread = controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet)); });
        QCOMPARE(from_thread.data(), first.data());

        // A new wallet from another thread: announced, and given its parent, on the GUI thread.
        const auto second{make_wallet("second")};
        QPointer<WalletModel> second_model;
        OffGuiThread([&] { second_model = controller.getOrCreateWallet(interfaces::MakeWallet(context, second)); });
        QVERIFY(second_model);
        QTRY_COMPARE(second_model->parent(), &controller);
        QVERIFY(std::find(announced.begin(), announced.end(), second_model.data()) != announced.end());
        QCOMPARE(controller.getOrCreateWallet(interfaces::MakeWallet(context, second)).data(), second_model.data());
        // A model being unloaded is no longer the model of its wallet: loaded again, it gets a new one.
        second_model->setUnloading();
        const QPointer<WalletModel> reloaded{controller.getOrCreateWallet(interfaces::MakeWallet(context, second))};
        QVERIFY(reloaded && reloaded.data() != second_model.data());
        // Closing all wallets asks first: not now.
        QCOMPARE(WithMessageBoxes(QMessageBox::Cancel, [&] { controller.closeAllWallets(); }).size(), 1);

        // Registered from another thread, and the controller gone before the GUI thread got to it.
        third = make_wallet("third");
        // (The thread is waited for without the events of the GUI thread: the model stays without its parent.)
        std::thread{[&] { late = controller.getOrCreateWallet(interfaces::MakeWallet(context, third)); }}.join();
        QVERIFY(late);
        QVERIFY(late->parent() != &controller);
    }
    // The controller deleted the model it had not handed out yet.
    QVERIFY(!late);
    QCoreApplication::processEvents();
    RemoveWallet(context, third, /*load_on_start=*/std::nullopt);
    for (const char* name : {"second"}) {
        if (auto other{GetWallet(context, name)}) RemoveWallet(context, other, /*load_on_start=*/std::nullopt);
    }
}

#ifndef WIN32
/** Write an executable shell script. */
void WriteScript(const QString& path, const QString& body)
{
    QFile file{path};
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(("#!/bin/sh\n" + body).toUtf8());
    file.close();
    QVERIFY(file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser));
}

/** A .tar.gz of `files` (name, content), made with tar. @return its path */
QString MakeArchive(const QString& dir, const QString& name, const std::map<QString, QString>& files)
{
    const QString root{QDir(dir).filePath(name + "-content")};
    for (const auto& [path, content] : files) {
        const QString full{QDir(root).filePath(path)};
        QDir().mkpath(QFileInfo(full).absolutePath());
        QFile file{full};
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(content.toUtf8());
        file.close();
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
    }
    const QString archive{QDir(dir).filePath(name + ".tar.gz")};
    if (QProcess::execute("tar", {"-czf", archive, "-C", root, "."}) != 0) return {};
    return archive;
}

QString Sha256(const QString& path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

/**
 * The window that runs the nodes of sidechains, with a sidechain whose programs are shell scripts:
 * the node marks itself running in its data directory, the client answers what the window asks.
 */
void TestSidechainNodes()
{
    QSettings().remove("SidechainNodes");
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    WriteScript(bin.filePath("fakechaind"), R"(dir=""
for a in "$@"; do case "$a" in -datadir=*) dir="${a#-datadir=}" ;; esac; done
echo "$@" > "$dir/args"
mkdir -p "$dir/regtest"
printf 'first line\nsecond line\nthird line\n' > "$dir/regtest/debug.log"
touch "$dir/running"
)");
    WriteScript(bin.filePath("fakechain-cli"), R"(dir=""
cmd=""
args=""
for a in "$@"; do
    case "$a" in
        -datadir=*) dir="${a#-datadir=}" ;;
        -*) ;;
        *) if [ -z "$cmd" ]; then cmd="$a"; else args="$args $a"; fi ;;
    esac
done
echo "$cmd$args" >> "$dir/calls"
if [ ! -f "$dir/running" ]; then echo "error: Could not connect to the server" >&2; exit 1; fi
case "$cmd" in
    getblockcount) echo 42 ;;
    getmainchaininfo) echo '{"connected": true, "height": 105}' ;;
    getbmminfo) if [ -f "$dir/mining" ]; then echo '{"mining": true, "idle": true, "blocks": 3}'; else echo '{"mining": false, "blocks": 0}'; fi ;;
    stop) rm -f "$dir/running" "$dir/loaded"; echo "Fakechain stopping" ;;
    listwallets) if [ -f "$dir/loaded" ]; then echo '["main"]'; else echo '[]'; fi ;;
    createwallet) if [ -f "$dir/wallet" ]; then echo "error: Wallet already exists" >&2; exit 1; fi; touch "$dir/wallet" "$dir/loaded"; echo '{"name": "main"}' ;;
    loadwallet) touch "$dir/loaded"; echo '{"name": "main"}' ;;
    getnewaddress) echo "fakeaddress" ;;
    setbmm) if [ "$args" = " false" ]; then rm -f "$dir/mining"; else touch "$dir/mining"; fi ;;
    requestbmmblock) echo '{"transactions": 4, "amount": 0.0002}' ;;
    *) echo "error: Method not found" >&2; exit 1 ;;
esac
)");

    std::optional<QString> wallet{QString{}};
    SidechainNodesDialog dialog([&] { return wallet; });
    auto* table{dialog.findChild<QTableWidget*>("sidechainNodes")};
    auto* log{dialog.findChild<QPlainTextEdit*>("sidechainNodesLog")};
    const auto click{[&](const char* name) { dialog.findChild<QPushButton*>(name)->click(); }};
    const auto logged{[&](const QString& text) { return log->toPlainText().contains(text); }};
    const auto boxes{[&](const char* name, QMessageBox::StandardButton button = QMessageBox::Ok) { return AnswerDialogs({}, button, [&] { click(name); }); }};
    dialog.show();
    QCOMPARE(table->rowCount(), 4);
    QVERIFY(table->item(0, 6)->text().startsWith("not on this computer yet"));
    // This node takes no RPC connections (no -server in the tests): the window says so.
    QCOMPARE(dialog.findChild<QLabel*>("sidechainNodesNotice")->isVisible(), !gArgs.GetBoolArg("-server", false));

    // Nothing selected. (The table takes a current row when it gets the focus back from a message box.)
    const auto unselected{[&](const char* name) {
        table->setCurrentCell(-1, -1);
        return boxes(name);
    }};
    QCOMPARE(unselected("locate"), QStringList{"Select a sidechain in the list first."});
    QCOMPARE(unselected("download"), QStringList{"Select a sidechain in the list first."});
    QCOMPARE(unselected("removeSidechain"), QStringList{"Select the sidechain to remove."});
    QCOMPARE(unselected("startNode"), QStringList{"Select a sidechain whose programs are on this computer."});
    QCOMPARE(unselected("startWallet"), QStringList{"Select a sidechain whose programs are on this computer."});
    QCOMPARE(unselected("mineOnce"), QStringList{"Select a sidechain whose node is running."});
    QCOMPARE(unselected("toggleMining"), QStringList{"Select a sidechain whose node is running."});
    QVERIFY(unselected("stopNode").isEmpty());
    QVERIFY(unselected("showLog").isEmpty());

    // Add a sidechain; not twice; cancelled half way, not at all.
    QVERIFY(AnswerDialogs({"Fakechain", "fakechain", "7"}, QMessageBox::Ok, [&] { click("addSidechain"); }).isEmpty());
    QCOMPARE(table->rowCount(), 5);
    QCOMPARE(table->item(4, 0)->text(), QString("Fakechain"));
    QCOMPARE(table->item(4, 1)->text(), QString("7"));
    QCOMPARE(AnswerDialogs({"Other", "fakechain", "8"}, QMessageBox::Ok, [&] { click("addSidechain"); }), QStringList{"A sidechain with these programs is in the list already."});
    QVERIFY(AnswerDialogs({"Other", "otherchain"}, QMessageBox::Ok, [&] { click("addSidechain"); }).isEmpty());
    QVERIFY(AnswerDialogs({}, QMessageBox::Ok, [&] { click("addSidechain"); }).isEmpty());
    QCOMPARE(table->rowCount(), 5);
    table->setCurrentCell(4, 0);
    QCOMPARE(boxes("startNode"), QStringList{"Select a sidechain whose programs are on this computer."});

    // Where its programs are (what Locate records, from a folder the user picks).
    QSettings().setValue("SidechainNodes/bindir/fakechain", bin.path());
    dialog.refresh();
    QCOMPARE(table->item(4, 6)->text(), bin.path());
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 2)->text(), QString("stopped"), 10000);

    // Start it: the node is pointed at this one.
    table->setCurrentCell(4, 0);
    click("startNode");
    QVERIFY(logged("Fakechain: node started"));
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 2)->text(), QString("running"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 3)->text(), QString("42"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 4)->text(), QString("yes, at block 105"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 5)->text(), QString("off"), 10000);
    const QString data_dir{GUIUtil::PathToQString(gArgs.GetDataDirBase() / "sidechains" / "fakechain" / "data")};
    QFile args_file{QDir(data_dir).filePath("args")};
    QVERIFY(args_file.open(QIODevice::ReadOnly));
    const QString args{QString::fromUtf8(args_file.readAll())};
    for (const QString& arg : QStringList{QString{"-datadir="} + data_dir, QString("-regtest"), QString("-sidechainslot=7"), QString("-mainchainrpcport="), QString("-mainchainrpccookiefile="), QString("-mainchainrpcwallet="), QString("-daemon")}) {
        QVERIFY2(args.contains(arg), qPrintable(args));
    }
    click("startNode");
    QVERIFY(logged("Fakechain is running already."));
    const QStringList busy{boxes("startWallet")};
    QCOMPARE(busy.size(), 1);
    QVERIFY(busy.at(0).startsWith("The node of Fakechain is running in the background. Stop it first"));

    // Mining needs an open wallet of this node, which pays for the blocks.
    wallet.reset();
    QVERIFY(boxes("toggleMining").at(0).startsWith("Open a wallet of this node first"));
    QVERIFY(boxes("mineOnce").at(0).startsWith("Open a wallet of this node first"));
    wallet = QString{};
    click("toggleMining");
    QTRY_VERIFY_WITH_TIMEOUT(logged("Fakechain: automatic mining turned on; fees go to fakeaddress"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 5)->text(), QString("auto, waiting for fees, 3 blocks"), 10000);
    click("toggleMining");
    QTRY_VERIFY_WITH_TIMEOUT(logged("Fakechain: mining turned off"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 5)->text(), QString("off"), 10000);
    AnswerDialogs({"0.0002"}, QMessageBox::Ok, [&] {
        click("mineOnce");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Fakechain: asked for one block with 3 transactions, offering 0.00020000."), 10000);
    });
    // Cancelled: nothing asked.
    AnswerDialogs({}, QMessageBox::Ok, [&] { click("mineOnce"); });

    // The end of its log; the first line, which may have been cut, left out.
    click("showLog");
    QVERIFY(logged("--- the end of"));
    QVERIFY(logged("third line"));
    QVERIFY(!logged("first line"));

    // Not removed while it runs. Stopped; stopping it again fails.
    QCOMPARE(boxes("removeSidechain"), QStringList{"The node of Fakechain is running. Stop it first."});
    click("stopNode");
    QTRY_VERIFY_WITH_TIMEOUT(logged("Fakechain: stopping"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 2)->text(), QString("stopped"), 10000);
    QCOMPARE(table->item(4, 3)->text(), QString{});
    QCOMPARE(boxes("mineOnce"), QStringList{"Select a sidechain whose node is running."});
    click("stopNode");
    QTRY_VERIFY_WITH_TIMEOUT(logged("Fakechain: not stopped. error: Could not connect to the server"), 10000);

    // Started again, its wallet exists but is not loaded: it is loaded for mining.
    click("startNode");
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 2)->text(), QString("running"), 10000);
    click("toggleMining");
    QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().count("automatic mining turned on") == 2, 10000);
    QFile calls{QDir(data_dir).filePath("calls")};
    QVERIFY(calls.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(calls.readAll()).contains("loadwallet main"));
    click("stopNode");
    QTRY_COMPARE_WITH_TIMEOUT(table->item(4, 2)->text(), QString("stopped"), 10000);
    // It has no wallet program.
    click("startWallet");
    QVERIFY(logged("Fakechain: the wallet could not be started"));

    // A sidechain that comes with the wallet stays; one added by hand goes, once confirmed.
    table->setCurrentCell(0, 0);
    QVERIFY(boxes("removeSidechain").at(0).startsWith("Thunder comes with this wallet and stays in the list."));
    table->setCurrentCell(4, 0);
    QCOMPARE(boxes("removeSidechain", QMessageBox::Cancel).size(), 1);
    QCOMPARE(table->rowCount(), 5);
    QCOMPARE(boxes("removeSidechain", QMessageBox::Yes).size(), 1);
    QCOMPARE(table->rowCount(), 4);
    QVERIFY(logged("Fakechain was taken off the list."));
    QVERIFY(!QSettings().contains("SidechainNodes/bindir/fakechain"));

    // Download a release of Thunder: checked against the hash given before it is unpacked.
    table->setCurrentCell(0, 0);
    QTemporaryDir downloads;
    const QString release{MakeArchive(downloads.path(), "release", {{"thunder-1.0/bin/thunderd", "#!/bin/sh\n"}})};
    const QString no_node{MakeArchive(downloads.path(), "no-node", {{"README", "nothing here"}})};
    QVERIFY(!release.isEmpty() && !no_node.isEmpty());
    const QString not_archive{QDir(downloads.path()).filePath("not-an-archive.tar.gz")};
    {
        QFile file{not_archive};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is no archive");
    }
    const auto url{[](const QString& path) { return QUrl::fromLocalFile(path).toString(); }};
    QCOMPARE(AnswerDialogs({url(release), "abc"}, QMessageBox::Ok, [&] { click("download"); }),
             QStringList{"A SHA-256 hash is 64 hexadecimal characters. Programs that hold coins are not installed unchecked."});
    QVERIFY(AnswerDialogs({}, QMessageBox::Ok, [&] { click("download"); }).isEmpty());
    QVERIFY(AnswerDialogs({url(release)}, QMessageBox::Ok, [&] { click("download"); }).isEmpty());
    const QStringList mismatch{AnswerDialogs({url(release), QString(64, 'a')}, QMessageBox::Ok, [&] {
        click("download");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Thunder: the download does NOT have the expected hash and was thrown away."), 10000);
    })};
    QCOMPARE(mismatch, QStringList{"The download does not have the hash you gave. It was thrown away."});
    AnswerDialogs({url(QDir(downloads.path()).filePath("missing.tar.gz")), Sha256(release)}, QMessageBox::Ok, [&] {
        click("download");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Thunder: the download failed."), 10000);
    });
    AnswerDialogs({url(not_archive), Sha256(not_archive)}, QMessageBox::Ok, [&] {
        click("download");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Thunder: unpacking failed."), 10000);
    });
    AnswerDialogs({url(no_node), Sha256(no_node)}, QMessageBox::Ok, [&] {
        click("download");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Thunder: the archive has no thunderd in it."), 10000);
    });
    {
        // Without curl.
        const QByteArray path{qgetenv("PATH")};
        qputenv("PATH", "");
        AnswerDialogs({url(release), Sha256(release)}, QMessageBox::Ok, [&] { click("download"); });
        qputenv("PATH", path);
        QTRY_VERIFY_WITH_TIMEOUT(logged("The download needs the program curl, which could not be started."), 10000);
    }
    AnswerDialogs({url(release), Sha256(release)}, QMessageBox::Ok, [&] {
        click("download");
        QTRY_VERIFY_WITH_TIMEOUT(logged("Thunder: installed in"), 10000);
    });
    const QString installed{QSettings().value("SidechainNodes/bindir/thunder").toString()};
    QVERIFY2(installed.endsWith("/sidechains/thunder/thunder-1.0/bin"), qPrintable(installed));
    QCOMPARE(table->item(0, 6)->text(), installed);
    // It has no client program: the window cannot reach the node, which counts as stopped.
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 2)->text(), QString("stopped"), 10000);

    dialog.setClientModel(nullptr);
    dialog.hide();
    QSettings().remove("SidechainNodes");
}
#endif // WIN32

void TestSidechainPage(interfaces::Node& node)
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    // The page works through RPC, including the wallet commands.
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    // A wallet holding the coinbase key of the test chain.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, /*require_checksum=*/false);
        QVERIFY(descs.size() == 1);
        WalletDescriptor w_desc(std::move(descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(w_desc, provider, "", false));
        wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    wallet->SetBroadcastTransactions(true);
    // There is no fee estimate on the test chain.
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());

    SidechainPage page(platform_style.get());
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.setWalletModel(&wallet_model);
    page.show();

    auto* tabs{page.findChild<QTabWidget*>("sidechainTabs")};
    auto* sidechains{page.findChild<QTableWidget*>("sidechains")};
    auto* proposals{page.findChild<QTableWidget*>("proposals")};
    auto* queued{page.findChild<QTableWidget*>("queuedProposals")};
    auto* bundles{page.findChild<QTableWidget*>("bundles")};
    QVERIFY(tabs && sidechains && proposals && queued && bundles);
    QCOMPARE(sidechains->rowCount(), 0);

    // Propose a sidechain.
    tabs->setCurrentIndex(1);
    page.findChild<QSpinBox*>("proposalSlot")->setValue(3);
    page.findChild<QLineEdit*>("proposalTitle")->setText("Testchain");
    page.findChild<QLineEdit*>("proposalDescription")->setText("A sidechain made by the GUI test");
    // No message box: the proposal is accepted.
    const QStringList propose_boxes{WithMessageBoxes(QMessageBox::Ok, [&] { page.findChild<QPushButton*>("proposeButton")->click(); })};
    QVERIFY2(propose_boxes.isEmpty(), qPrintable(propose_boxes.join(" | ")));
    QCOMPARE(queued->rowCount(), 1);
    QCOMPARE(queued->item(0, 1)->text(), QString("Testchain"));
    QCOMPARE(proposals->rowCount(), 0);

    // An empty title is refused by the node; the page shows the error and stays as it is.
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { page.findChild<QPushButton*>("proposeButton")->click(); }).size(), 1);
    QCOMPARE(queued->rowCount(), 1);

    // The next block carries the proposal; this node acks it.
    MineBlock(test);
    page.refresh();
    QCOMPARE(queued->rowCount(), 0);
    QCOMPARE(proposals->rowCount(), 1);
    QCOMPARE(proposals->item(0, 0)->text(), QString("3"));
    QCOMPARE(proposals->item(0, 7)->text(), QString("yes"));
    SaveScreenshot(page, "sidechains-proposals");

    // Stop acking and ack again.
    proposals->setCurrentCell(0, 0);
    page.findChild<QPushButton*>("nackButton")->click();
    QCOMPARE(proposals->item(0, 7)->text(), QString("no"));
    UniValue ack_params{UniValue::VARR};
    ack_params.push_back(proposals->item(0, 8)->text().toStdString());
    node.executeRpc("acksidechain", ack_params, "/");

    for (int i{1}; i < ACTIVATION_PERIOD; ++i) MineBlock(test);
    page.refresh();
    QCOMPARE(proposals->rowCount(), 0);
    QCOMPARE(sidechains->rowCount(), 1);
    QCOMPARE(sidechains->item(0, 1)->text(), QString("Testchain"));

    // Deposit to it.
    tabs->setCurrentIndex(0);
    auto* deposit_button{page.findChild<QPushButton*>("depositButton")};
    QVERIFY(!deposit_button->isEnabled());
    sidechains->setCurrentCell(0, 0);
    QVERIFY(deposit_button->isEnabled());
    // A deposit address with a typing error is refused before anything is asked.
    auto* destination{page.findChild<QLineEdit*>("depositDestination")};
    destination->setText("s3_alice_000000");
    page.findChild<BitcoinAmountField*>("depositAmount")->setValue(5 * COIN);
    {
        const QStringList refused{WithMessageBoxes(QMessageBox::Yes, [&] { page.findChild<QPushButton*>("depositButton")->click(); })};
        QCOMPARE(refused.size(), 1);
        QVERIFY2(refused.at(0).contains("typing error"), qPrintable(refused.at(0)));
    }
    // One for another sidechain too.
    destination->setText(QString::fromStdString(drivechain::FormatDepositAddress({7, "alice"})));
    {
        const QStringList refused{WithMessageBoxes(QMessageBox::Yes, [&] { page.findChild<QPushButton*>("depositButton")->click(); })};
        QCOMPARE(refused.size(), 1);
        QVERIFY2(refused.at(0).contains("is for the sidechain in slot 7"), qPrintable(refused.at(0)));
    }
    destination->setText(QString::fromStdString(drivechain::FormatDepositAddress({3, "alice"})));
    page.findChild<BitcoinAmountField*>("depositAmount")->setValue(5 * COIN);
    // A question to confirm, then the transaction id.
    const QStringList deposit_boxes{WithMessageBoxes(QMessageBox::Yes, [&] { deposit_button->click(); })};
    QCOMPARE(deposit_boxes.size(), 2);
    QVERIFY2(deposit_boxes[1].startsWith("Deposit sent"), qPrintable(deposit_boxes[1]));
    QCOMPARE(test.m_node.mempool->size(), 1U);
    MineBlock(test);
    page.refresh();
    QVERIFY(sidechains->item(0, 2)->text().startsWith("5.00"));
    SaveScreenshot(page, "sidechains-deposit");

    // A withdrawal bundle handed to the node shows up after the next block, and can be voted on.
    CMutableTransaction bundle;
    bundle.vout.emplace_back(0, drivechain::WithdrawalFeeScript(1000));
    bundle.vout.emplace_back(2 * COIN, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    UniValue bundle_params{UniValue::VARR};
    bundle_params.push_back(3);
    bundle_params.push_back(EncodeHexTx(CTransaction{bundle}));
    node.executeRpc("receivewithdrawalbundle", bundle_params, "/");
    MineBlock(test);
    tabs->setCurrentIndex(2);
    page.refresh();
    QCOMPARE(bundles->rowCount(), 1);
    // The node was handed the bundle, so it upvotes it by default; the buttons say otherwise or the same.
    QCOMPARE(bundles->item(0, 5)->text(), QString("upvote"));
    bundles->setCurrentCell(0, 0);
    page.findChild<QPushButton*>("downvoteButton")->click();
    QCOMPARE(bundles->item(0, 5)->text(), QString("downvote"));
    bundles->setCurrentCell(0, 0);
    page.findChild<QPushButton*>("upvoteButton")->click();
    QCOMPARE(bundles->item(0, 5)->text(), QString("upvote"));

    for (int i{1}; i < WITHDRAWAL_MIN_SCORE; ++i) MineBlock(test);
    page.refresh();
    QCOMPARE(bundles->rowCount(), 1);
    QCOMPARE(bundles->item(0, 3)->text(), QString("yes"));
    SaveScreenshot(page, "sidechains-withdrawals");

    // Paying the bundle out is the business of the block assembler; see feature_drivechain.py.
    tabs->setCurrentIndex(0);
    page.refresh();
    QVERIFY(sidechains->item(0, 2)->text().startsWith("5.00"));
    QCOMPARE(sidechains->item(0, 3)->text(), QString("1"));

    // (Before the tools: their miner pays the bundle out.)
    // The queued proposals and the vote buttons the tests above do not press.
    tabs->setCurrentIndex(1);
    page.findChild<QSpinBox*>("proposalSlot")->setValue(5);
    page.findChild<QLineEdit*>("proposalTitle")->setText("Queued");
    WithMessageBoxes(QMessageBox::Ok, [&] { page.findChild<QPushButton*>("proposeButton")->click(); });
    QCOMPARE(queued->rowCount(), 1);
    queued->setCurrentCell(0, 0);
    Button(page, "Re&move")->click();
    QCOMPARE(queued->rowCount(), 0);
    // Nothing selected: nothing removed, nothing acked.
    Button(page, "Re&move")->click();
    Button(page, "&Ack")->click();
    tabs->setCurrentIndex(2);
    page.refresh();
    QCOMPARE(bundles->rowCount(), 1);
    bundles->setCurrentCell(0, 0);
    Button(page, "&Abstain")->click();
    QCOMPARE(bundles->item(0, 5)->text(), QString("abstain"));
    bundles->setCurrentCell(0, 0);
    Button(page, "Use de&fault")->click();
    QCOMPARE(bundles->item(0, 5)->text(), QString("upvote"));
    // Another unit: the page shows amounts in it.
    options_model.setDisplayUnit(QVariant::fromValue(BitcoinUnit::mBTC));
    options_model.setDisplayUnit(QVariant::fromValue(BitcoinUnit::BTC));
    // A new block reloads a page someone is looking at.
    MineBlock(test);
    Q_EMIT client_model.numBlocksChanged(0, {}, 0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);

    TestTools(test, client_model);

    TestMoreWindows(test, client_model, wallet_model, options_model, platform_style.get());
    TestWalletController(node, client_model, platform_style.get(), context, wallet);

    RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
}

//! ChainActivity::Fetch looks up only what it does not know yet, and a reply of an unexpected shape
//! leaves a table as it was instead of throwing out of a Qt slot (which terminates).
void TestChainActivityFetch()
{
    std::map<std::string, int> calls;
    bool malformed{false};
    const ChainActivity::CallFn call{[&](const std::string& method, const UniValue& params) -> std::optional<UniValue> {
        ++calls[method];
        UniValue result;
        if (method == "getbestblockhash") return UniValue{"b2"};
        if (method == "getblockheader") {
            const std::string hash{params[0].get_str()};
            if (!result.read(hash == "b3" ? (malformed ? R"({"height":3,"time":"soon","nTx":1,"previousblockhash":"b2"})" : R"({"height":3,"time":300,"nTx":1,"previousblockhash":"b2"})")
                      : hash == "b2" ? R"({"height":2,"time":200,"nTx":2,"previousblockhash":"b1"})"
                                     : R"({"height":1,"time":100,"nTx":1})")) return std::nullopt;
            return result;
        }
        if (method == "getrawmempool") {
            if (!result.read(R"(["t1", 5, "t2"])")) return std::nullopt;
            return result;
        }
        if (method == "getmempoolentry") {
            if (!result.read(params[0].get_str() == "t1" ? R"({"time":10,"fees":{"base":0.0001},"vsize":150})" : R"({"time":"x"})")) return std::nullopt;
            return result;
        }
        return std::nullopt;
    }};
    ChainActivity::Snapshot first{ChainActivity::Fetch(call, {})};
    QCOMPARE(first.blocks.size(), size_t{2});
    QCOMPARE(first.blocks[0].hash, QString{"b2"});
    QCOMPARE(first.blocks[1].height, QString{"1"});
    QCOMPARE(calls["getblockheader"], 2);
    // The malformed entry is left out.
    QCOMPARE(first.mempool.size(), size_t{1});
    QVERIFY(first.mempool.contains("t1"));
    QCOMPARE(calls["getmempoolentry"], 2);

    // Nothing new: no block and no transaction looked up again (t2 is, being still unknown).
    calls.clear();
    const ChainActivity::Snapshot second{ChainActivity::Fetch(call, first)};
    QCOMPARE(second.blocks.size(), size_t{2});
    QCOMPARE(calls["getblockheader"], 0);
    QCOMPARE(calls["getmempoolentry"], 1);
    QCOMPARE(second.mempool.size(), size_t{1});

    // A malformed header keeps the blocks shown last time.
    malformed = true;
    const ChainActivity::CallFn new_tip{[&](const std::string& method, const UniValue& params) -> std::optional<UniValue> {
        if (method == "getbestblockhash") return UniValue{"b3"};
        return call(method, params);
    }};
    const ChainActivity::Snapshot third{ChainActivity::Fetch(new_tip, second)};
    QCOMPARE(third.blocks.size(), size_t{2});
    QCOMPARE(third.blocks[0].hash, QString{"b2"});
    malformed = false;
    calls.clear();
    const ChainActivity::Snapshot fourth{ChainActivity::Fetch(new_tip, second)};
    QCOMPARE(fourth.blocks.size(), size_t{3});
    QCOMPARE(fourth.blocks[0].height, QString{"3"});
    // Only the new block was looked up.
    QCOMPARE(calls["getblockheader"], 1);
}

} // namespace

void SidechainTests::sidechainTests()
{
    // Earlier tests in this binary leave single-shot timers behind (see
    // ConfirmMessage() in qt/test/util.cpp) that click on whatever message box
    // is open when they fire and store its text through a pointer that is no
    // longer valid. Let them fire now, while there is no message box.
    QTest::qWait(500);
    // AppTests shut a node down before, which stopped the calls off the GUI thread.
    NodeRpc::Restart();
    TestChainActivityFetch();
    TestSidechainPage(m_node);
#if !defined(WIN32) && !defined(CHAINS_TSAN_BUILD)
    // Not under ThreadSanitizer: QProcess starts programs through a vfork-style
    // clone(CLONE_VM), which the TSan runtime cannot follow (it aborts with
    // "CHECK failed: tsan_rtl.cpp ... ((!thr->slot)) != (0)").
    TestSidechainNodes();
#endif
}

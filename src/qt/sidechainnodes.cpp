// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sidechainnodes.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <crypto/sha256.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
constexpr int REFRESH_INTERVAL_MS{4000};
const char* const SETTING_CUSTOM{"SidechainNodes/custom"};
enum { COL_NAME, COL_SLOT, COL_STATUS, COL_BLOCKS, COL_MAINCHAIN, COL_MINING, COL_PROGRAMS };

#ifdef WIN32
const QString EXE{QStringLiteral(".exe")};
#else
const QString EXE{};
#endif

QString SettingKey(const QString& stem) { return QStringLiteral("SidechainNodes/bindir/%1").arg(stem); }

/**
 * Start a program that keeps running when this one ends, and that shares
 * nothing with it: a program started the plain way inherits what this one has
 * open, the ports this node listens on among it, and would keep them open
 * after this node has stopped.
 */
bool Launch(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
#ifndef WIN32
    // Whatever this program has open now is closed in the program that is started.
    const long limit{sysconf(_SC_OPEN_MAX)};
    for (int fd{3}; fd < (limit > 0 ? limit : 4096); ++fd) {
        const int flags{fcntl(fd, F_GETFD)};
        if (flags != -1) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif
    return process.startDetached();
}

/** SHA-256 of a file, in hexadecimal; empty if it cannot be read. */
QString FileHash(const QString& path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) return {};
    CSHA256 hasher;
    while (!file.atEnd()) {
        const QByteArray chunk{file.read(1 << 20)};
        hasher.Write(reinterpret_cast<const unsigned char*>(chunk.constData()), chunk.size());
    }
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(hash);
    return QString::fromStdString(HexStr(hash));
}
} // namespace

SidechainNodesDialog::SidechainNodesDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Sidechain Nodes"));
    auto* layout{new QVBoxLayout(this)};
    auto* intro{new QLabel(tr("Run the nodes of sidechains from here. A sidechain node follows this node, mines its blocks through the "
                              "miners of this chain, and holds the coins you deposit to the sidechain."), this)};
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_notice = new QLabel(this);
    m_notice->setObjectName("sidechainNodesNotice");
    m_notice->setWordWrap(true);
    layout->addWidget(m_notice);

    m_table = new QTableWidget(0, 7, this);
    m_table->setObjectName("sidechainNodes");
    m_table->setHorizontalHeaderLabels({tr("Sidechain"), tr("Slot"), tr("Node"), tr("Blocks"), tr("Follows this node"), tr("Mining"), tr("Programs")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 2);

    // Two rows: what is done with a node, and what the list is made of.
    auto* buttons{new QHBoxLayout};
    auto* setup_buttons{new QHBoxLayout};
    QHBoxLayout* row{buttons};
    const auto button{[&](const QString& text, const char* name, const QString& tip, void (SidechainNodesDialog::*slot)()) {
        auto* b{new QPushButton(text, this)};
        b->setObjectName(name);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, slot);
        row->addWidget(b);
    }};
    button(tr("Start node"), "startNode", tr("Start the node of the selected sidechain in the background"), &SidechainNodesDialog::startNode);
    button(tr("Open wallet"), "startWallet", tr("Start the selected sidechain with its wallet window, in place of the background node"), &SidechainNodesDialog::startWallet);
    button(tr("Stop"), "stopNode", tr("Stop the node of the selected sidechain"), &SidechainNodesDialog::stopNode);
    button(tr("Auto mining on / off"), "toggleMining", tr("Have the selected sidechain ask for a block whenever its transactions carry fees that pay for one. It offers the miners of this chain 99% of those fees, from the open wallet of this node, and keeps the fees; without fees it asks for nothing."), &SidechainNodesDialog::toggleMining);
    button(tr("Mine one block…"), "mineOnce", tr("Have the selected sidechain mine one block now, empty or not, for a fee you choose, paid by the open wallet of this node. This is how a deposit gets paid when the sidechain has no transactions."), &SidechainNodesDialog::mineOnce);
    button(tr("Log"), "showLog", tr("Show the end of the log of the node"), &SidechainNodesDialog::showLog);
    buttons->addStretch();
    row = setup_buttons;
    button(tr("Locate programs…"), "locate", tr("Tell where the programs of the selected sidechain are on this computer"), &SidechainNodesDialog::locate);
    button(tr("Download…"), "download", tr("Download a release of the selected sidechain and check it"), &SidechainNodesDialog::download);
    button(tr("Add sidechain…"), "addSidechain", tr("List a sidechain that is not in the list"), &SidechainNodesDialog::addSidechain);
    button(tr("Remove"), "removeSidechain", tr("Take the selected sidechain off this list. Only for a sidechain that was added by hand; its programs and its data stay on the computer."), &SidechainNodesDialog::removeSidechain);
    setup_buttons->addStretch();
    layout->addLayout(buttons);
    layout->addLayout(setup_buttons);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName("sidechainNodesLog");
    m_log->setReadOnly(true);
    m_log->setFont(GUIUtil::fixedPitchFont());
    m_log->setMaximumBlockCount(500);
    layout->addWidget(m_log, 1);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SidechainNodesDialog::refresh);
    resize(1000, 600);
    GUIUtil::handleCloseWindowShortcut(this);
}

void SidechainNodesDialog::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) m_timer->stop();
}

void SidechainNodesDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
    m_timer->start(REFRESH_INTERVAL_MS);
}

void SidechainNodesDialog::hideEvent(QHideEvent* event)
{
    QDialog::hideEvent(event);
    m_timer->stop();
}

QList<SidechainNodesDialog::Sidechain> SidechainNodesDialog::sidechains() const
{
    QList<Sidechain> list{
        {QStringLiteral("Thunder"), QStringLiteral("thunder"), 2, tr("Large blocks, for volume")},
        {QStringLiteral("Hivemind"), QStringLiteral("hivemind"), 9, tr("Prediction markets")},
        {QStringLiteral("BitNames"), QStringLiteral("bitnames"), 3, tr("Names, and their .x domains")},
        {tr("Sidechain template"), QStringLiteral("sidechain"), 0, tr("A sidechain with nothing but what every sidechain needs")},
    };
    const QStringList custom{QSettings().value(SETTING_CUSTOM).toStringList()};
    for (const QString& entry : custom) {
        const QStringList parts{entry.split('\t')};
        if (parts.size() == 3) list.push_back({parts[0], parts[1], parts[2].toInt(), tr("Added by you")});
    }
    return list;
}

QString SidechainNodesDialog::binDir(const QString& stem) const
{
    return QSettings().value(SettingKey(stem)).toString();
}

void SidechainNodesDialog::setBinDir(const QString& stem, const QString& dir)
{
    QSettings().setValue(SettingKey(stem), dir);
}

QString SidechainNodesDialog::program(const Sidechain& sidechain, const QString& suffix) const
{
    return QDir(binDir(sidechain.stem)).filePath(sidechain.stem + suffix + EXE);
}

QString SidechainNodesDialog::dataDir(const Sidechain& sidechain) const
{
    // The nodes run from here keep their data with the data of this node, apart from
    // whatever else on this computer goes by the name of the sidechain.
    return GUIUtil::PathToQString(gArgs.GetDataDirBase() / "sidechains") + QLatin1Char('/') + sidechain.stem + QStringLiteral("/data");
}

QStringList SidechainNodesDialog::networkArgs(const Sidechain& sidechain) const
{
    // The node of a sidechain is on the network that this node is on.
    QStringList args{QStringLiteral("-datadir=%1").arg(dataDir(sidechain))};
    switch (Params().GetChainType()) {
    case ChainType::MAIN: break;
    case ChainType::TESTNET: args << QStringLiteral("-testnet"); break;
    case ChainType::SIGNET: args << QStringLiteral("-signet"); break;
    case ChainType::REGTEST: args << QStringLiteral("-regtest") << QStringLiteral("-sidechainslot=%1").arg(sidechain.slot); break;
    }
    return args;
}

QStringList SidechainNodesDialog::nodeArgs(const Sidechain& sidechain) const
{
    // The node is pointed at this node.
    QStringList args{networkArgs(sidechain)};
    args << QStringLiteral("-mainchainrpcport=%1").arg(gArgs.GetIntArg("-rpcport", BaseParams().RPCPort()));
    if (gArgs.IsArgSet("-rpcpassword")) {
        args << QStringLiteral("-mainchainrpcuser=%1").arg(QString::fromStdString(gArgs.GetArg("-rpcuser", "")));
        args << QStringLiteral("-mainchainrpcpassword=%1").arg(QString::fromStdString(gArgs.GetArg("-rpcpassword", "")));
    } else {
        args << QStringLiteral("-mainchainrpccookiefile=%1").arg(GUIUtil::PathToQString(gArgs.GetDataDirNet() / ".cookie"));
    }
    if (const auto wallet{m_wallet_name()}) args << QStringLiteral("-mainchainrpcwallet=%1").arg(*wallet);
    return args;
}

std::optional<SidechainNodesDialog::Sidechain> SidechainNodesDialog::selected() const
{
    const QList<Sidechain> list{sidechains()};
    const int row{m_table->currentRow()};
    if (row < 0 || row >= list.size()) return std::nullopt;
    return list[row];
}

void SidechainNodesDialog::say(const QString& text)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void SidechainNodesDialog::setCell(int row, int column, const QString& text)
{
    if (QTableWidgetItem* item{m_table->item(row, column)}) {
        if (item->text() != text) item->setText(text);
    } else {
        m_table->setItem(row, column, NodeRpc::Item(text));
    }
}

void SidechainNodesDialog::runCli(const Sidechain& sidechain, const QStringList& command, std::function<void(const std::optional<UniValue>&, const QString&)> then)
{
    auto* process{new QProcess(this)};
    connect(process, &QProcess::finished, this, [process, then](int exit_code, QProcess::ExitStatus status) {
        const QString out{QString::fromUtf8(process->readAllStandardOutput()).trimmed()};
        const QString err{QString::fromUtf8(process->readAllStandardError()).trimmed()};
        process->deleteLater();
        if (status != QProcess::NormalExit || exit_code != 0) {
            then(std::nullopt, err.isEmpty() ? out : err);
            return;
        }
        UniValue value;
        // A command that answers with a bare word, like a new address, is not JSON.
        if (!value.read(out.toStdString())) value = UniValue{out.toStdString()};
        then(value, {});
    });
    connect(process, &QProcess::errorOccurred, this, [process, then](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        process->deleteLater();
        then(std::nullopt, tr("The program could not be started."));
    });
    process->start(program(sidechain, QStringLiteral("-cli")), networkArgs(sidechain) + QStringList{QStringLiteral("-rpcclienttimeout=10")} + command);
}

void SidechainNodesDialog::refresh()
{
    const bool server{gArgs.GetBoolArg("-server", false)};
    m_notice->setText(server ? QString{} : tr("<b>This node does not accept RPC connections</b>, which sidechain nodes need to follow it. "
                                              "Turn on \"Enable RPC server\" in Settings → Options → Main and restart, or start with -server."));
    m_notice->setVisible(!server);
    setWindowTitle(tr("Sidechain Nodes (%1)").arg(QString::fromStdString(Params().GetChainTypeString())));

    const QList<Sidechain> list{sidechains()};
    m_table->setRowCount(list.size());
    for (int row{0}; row < list.size(); ++row) {
        const Sidechain& sidechain{list[row]};
        setCell(row, COL_NAME, sidechain.name);
        m_table->item(row, COL_NAME)->setToolTip(sidechain.description);
        setCell(row, COL_SLOT, QString::number(sidechain.slot));
        const QString dir{binDir(sidechain.stem)};
        const bool installed{!dir.isEmpty() && QFileInfo::exists(program(sidechain, QStringLiteral("d")))};
        setCell(row, COL_PROGRAMS, installed ? dir : dir.isEmpty() ? tr("not on this computer yet: Locate or Download") : tr("no %1 in %2").arg(sidechain.stem + QStringLiteral("d"), dir));
        if (!installed) {
            setCell(row, COL_STATUS, QStringLiteral("-"));
            setCell(row, COL_BLOCKS, QString{});
            setCell(row, COL_MAINCHAIN, QString{});
            setCell(row, COL_MINING, QString{});
            m_running[sidechain.stem] = false;
            continue;
        }
        const QString stem{sidechain.stem};
        const auto row_of{[this, stem]() {
            const QList<Sidechain> now{sidechains()};
            for (int r{0}; r < now.size(); ++r) {
                if (now[r].stem == stem) return r;
            }
            return -1;
        }};
        runCli(sidechain, {QStringLiteral("getblockcount")}, [this, stem, row_of](const std::optional<UniValue>& blocks, const QString&) {
            const int r{row_of()};
            if (r < 0 || r >= m_table->rowCount()) return;
            m_running[stem] = blocks.has_value();
            setCell(r, COL_STATUS, blocks ? tr("running") : tr("stopped"));
            setCell(r, COL_BLOCKS, blocks ? NodeRpc::Text(*blocks) : QString{});
            if (!blocks) {
                setCell(r, COL_MAINCHAIN, QString{});
                setCell(r, COL_MINING, QString{});
            }
        });
        runCli(sidechain, {QStringLiteral("getmainchaininfo")}, [this, row_of](const std::optional<UniValue>& info, const QString&) {
            const int r{row_of()};
            if (r < 0 || r >= m_table->rowCount() || !info || !info->isObject()) return;
            setCell(r, COL_MAINCHAIN, (*info)["connected"].get_bool() ? tr("yes, at block %1").arg(NodeRpc::Text((*info)["height"])) : tr("no: %1").arg(NodeRpc::Text((*info)["error"])));
        });
        runCli(sidechain, {QStringLiteral("getbmminfo")}, [this, stem, row_of](const std::optional<UniValue>& info, const QString&) {
            const int r{row_of()};
            if (r < 0 || r >= m_table->rowCount() || !info || !info->isObject()) return;
            m_mining[stem] = (*info)["mining"].get_bool();
            // A node that mines only asks for a block when the block has something to do.
            const bool idle{m_mining[stem] && info->exists("idle") && (*info)["idle"].get_bool()};
            QString text{!m_mining[stem] ? tr("off") : idle ? tr("auto, waiting for fees, %1 blocks").arg(NodeRpc::Text((*info)["blocks"])) : tr("auto, %1 blocks").arg(NodeRpc::Text((*info)["blocks"]))};
            if (info->exists("error")) text += QStringLiteral(" (%1)").arg(NodeRpc::Text((*info)["error"]));
            setCell(r, COL_MINING, text);
        });
    }
    // The answers of the nodes come in later; the columns are fitted again then.
    QTimer::singleShot(1500, this, [this] {
        m_table->resizeColumnsToContents();
        // The headings are in bold, which the sizes above leave no room for.
        QFont heading{m_table->horizontalHeader()->font()};
        heading.setBold(true);
        const QFontMetrics metrics{heading};
        for (int column{0}; column + 1 < m_table->columnCount(); ++column) {
            const int needed{metrics.horizontalAdvance(m_table->horizontalHeaderItem(column)->text()) + 28};
            if (m_table->columnWidth(column) < needed) m_table->setColumnWidth(column, needed);
        }
        m_table->horizontalHeader()->setStretchLastSection(true);
    });
}

void SidechainNodesDialog::locate()
{
    const auto sidechain{selected()};
    if (!sidechain) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain in the list first."));
        return;
    }
    const QString dir{QFileDialog::getExistingDirectory(this, tr("The folder with %1").arg(sidechain->stem + QStringLiteral("d")), binDir(sidechain->stem))};
    if (dir.isEmpty()) return;
    if (!QFileInfo::exists(QDir(dir).filePath(sidechain->stem + QStringLiteral("d") + EXE))) {
        QMessageBox::warning(this, windowTitle(), tr("There is no %1 in that folder.").arg(sidechain->stem + QStringLiteral("d")));
        return;
    }
    setBinDir(sidechain->stem, dir);
    say(tr("%1: programs in %2").arg(sidechain->name, dir));
    refresh();
}

void SidechainNodesDialog::download()
{
    const auto sidechain{selected()};
    if (!sidechain) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain in the list first."));
        return;
    }
    bool ok;
    const QString url{QInputDialog::getText(this, windowTitle(), tr("Address of the release of %1 to download (a .tar.gz archive):").arg(sidechain->name), QLineEdit::Normal, {}, &ok).trimmed()};
    if (!ok || url.isEmpty()) return;
    const QString expected{QInputDialog::getText(this, windowTitle(), tr("SHA-256 hash that the publisher gives for it. The download is thrown away if it does not match:"), QLineEdit::Normal, {}, &ok).trimmed().toLower()};
    if (!ok) return;
    if (expected.size() != 64) {
        QMessageBox::warning(this, windowTitle(), tr("A SHA-256 hash is 64 hexadecimal characters. Programs that hold coins are not installed unchecked."));
        return;
    }

    const QString target{GUIUtil::PathToQString(gArgs.GetDataDirBase() / "sidechains") + QLatin1Char('/') + sidechain->stem};
    QDir().mkpath(target);
    const QString archive{QDir(target).filePath(QStringLiteral("download.tar.gz"))};
    QFile::remove(archive);
    const Sidechain chosen{*sidechain};
    say(tr("%1: downloading %2").arg(chosen.name, url));

    auto* fetch{new QProcess(this)};
    connect(fetch, &QProcess::errorOccurred, this, [this, fetch](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        fetch->deleteLater();
        say(tr("The download needs the program curl, which could not be started."));
    });
    connect(fetch, &QProcess::finished, this, [this, fetch, archive, expected, target, chosen](int exit_code, QProcess::ExitStatus) {
        const QString err{QString::fromUtf8(fetch->readAllStandardError()).trimmed()};
        fetch->deleteLater();
        if (exit_code != 0) {
            say(tr("%1: the download failed. %2").arg(chosen.name, err));
            return;
        }
        const QString hash{FileHash(archive)};
        if (hash != expected) {
            QFile::remove(archive);
            say(tr("%1: the download does NOT have the expected hash and was thrown away. It has %2.").arg(chosen.name, hash));
            QMessageBox::warning(this, windowTitle(), tr("The download does not have the hash you gave. It was thrown away."));
            return;
        }
        say(tr("%1: the hash matches; unpacking").arg(chosen.name));
        auto* unpack{new QProcess(this)};
        connect(unpack, &QProcess::finished, this, [this, unpack, archive, target, chosen](int code, QProcess::ExitStatus) {
            const QString unpack_err{QString::fromUtf8(unpack->readAllStandardError()).trimmed()};
            unpack->deleteLater();
            QFile::remove(archive);
            if (code != 0) {
                say(tr("%1: unpacking failed. %2").arg(chosen.name, unpack_err));
                return;
            }
            // The programs are somewhere in what was unpacked.
            QDirIterator it{target, {chosen.stem + QStringLiteral("d") + EXE}, QDir::Files, QDirIterator::Subdirectories};
            if (!it.hasNext()) {
                say(tr("%1: the archive has no %2 in it.").arg(chosen.name, chosen.stem + QStringLiteral("d")));
                return;
            }
            const QString dir{QFileInfo(it.next()).absolutePath()};
            setBinDir(chosen.stem, dir);
            say(tr("%1: installed in %2").arg(chosen.name, dir));
            refresh();
        });
        unpack->start(QStringLiteral("tar"), {QStringLiteral("-xzf"), archive, QStringLiteral("-C"), target});
    });
    fetch->start(QStringLiteral("curl"), {QStringLiteral("--fail"), QStringLiteral("--location"), QStringLiteral("--silent"), QStringLiteral("--show-error"), QStringLiteral("--output"), archive, url});
}

void SidechainNodesDialog::addSidechain()
{
    bool ok;
    const QString name{QInputDialog::getText(this, windowTitle(), tr("Name of the sidechain:"), QLineEdit::Normal, {}, &ok).trimmed()};
    if (!ok || name.isEmpty()) return;
    const QString stem{QInputDialog::getText(this, windowTitle(), tr("What its programs are named after: if its node is \"examplechaind\", enter \"examplechain\"."), QLineEdit::Normal, name.toLower().remove(' '), &ok).trimmed()};
    if (!ok || stem.isEmpty() || stem.contains('\t') || name.contains('\t')) return;
    const int slot{QInputDialog::getInt(this, windowTitle(), tr("The slot of the sidechain on this chain:"), 0, 0, 65535, 1, &ok)};
    if (!ok) return;
    for (const Sidechain& known : sidechains()) {
        if (known.stem == stem) {
            QMessageBox::information(this, windowTitle(), tr("A sidechain with these programs is in the list already."));
            return;
        }
    }
    QSettings settings;
    QStringList custom{settings.value(SETTING_CUSTOM).toStringList()};
    custom << QStringLiteral("%1\t%2\t%3").arg(name, stem).arg(slot);
    settings.setValue(SETTING_CUSTOM, custom);
    refresh();
}

void SidechainNodesDialog::removeSidechain()
{
    const auto sidechain{selected()};
    if (!sidechain) {
        QMessageBox::information(this, windowTitle(), tr("Select the sidechain to remove."));
        return;
    }
    QSettings settings;
    QStringList custom{settings.value(SETTING_CUSTOM).toStringList()};
    const auto entry{std::find_if(custom.begin(), custom.end(), [&](const QString& line) { return line.section('\t', 1, 1) == sidechain->stem; })};
    if (entry == custom.end()) {
        QMessageBox::information(this, windowTitle(), tr("%1 comes with this wallet and stays in the list. Only a sidechain that was added by hand can be removed.").arg(sidechain->name));
        return;
    }
    if (m_running.value(sidechain->stem)) {
        QMessageBox::information(this, windowTitle(), tr("The node of %1 is running. Stop it first.").arg(sidechain->name));
        return;
    }
    if (QMessageBox::question(this, windowTitle(), tr("Take %1 off the list? Its programs and its data stay on this computer.").arg(sidechain->name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    custom.erase(entry);
    settings.setValue(SETTING_CUSTOM, custom);
    // Where its programs are is forgotten with it.
    settings.remove(SettingKey(sidechain->stem));
    m_running.remove(sidechain->stem);
    m_mining.remove(sidechain->stem);
    say(tr("%1 was taken off the list.").arg(sidechain->name));
    refresh();
}

void SidechainNodesDialog::startNode()
{
    const auto sidechain{selected()};
    if (!sidechain || binDir(sidechain->stem).isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain whose programs are on this computer."));
        return;
    }
    if (m_running.value(sidechain->stem)) {
        say(tr("%1 is running already.").arg(sidechain->name));
        return;
    }
    QDir().mkpath(dataDir(*sidechain));
    const QStringList args{nodeArgs(*sidechain) + QStringList{QStringLiteral("-daemon")}};
    if (Launch(program(*sidechain, QStringLiteral("d")), args)) {
        say(tr("%1: node started").arg(sidechain->name));
    } else {
        say(tr("%1: the node could not be started").arg(sidechain->name));
    }
    QTimer::singleShot(1500, this, &SidechainNodesDialog::refresh);
}

void SidechainNodesDialog::startWallet()
{
    const auto sidechain{selected()};
    if (!sidechain || binDir(sidechain->stem).isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain whose programs are on this computer."));
        return;
    }
    if (m_running.value(sidechain->stem)) {
        QMessageBox::information(this, windowTitle(), tr("The node of %1 is running in the background. Stop it first: the wallet window is a node itself, and there can be only one.").arg(sidechain->name));
        return;
    }
    QDir().mkpath(dataDir(*sidechain));
    if (Launch(program(*sidechain, QStringLiteral("-qt")), nodeArgs(*sidechain) + QStringList{QStringLiteral("-server")})) {
        say(tr("%1: wallet started").arg(sidechain->name));
    } else {
        say(tr("%1: the wallet could not be started").arg(sidechain->name));
    }
    QTimer::singleShot(3000, this, &SidechainNodesDialog::refresh);
}

void SidechainNodesDialog::stopNode()
{
    const auto sidechain{selected()};
    if (!sidechain) return;
    const QString name{sidechain->name};
    runCli(*sidechain, {QStringLiteral("stop")}, [this, name](const std::optional<UniValue>& result, const QString& error) {
        say(result ? tr("%1: stopping").arg(name) : tr("%1: not stopped. %2").arg(name, error));
        QTimer::singleShot(1500, this, &SidechainNodesDialog::refresh);
    });
}

void SidechainNodesDialog::mineOnce()
{
    const auto sidechain{selected()};
    if (!sidechain || !m_running.value(sidechain->stem)) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain whose node is running."));
        return;
    }
    if (!m_wallet_name()) {
        QMessageBox::information(this, windowTitle(), tr("Open a wallet of this node first: it pays the miners of this chain for the block of the sidechain."));
        return;
    }
    bool ok{false};
    const QString amount{QInputDialog::getText(this, windowTitle(), tr("What to offer the miners of this chain for one block of %1, in coins of this chain:").arg(sidechain->name), QLineEdit::Normal, QStringLiteral("0.0001"), &ok).trimmed()};
    if (!ok || amount.isEmpty()) return;
    const Sidechain chosen{*sidechain};
    runCli(chosen, {QStringLiteral("getnewaddress"), QStringLiteral("mining")}, [this, chosen, amount](const std::optional<UniValue>& address, const QString& address_error) {
        if (!address) {
            say(tr("%1: %2 (the sidechain node needs a wallet: turn automatic mining on once, or open its wallet)").arg(chosen.name, address_error));
            return;
        }
        runCli(chosen, {QStringLiteral("requestbmmblock"), NodeRpc::Text(*address), amount}, [this, chosen](const std::optional<UniValue>& result, const QString& error) {
            say(result ? tr("%1: asked for one block with %2 transactions, offering %3. It is mined if the next block of this chain takes the offer.")
                             .arg(chosen.name, QString::number((*result)["transactions"].getInt<int>() - 1), QString::number((*result)["amount"].get_real(), 'f', 8))
                       : tr("%1: %2").arg(chosen.name, error));
            refresh();
        });
    });
}

void SidechainNodesDialog::toggleMining()
{
    const auto sidechain{selected()};
    if (!sidechain || !m_running.value(sidechain->stem)) {
        QMessageBox::information(this, windowTitle(), tr("Select a sidechain whose node is running."));
        return;
    }
    const Sidechain chosen{*sidechain};
    const auto report{[this, chosen](const QString& what) {
        return [this, chosen, what](const std::optional<UniValue>& result, const QString& error) {
            say(result ? tr("%1: %2").arg(chosen.name, what) : tr("%1: %2").arg(chosen.name, error));
            refresh();
        };
    }};
    if (m_mining.value(chosen.stem)) {
        runCli(chosen, {QStringLiteral("setbmm"), QStringLiteral("false")}, report(tr("mining turned off")));
        return;
    }
    if (!m_wallet_name()) {
        QMessageBox::information(this, windowTitle(), tr("Open a wallet of this node first: it pays the miners of this chain for each block of the sidechain."));
        return;
    }
    // The fees of the blocks go to a wallet of the sidechain node; it gets one if it has none.
    runCli(chosen, {QStringLiteral("listwallets")}, [this, chosen, report](const std::optional<UniValue>& wallets, const QString& error) {
        if (!wallets) {
            say(tr("%1: %2").arg(chosen.name, error));
            return;
        }
        const auto mine{[this, chosen, report] {
            runCli(chosen, {QStringLiteral("getnewaddress"), QStringLiteral("mining")}, [this, chosen, report](const std::optional<UniValue>& address, const QString& address_error) {
                if (!address) {
                    say(tr("%1: %2").arg(chosen.name, address_error));
                    return;
                }
                runCli(chosen, {QStringLiteral("setbmm"), QStringLiteral("true"), NodeRpc::Text(*address)}, report(tr("automatic mining turned on; fees go to %1").arg(NodeRpc::Text(*address))));
            });
        }};
        if (wallets->isArray() && !wallets->empty()) {
            mine();
        } else {
            runCli(chosen, {QStringLiteral("createwallet"), QStringLiteral("main")}, [this, chosen, mine](const std::optional<UniValue>& created, const QString& create_error) {
                if (!created) {
                    // It may exist and not be loaded.
                    runCli(chosen, {QStringLiteral("loadwallet"), QStringLiteral("main")}, [this, chosen, mine, create_error](const std::optional<UniValue>& loaded, const QString&) {
                        if (loaded) {
                            mine();
                        } else {
                            say(tr("%1: %2").arg(chosen.name, create_error));
                        }
                    });
                    return;
                }
                mine();
            });
        }
    });
}

void SidechainNodesDialog::showLog()
{
    const auto sidechain{selected()};
    if (!sidechain) return;
    // The data directory has a folder per network.
    QString dir{dataDir(*sidechain)};
    switch (Params().GetChainType()) {
    case ChainType::MAIN: break;
    case ChainType::TESTNET: dir += QStringLiteral("/testnet"); break;
    case ChainType::SIGNET: dir += QStringLiteral("/signet"); break;
    case ChainType::REGTEST: dir += QStringLiteral("/regtest"); break;
    }
    QFile file{QDir(dir).filePath(QStringLiteral("debug.log"))};
    if (!file.open(QIODevice::ReadOnly)) {
        say(tr("%1 has no log at %2 yet.").arg(sidechain->name, file.fileName()));
        return;
    }
    file.seek(std::max<qint64>(0, file.size() - 4000));
    const QStringList lines{QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts)};
    m_log->appendPlainText(tr("--- the end of %1 ---").arg(file.fileName()));
    for (int i{lines.size() > 1 ? 1 : 0}; i < lines.size(); ++i) m_log->appendPlainText(lines[i]);
}

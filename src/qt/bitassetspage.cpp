// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitassetspage.h>

#include <bitassets/state.h>

#include <qt/clientmodel.h>
#include <qt/itemviews.h>
#include <qt/qrimagewidget.h>

#include <QClipboard>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

namespace {
const char* const GREEN{"#10b981"};
const char* const ORANGE{"#f59e0b"};
const char* const GREY{"#6b7280"};
const char* const PURPLE{"#8b5cf6"};
const char* const BLUE{"#3b82f6"};
const char* const RED{"#ef4444"};

QLabel* Dim(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    label->setEnabled(false);
    return label;
}

QLabel* Heading(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setObjectName("sectionHeading");
    return label;
}

QLabel* Pill(const QString& text, const char* colour, QWidget* parent)
{
    auto* pill{new QLabel(text, parent)};
    pill->setStyleSheet(QStringLiteral("color: white; background: %1; border-radius: 8px; padding: 1px 8px; font-size: 8pt; font-weight: bold;").arg(QLatin1String(colour)));
    return pill;
}

QFrame* Card(QWidget* parent)
{
    auto* card{new QFrame(parent)};
    card->setObjectName("card");
    return card;
}

/** A field for an amount: digits, and a point. */
QLineEdit* AmountEdit(const QString& placeholder, QWidget* parent)
{
    auto* edit{new QLineEdit(parent)};
    edit->setPlaceholderText(placeholder);
    edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral(R"(\d{0,19}(\.\d{0,12})?)")), edit));
    edit->setMinimumHeight(28);
    return edit;
}

QListWidget* PickList(QWidget* parent)
{
    auto* list{new ItemViews::List(parent)};
    list->setObjectName("pickList");
    return list;
}

QTableWidget* ViewTable(const QStringList& headers, QWidget* parent)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

/** A number with its thousands apart: 1,234,567.25. */
QString Grouped(const QString& number)
{
    const int point{number.indexOf(QLatin1Char('.'))};
    QString whole{point < 0 ? number : number.left(point)};
    const QString rest{point < 0 ? QString{} : number.mid(point)};
    const bool negative{whole.startsWith(QLatin1Char('-'))};
    if (negative) whole.remove(0, 1);
    for (int i{static_cast<int>(whole.size()) - 3}; i > 0; i -= 3) whole.insert(i, QLatin1Char(','));
    return (negative ? QStringLiteral("-") : QString{}) + whole + rest;
}

QString Num(const UniValue& value) { return Grouped(Text(value)); }

/**
 * The price of one `base` in `quote`, readably: "0.0025 CHN each", or, when one unit is worth
 * almost nothing, the other way round: "1 CHN buys 400,000 PEBBLES".
 */
QString UnitPrice(const UniValue& price, const QString& base, const QString& quote)
{
    const double p{QString::fromStdString(price.getValStr()).toDouble()};
    if (p <= 0) return QObject::tr("—");
    if (p >= 0.0001) {
        QString text{QString::number(p, 'f', p >= 1 ? 4 : 8)};
        text.remove(QRegularExpression(QStringLiteral("\\.?0+$")));
        return QObject::tr("%1 %2 each").arg(Grouped(text), quote);
    }
    return QObject::tr("1 %1 buys %2 %3").arg(quote, Grouped(QString::number(1 / p, 'f', 0)), base);
}

/** A part of a panel that opens and closes. */
QWidget* Foldable(const QString& title, QWidget* content, QWidget* parent, bool open = false)
{
    auto* box{new QWidget(parent)};
    auto* layout{new QVBoxLayout(box)};
    layout->setContentsMargins(0, 6, 0, 0);
    auto* toggle{new QToolButton(box)};
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setAutoRaise(true);
    toggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; border: none; }"));
    layout->addWidget(toggle);
    content->setParent(box);
    layout->addWidget(content);
    const auto set{[toggle, content](bool shown) {
        content->setVisible(shown);
        toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    }};
    set(open);
    toggle->setChecked(open);
    QObject::connect(toggle, &QToolButton::toggled, content, set);
    return box;
}

/** A row of a field and a button. */
QHBoxLayout* Row(std::initializer_list<QWidget*> widgets, std::initializer_list<int> stretches = {})
{
    auto* row{new QHBoxLayout};
    auto stretch{stretches.begin()};
    for (QWidget* w : widgets) {
        row->addWidget(w, stretch != stretches.end() ? *stretch++ : 0);
    }
    return row;
}

/** The names this page reserved and will register, per wallet: a JSON object each. */
QString PendingKey(const QString& wallet) { return QStringLiteral("BitAssets/pending/%1").arg(wallet); }

/**
 * Whether a name can be an asset's: 1 to 64 printable characters, no space at either end, and not one
 * that reads as another asset (CHN, a number such as 1739-0029, "0x" and a hash): as the rules say.
 */
bool GoodName(const QString& name)
{
    if (name.isEmpty() || name.size() > 64 || name.front() == QLatin1Char(' ') || name.back() == QLatin1Char(' ')) return false;
    if (name.compare(QStringLiteral("CHN"), Qt::CaseInsensitive) == 0) return false;
    if (name.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) return false;
    static const QRegularExpression number{QStringLiteral("^[0-9]{4}-[0-9]{4}(-[0-9]{4})?$")};
    if (number.match(name).hasMatch()) return false;
    return std::all_of(name.begin(), name.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() <= 0x7E; });
}

/**
 * How the page names an asset to the commands: "CHN", or "0x" and its hash; never by its label, which
 * an asset registered under a name like "CHN" or like a number of another shares.
 */
QString AssetArg(const UniValue& hex)
{
    const std::string id{hex.get_str()};
    if (id.find_first_not_of('0') == std::string::npos) return QStringLiteral("CHN");
    return QStringLiteral("0x") + QString::fromStdString(id);
}

/** The asset picked in a list of assets: its AssetArg (the label is what is shown). */
QString ArgOf(const QComboBox* combo) { return combo->currentData().toString(); }

void SelectAsset(QComboBox* combo, const QString& arg)
{
    const int index{combo->findData(arg)};
    if (index >= 0) combo->setCurrentIndex(index);
}
} // namespace

BitAssetsPage::BitAssetsPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QWidget(parent), m_wallet_name{std::move(wallet_name)}
{
    setStyleSheet(QStringLiteral(
        "QLabel#pageTitle { font-size: 18pt; font-weight: bold; }"
        "QLabel#panelTitle { font-size: 20pt; font-weight: bold; }"
        "QLabel#bigNumber { font-size: 16pt; font-weight: bold; }"
        "QLabel#sectionHeading { font-size: 11pt; font-weight: bold; margin-top: 10px; }"
        "QLabel#getAmount { font-size: 16pt; font-weight: bold; }"
        "QFrame#card { border: 1px solid palette(mid); border-radius: 10px; }"
        "QListWidget#pickList::item { padding: 6px; }"
        "QPushButton#primary { font-weight: bold; padding: 6px 18px; }"
        "QPushButton#newButton { font-weight: bold; }"
        "QLineEdit#nameSearch { font-size: 14pt; padding: 4px; }"));
    auto* layout{new QVBoxLayout(this)};

    auto* top{new QHBoxLayout};
    auto* title{new QLabel(tr("Assets"), this)};
    title->setObjectName("pageTitle");
    top->addWidget(title);
    top->addStretch();
    m_summary = new QLabel(this);
    m_summary->setEnabled(false);
    top->addWidget(m_summary);
    layout->addLayout(top);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(createMineTab(), tr("My assets"));
    m_tabs->addTab(createTradeTab(), tr("Trade"));
    m_tabs->addTab(createAuctionsTab(), tr("Auctions"));
    m_tabs->addTab(createExploreTab(), tr("Explore"));
    m_tabs->addTab(createActivityTab(), tr("Activity"));
    layout->addWidget(m_tabs, 1);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });
    auto* timer{new QTimer(this)};
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible()) refresh();
    });
    timer->start(5000);
}

void BitAssetsPage::setClientModel(ClientModel* client_model) { m_client_model = client_model; }

void BitAssetsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

std::optional<UniValue> BitAssetsPage::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    if (!m_client_model) return std::nullopt;
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        wallet_name = m_wallet_name();
        if (!wallet_name) {
            if (!quiet) say(tr("Open or create a wallet first."), true);
            return std::nullopt;
        }
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result && !quiet) say(error, true);
    return result;
}

void BitAssetsPage::say(const QString& text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("color: %1;").arg(QLatin1String(RED)) : QStringLiteral("color: %1;").arg(QLatin1String(GREEN)));
    m_status->setText(text);
}

bool BitAssetsPage::confirm(const QString& title, const QString& text, bool danger)
{
    m_busy = true;
    const bool yes{danger ? QMessageBox::warning(this, title, text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes
                          : QMessageBox::question(this, title, text) == QMessageBox::Yes};
    m_busy = false;
    return yes;
}

void BitAssetsPage::setPanel(QScrollArea* area, QWidget* panel)
{
    // The panel shown before goes once control is back in the event loop: what called this may be in it.
    if (QWidget* old{area->takeWidget()}) old->deleteLater();
    area->setWidget(panel);
}

//
// Refreshing.
//

void BitAssetsPage::refresh()
{
    if (!m_client_model || m_busy) return;
    refreshInfo();
    refreshAssetChoices();
    registerPending();
    switch (m_tabs->currentIndex()) {
    case 0: refreshMine(); break;
    case 1: refreshTrade(); break;
    case 2: refreshAuctions(); break;
    case 3: refreshExplore(); break;
    case 4: refreshActivity(); break;
    }
}

void BitAssetsPage::refreshInfo()
{
    const auto info{call("getbitassetsinfo", UniValue{UniValue::VARR}, false, true)};
    if (!info) return;
    m_height = (*info)["height"].getInt<int>();
    if ((*info).exists("reveal_depth")) m_reveal_depth = (*info)["reveal_depth"].getInt<int>();
    m_summary->setText(tr("%1 assets · %2 pools · %3 auctions · next block %4")
                           .arg(Text((*info)["assets"]), Text((*info)["pools"]), Text((*info)["auctions"]), QString::number(m_height)));
}

void BitAssetsPage::refreshAssetChoices()
{
    const auto assets{call("listassets", Args({1000, 0}), false, true)};
    if (!assets) return;
    // Each asset by its id: labels may look alike, the ids never do.
    QStringList labels{QStringLiteral("CHN")}, args{QStringLiteral("CHN")};
    std::map<QString, int> decimals{{QStringLiteral("CHN"), 8}};
    for (const UniValue& a : assets->getValues()) {
        const QString arg{AssetArg(a["asset"])};
        labels << Text(a["label"]);
        args << arg;
        decimals[arg] = a["decimals"].getInt<int>();
    }
    if (labels == m_asset_labels && args == m_asset_args) return;
    m_asset_labels = labels;
    m_asset_args = args;
    m_decimals = decimals;
    for (QComboBox* combo : {m_pay_asset, m_get_asset, m_lq_a, m_lq_b, m_sell_asset, m_sell_for}) {
        const QString current{ArgOf(combo)};
        const QSignalBlocker blocker{combo};
        combo->clear();
        for (int i{0}; i < labels.size(); ++i) combo->addItem(labels[i], args[i]);
        if (!current.isEmpty()) SelectAsset(combo, current);
    }
    // Sensible starting pairs: CHN for the first asset.
    if (labels.size() > 1) {
        if (ArgOf(m_get_asset) == ArgOf(m_pay_asset)) m_get_asset->setCurrentIndex(1);
        if (ArgOf(m_lq_a) == ArgOf(m_lq_b)) m_lq_a->setCurrentIndex(1);
        if (ArgOf(m_sell_asset) == ArgOf(m_sell_for)) m_sell_asset->setCurrentIndex(1);
    }
}

//
// My assets.
//

QWidget* BitAssetsPage::createMineTab()
{
    auto* split{new QSplitter(Qt::Horizontal, this)};
    split->setChildrenCollapsible(false);
    auto* left{new QWidget(split)};
    auto* left_layout{new QVBoxLayout(left)};
    left_layout->setContentsMargins(0, 6, 0, 0);
    auto* create{new QPushButton(tr("+  Create an asset"), left)};
    create->setObjectName("newButton");
    create->setMinimumHeight(34);
    auto* transfer{new QPushButton(tr("⇄  Send && receive"), left)};
    transfer->setObjectName("newButton");
    transfer->setMinimumHeight(34);
    transfer->setToolTip(tr("Send assets to someone, or get the address they send yours to"));
    auto* buttons{new QHBoxLayout};
    buttons->addWidget(create, 1);
    buttons->addWidget(transfer, 1);
    left_layout->addLayout(buttons);
    m_mine = PickList(left);
    m_mine->setMinimumWidth(240);
    left_layout->addWidget(m_mine, 1);
    split->addWidget(left);
    m_mine_detail = new QScrollArea(split);
    m_mine_detail->setWidgetResizable(true);
    m_mine_detail->setFrameShape(QFrame::NoFrame);
    split->addWidget(m_mine_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    m_mine_shown = QStringLiteral("create");
    QTimer::singleShot(0, this, [this] { showCreatePanel(); });

    connect(create, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker{m_mine};
        m_mine->clearSelection();
        m_mine->setCurrentItem(nullptr);
        m_mine_shown = QStringLiteral("create");
        showCreatePanel();
    });
    connect(transfer, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker{m_mine};
        m_mine->clearSelection();
        m_mine->setCurrentItem(nullptr);
        m_mine_shown = QStringLiteral("transfer");
        showTransferPanel();
    });
    connect(m_mine, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!item || item->data(Qt::UserRole).toString().isEmpty()) return;
        m_mine_shown = item->data(Qt::UserRole).toString();
        m_mine_shown_name = item->data(Qt::UserRole + 1).toString();
        showMineDetail();
    });
    return split;
}

void BitAssetsPage::refreshMine()
{
    const auto mine{call("listmyassets", UniValue{UniValue::VARR}, true, true)};
    if (!mine) return;
    const std::string listed{mine->write()};
    if (listed == m_mine_listed) return;
    m_mine_listed = listed;
    m_holdings = *mine;
    const QSignalBlocker blocker{m_mine};
    m_mine->clear();
    QListWidgetItem* picked{nullptr};
    const auto add{[&](const QString& key, const QString& text, const char* colour, const QString& name = {}) {
        auto* item{new QListWidgetItem(text, m_mine)};
        item->setData(Qt::UserRole, key);
        item->setData(Qt::UserRole + 1, name);
        if (colour) item->setForeground(QColor{colour});
        if (key == m_mine_shown) picked = item;
    }};
    const auto header{[&](const QString& text) {
        auto* item{new QListWidgetItem(text, m_mine)};
        item->setFlags(Qt::NoItemFlags);
        QFont font{item->font()};
        font.setBold(true);
        item->setFont(font);
    }};
    if (!(*mine)["assets"].empty()) header(tr("Assets"));
    for (const UniValue& a : (*mine)["assets"].getValues()) {
        QString line{Num(a["balance"]) + QStringLiteral("  ") + Text(a["label"])};
        // What is on its way back (change) or in: shown, not yet spendable.
        if (a["pending"].get_real() > 0) line += tr("   +%1 waiting").arg(Num(a["pending"]));
        if (a["control"].isTrue()) line += QStringLiteral("   ★");
        add(QStringLiteral("asset:") + Text(a["asset"]), line, nullptr, Text(a["label"]));
    }
    // What this page is still registering.
    QStringList pending;
    if (const auto wallet{m_wallet_name()}) {
        for (const QString& entry : QSettings{}.value(PendingKey(*wallet)).toStringList()) {
            UniValue obj;
            if (obj.read(entry.toStdString())) pending << Text(obj["name"]);
        }
    }
    if (!(*mine)["reservations"].empty() || !pending.isEmpty()) header(tr("Being created"));
    QStringList shown;
    for (const UniValue& r : (*mine)["reservations"].getValues()) {
        const QString name{r.exists("name") ? Text(r["name"]) : tr("(a reservation)")};
        shown << name;
        const int wait{r.exists("wait") ? r["wait"].getInt<int>() : 0};
        const QString state{r["taken"].isTrue() ? tr("%1 — taken by another").arg(name) : wait > 0 ? tr("%1 — registering in %n block(s)", "", wait).arg(name) : tr("%1 — registering…").arg(name)};
        add(QStringLiteral("reservation:") + Text(r["txid"]), state, r["taken"].isTrue() ? RED : ORANGE, name);
    }
    for (const QString& name : pending) {
        if (!shown.contains(name)) add(QStringLiteral("pending:") + name, tr("%1 — being created…").arg(name), ORANGE, name);
    }
    if (!(*mine)["liquidity"].empty()) header(tr("Pool shares"));
    for (const UniValue& l : (*mine)["liquidity"].getValues()) {
        add(QStringLiteral("liquidity:") + Text(l["pool"]), tr("%1 / %2   %3%").arg(Text(l["asset_a"]), Text(l["asset_b"]), QString::number(l["percent"].get_real(), 'f', 2)), BLUE);
    }
    if (!(*mine)["receipts"].empty()) header(tr("Auctions to collect"));
    for (const UniValue& r : (*mine)["receipts"].getValues()) {
        add(QStringLiteral("receipt:") + Text(r["auction"]), tr("Auction %1…").arg(Text(r["auction"]).left(12)), PURPLE);
    }
    if (m_mine->count() == 0) {
        auto* item{new QListWidgetItem(tr("Nothing yet: create an asset, or buy one in Trade."), m_mine)};
        item->setFlags(Qt::NoItemFlags);
    }
    if (!picked && (m_mine_shown.startsWith(QLatin1String("pending:")) || m_mine_shown.startsWith(QLatin1String("reservation:")))) {
        // A name being created moved on, from reserving to registering to registered: follow it.
        const QString name{m_mine_shown.startsWith(QLatin1String("pending:")) ? m_mine_shown.mid(8) : m_mine_shown_name};
        for (int i{0}; i < m_mine->count() && !name.isEmpty(); ++i) {
            if (m_mine->item(i)->data(Qt::UserRole + 1).toString() == name) {
                picked = m_mine->item(i);
                m_mine_shown = picked->data(Qt::UserRole).toString();
            }
        }
    }
    if (picked) {
        m_mine_shown_name = picked->data(Qt::UserRole + 1).toString();
        m_mine->setCurrentItem(picked);
        // What it shows changed: rebuilt, unless it is being typed in.
        QWidget* focus{QApplication::focusWidget()};
        if (!m_mine_detail->widget() || !focus || !m_mine_detail->widget()->isAncestorOf(focus)) showMineDetail();
    } else if (m_mine_shown != QLatin1String("create") && m_mine_shown != QLatin1String("transfer")) {
        // What was shown is gone (sent, collected, released).
        m_mine_shown = QStringLiteral("create");
        showCreatePanel();
    }
}

void BitAssetsPage::showMineDetail()
{
    const QString kind{m_mine_shown.section(QLatin1Char(':'), 0, 0)};
    const QString key{m_mine_shown.section(QLatin1Char(':'), 1)};
    const auto find{[&](const char* list, const char* field) -> std::optional<UniValue> {
        if (!m_holdings.isObject()) return std::nullopt;
        for (const UniValue& v : m_holdings[list].getValues()) {
            if (Text(v[field]) == key) return v;
        }
        return std::nullopt;
    }};
    if (kind == QLatin1String("asset")) {
        if (const auto v{find("assets", "asset")}) return showAssetPanel(*v);
    } else if (kind == QLatin1String("reservation")) {
        if (const auto v{find("reservations", "txid")}) return showReservationPanel(*v);
    } else if (kind == QLatin1String("liquidity")) {
        if (const auto v{find("liquidity", "pool")}) return showLiquidityPanel(*v);
    } else if (kind == QLatin1String("receipt")) {
        return showReceiptPanel(key);
    } else if (kind == QLatin1String("pending")) {
        UniValue r(UniValue::VOBJ);
        r.pushKV("name", key.toStdString());
        r.pushKV("taken", false);
        return showReservationPanel(r);
    }
    showCreatePanel();
}

void BitAssetsPage::showTransferPanel()
{
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("Send && receive assets"), panel)};
    title->setObjectName("panelTitle");
    layout->addWidget(title);

    // Receiving: an address of this wallet; any asset can be sent to it.
    layout->addWidget(Heading(tr("Receive"), panel));
    layout->addWidget(Dim(tr("Give this address to whoever sends you assets: any asset, any amount, as often as they like."), panel));
    if (m_receive_address.isEmpty()) {
        if (const auto a{call("getnewaddress", Args({"assets", "bech32"}), true, true)}) m_receive_address = Text(*a);
    }
    auto* receive_row{new QHBoxLayout};
    auto* address{new QLineEdit(m_receive_address, panel)};
    address->setReadOnly(true);
    address->setStyleSheet(QStringLiteral("font-family: monospace;"));
    auto* copy{new QPushButton(tr("Copy"), panel)};
    auto* fresh{new QPushButton(tr("New address"), panel)};
    auto* receive_text{new QVBoxLayout};
    receive_text->addWidget(address);
    receive_text->addLayout(Row({copy, fresh}));
    receive_text->addStretch();
    receive_row->addLayout(receive_text, 1);
    auto* qr{new QRImageWidget(panel)};
    qr->setFixedSize(130, 130);
    qr->setScaledContents(true);
    qr->setQR(m_receive_address);
    receive_row->addWidget(qr);
    layout->addLayout(receive_row);
    connect(copy, &QPushButton::clicked, this, [this, address] {
        QApplication::clipboard()->setText(address->text());
        say(tr("The address is copied."));
    });
    connect(fresh, &QPushButton::clicked, this, [this, address, qr] {
        if (const auto a{call("getnewaddress", Args({"assets", "bech32"}), true)}) {
            m_receive_address = Text(*a);
            address->setText(m_receive_address);
            qr->setQR(m_receive_address);
        }
    });

    // Sending: what the wallet holds.
    layout->addWidget(Heading(tr("Send"), panel));
    auto* form{new QFormLayout};
    auto* asset{new QComboBox(panel)};
    std::map<QString, QString> balances;
    if (!m_holdings.isObject()) {
        if (const auto mine{call("listmyassets", UniValue{UniValue::VARR}, true, true)}) m_holdings = *mine;
    }
    if (m_holdings.isObject()) {
        for (const UniValue& a : m_holdings["assets"].getValues()) {
            if (a["balance"].get_real() <= 0) continue;
            asset->addItem(Text(a["label"]), AssetArg(a["asset"]));
            balances[AssetArg(a["asset"])] = Text(a["balance"]);
        }
    }
    auto* held{Dim(QString{}, panel)};
    auto* to{new QLineEdit(panel)};
    to->setPlaceholderText(tr("Their address"));
    auto* amount{AmountEdit(tr("Amount"), panel)};
    auto* all{new QPushButton(tr("All"), panel)};
    auto* send{new QPushButton(tr("Send"), panel)};
    send->setObjectName("primary");
    form->addRow(tr("Asset"), asset);
    form->addRow(QString{}, held);
    form->addRow(tr("To"), to);
    form->addRow(tr("Amount"), Row({amount, all}, {1, 0}));
    layout->addLayout(form);
    layout->addWidget(send, 0, Qt::AlignRight);
    if (asset->count() == 0) {
        asset->setEnabled(false);
        send->setEnabled(false);
        held->setText(tr("This wallet holds no assets in blocks yet: create one, buy one in Trade, or have one sent here."));
    }
    layout->addWidget(Dim(tr("CHN itself is sent from the Send page of the wallet."), panel));
    layout->addStretch();
    const auto show_held{[asset, held, balances] {
        if (const auto it{balances.find(ArgOf(asset))}; it != balances.end()) held->setText(QObject::tr("You hold %1 %2").arg(Grouped(it->second), asset->currentText()));
    }};
    show_held();
    connect(asset, &QComboBox::currentTextChanged, held, show_held);
    connect(all, &QPushButton::clicked, amount, [asset, amount, balances] {
        if (const auto it{balances.find(ArgOf(asset))}; it != balances.end()) amount->setText(it->second);
    });
    connect(send, &QPushButton::clicked, this, [this, asset, to, amount] {
        const QString label{asset->currentText()};
        if (to->text().trimmed().isEmpty() || amount->text().isEmpty()) return say(tr("Give an address and an amount."), true);
        if (!confirm(tr("Send %1").arg(label), tr("Send %1 %2 to %3?").arg(amount->text(), label, to->text().trimmed()))) return;
        if (call("sendasset", Args({to->text().trimmed().toStdString(), ArgOf(asset).toStdString(), amount->text().toStdString()}), true)) {
            say(tr("Sent %1 %2: it arrives with the next block.").arg(amount->text(), label));
            amount->clear();
            to->clear();
        }
    });
    setPanel(m_mine_detail, panel);
}

void BitAssetsPage::showCreatePanel()
{
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("Create an asset"), panel)};
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    layout->addWidget(Dim(tr("A token of your own: shares, points, tickets, a stablecoin. You hold its control coin, which mints more and "
                             "describes it; trade it in a pool, or sell it by auction."), panel));
    auto* name{new QLineEdit(panel)};
    name->setObjectName("nameSearch");
    name->setPlaceholderText(tr("Its name, such as GOLD"));
    name->setMinimumHeight(38);
    name->setMaxLength(64);
    layout->addWidget(name);
    auto* availability{new QLabel(panel)};
    availability->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(availability);

    auto* form{new QFormLayout};
    auto* supply{AmountEdit(tr("0: none yet; mint later"), panel)};
    auto* decimals{new QSpinBox(panel)};
    decimals->setRange(0, 12);
    decimals->setValue(2);
    decimals->setToolTip(tr("With 2 decimals, the smallest amount is 0.01"));
    auto* info{new QLineEdit(panel)};
    info->setPlaceholderText(tr("What it is: a description, a link (optional)"));
    info->setMaxLength(512);
    auto* make_public{new QCheckBox(tr("Publish the name, so that it is listed and found by name"), panel)};
    make_public->setChecked(true);
    form->addRow(tr("Initial supply"), supply);
    form->addRow(tr("Decimals"), decimals);
    form->addRow(tr("Description"), info);
    form->addRow(QString{}, make_public);
    layout->addLayout(form);
    auto* create{new QPushButton(tr("Create"), panel)};
    create->setObjectName("primary");
    create->setEnabled(false);
    layout->addWidget(create, 0, Qt::AlignLeft);
    layout->addWidget(Heading(tr("How it works"), panel));
    layout->addWidget(Dim(tr("1. A reservation hides the name, so that nobody can see it coming and take it first. 2. Once the reservation is "
                             "%n block(s) deep, this page registers it with its supply: not sooner, so that whoever makes a block cannot take "
                             "the name first. 3. After the block after that, it is in your assets. Keep this page open meanwhile, or come back "
                             "to it.", "", m_reveal_depth), panel));
    layout->addStretch();

    auto* check{new QTimer(panel)};
    check->setSingleShot(true);
    check->setInterval(350);
    connect(name, &QLineEdit::textChanged, check, [check] { check->start(); });
    connect(check, &QTimer::timeout, panel, [this, name, availability, create] {
        const QString text{name->text()};
        create->setEnabled(false);
        const auto colour{[&](const char* c) { availability->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;").arg(QLatin1String(c))); }};
        if (text.isEmpty()) return availability->clear();
        if (!GoodName(text)) {
            colour(RED);
            return availability->setText(tr("A name is 1 to 64 plain characters, without a space at either end, and does not read as another asset (CHN, a number like 1739-0029, \"0x\")"));
        }
        if (call("getasset", Args({text.toStdString()}), false, true)) {
            colour(RED);
            return availability->setText(tr("%1 is taken").arg(text));
        }
        colour(GREEN);
        availability->setText(tr("✓ %1 is free").arg(text));
        create->setEnabled(true);
    });
    connect(create, &QPushButton::clicked, this, [this, name, supply, decimals, info, make_public, create] {
        const QString text{name->text()};
        const auto wallet{m_wallet_name()};
        if (!wallet || !create->isEnabled()) return;
        if (!call("reserveasset", Args({text.toStdString()}), true)) return;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("name", text.toStdString());
        entry.pushKV("supply", supply->text().isEmpty() ? std::string{"0"} : supply->text().toStdString());
        entry.pushKV("decimals", decimals->value());
        entry.pushKV("info", info->text().toStdString());
        entry.pushKV("public", make_public->isChecked());
        QSettings settings;
        QStringList pending{settings.value(PendingKey(*wallet)).toStringList()};
        pending << QString::fromStdString(entry.write());
        settings.setValue(PendingKey(*wallet), pending);
        say(tr("%1 is reserved: it is registered by itself once the reservation is %n block(s) deep.", "", m_reveal_depth).arg(text));
        m_mine_listed.clear();
        m_mine_shown = QStringLiteral("pending:") + text;
        refresh();
    });

    setPanel(m_mine_detail, panel);
    name->setFocus();
}

void BitAssetsPage::registerPending()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) return;
    QSettings settings;
    const QStringList pending{settings.value(PendingKey(*wallet)).toStringList()};
    if (pending.isEmpty()) return;
    const auto mine{call("listmyassets", UniValue{UniValue::VARR}, true, true)};
    if (!mine) return;
    QStringList kept;
    for (const QString& entry : pending) {
        UniValue obj;
        if (!obj.read(entry.toStdString())) continue;
        const std::string name{obj["name"].get_str()};
        bool reserved{false};
        for (const UniValue& r : (*mine)["reservations"].getValues()) {
            if (!r.exists("name") || r["name"].get_str() != name) continue;
            reserved = true;
            if (r["taken"].isTrue()) {
                say(tr("%1 was registered by someone else first: release the reservation in My assets.").arg(QString::fromStdString(name)), true);
                continue;
            }
            // Not deep enough yet: a registration now would not be taken. Waited for, without a word.
            if (r.exists("wait") && r["wait"].getInt<int>() > 0) continue;
            // In a block: register it. Its reservation is then spent, and not listed again.
            UniValue data(UniValue::VOBJ);
            if (!obj["info"].get_str().empty()) data.pushKV("info", obj["info"]);
            if (call("registerasset", Args({name, obj["supply"], obj["decimals"], data, obj["public"]}), true)) {
                say(tr("%1 is being registered.").arg(QString::fromStdString(name)));
            }
        }
        // Kept while reserved, or until the asset is registered: the reservation or the registration may wait for a block.
        if (reserved || !call("getasset", Args({name}), false, true)) kept << entry;
    }
    settings.setValue(PendingKey(*wallet), kept);
}

void BitAssetsPage::showAssetPanel(const UniValue& holding)
{
    const QString label{Text(holding["label"])};
    const QString id{QStringLiteral("0x") + Text(holding["asset"])};
    const auto asset{call("getasset", Args({id.toStdString()}), false)};
    if (!asset) return;
    const int decimals{holding["decimals"].getInt<int>()};
    const bool control_pending{holding["control_pending"].isTrue()};
    // While the control coin moves (a mint, a change, waiting for a block), what it does waits too.
    const bool control{holding["control"].isTrue() && !control_pending};
    const bool fixed{(*asset)["fixed"].isTrue()};

    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(label, panel)};
    title->setObjectName("panelTitle");
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    auto* tags{new QHBoxLayout};
    tags->addWidget(Pill(QStringLiteral("#") + Text((*asset)["seq"]), GREY, panel));
    if (control || control_pending) tags->addWidget(Pill(tr("YOU CONTROL IT"), PURPLE, panel));
    if (fixed) tags->addWidget(Pill(tr("FIXED SUPPLY"), GREEN, panel));
    if (!(*asset).exists("name")) tags->addWidget(Pill(tr("PRIVATE NAME"), ORANGE, panel));
    tags->addStretch();
    layout->addLayout(tags);
    auto* balance{new QLabel(tr("%1 %2").arg(Num(holding["balance"]), label), panel)};
    balance->setObjectName("bigNumber");
    layout->addWidget(balance);
    if (holding["pending"].get_real() > 0) {
        auto* pending{new QLabel(tr("+%1 %2 on the way: spendable after the next block").arg(Num(holding["pending"]), label), panel)};
        pending->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(QLatin1String(ORANGE)));
        layout->addWidget(pending);
    }
    if (control_pending) {
        layout->addWidget(Dim(tr("The control coin is moving, with a transaction waiting for a block: minting and changes are possible again after it."), panel));
    }
    layout->addWidget(Dim(tr("of %1 in all · %2 minted · %3 burned · %4 decimals")
                              .arg(Num((*asset)["supply"]), Num((*asset)["minted"]), Num((*asset)["burned"]), QString::number(decimals)), panel));
    if ((*asset).exists("data") && (*asset)["data"].exists("info")) {
        auto* info{new QLabel(Text((*asset)["data"]["info"]), panel)};
        info->setWordWrap(true);
        info->setTextInteractionFlags(Qt::TextBrowserInteraction);
        info->setOpenExternalLinks(true);
        layout->addWidget(info);
    }
    // Its market, if it has one with CHN.
    if (const auto pool{call("getpool", Args({id.toStdString(), "CHN"}), false, true)}) {
        auto* market{new QHBoxLayout};
        market->addWidget(new QLabel(tr("1 %1 ≈ <b>%2 CHN</b> in its pool (%3 %1 / %4 CHN)").arg(label, Text((*pool)["price"]), Num((*pool)["reserve_a"]), Num((*pool)["reserve_b"])), panel));
        market->addStretch();
        auto* trade{new QPushButton(tr("Trade"), panel)};
        market->addWidget(trade);
        layout->addLayout(market);
        connect(trade, &QPushButton::clicked, this, [this, id] { tradeAsset(id); });
    } else {
        auto* market{new QHBoxLayout};
        market->addWidget(Dim(tr("No pool trades it for CHN yet."), panel), 1);
        auto* pool_button{new QPushButton(tr("Make a pool"), panel)};
        market->addWidget(pool_button);
        layout->addLayout(market);
        connect(pool_button, &QPushButton::clicked, this, [this, id] {
            SelectAsset(m_lq_a, id);
            SelectAsset(m_lq_b, QStringLiteral("CHN"));
            m_tabs->setCurrentIndex(1);
            m_lq_amount_a->setFocus();
        });
    }

    // Sending.
    layout->addWidget(Heading(tr("Send"), panel));
    auto* to{new QLineEdit(panel)};
    to->setPlaceholderText(tr("Address"));
    auto* amount{AmountEdit(tr("Amount"), panel)};
    auto* max{new QPushButton(tr("All"), panel)};
    auto* send{new QPushButton(tr("Send"), panel)};
    send->setObjectName("primary");
    layout->addLayout(Row({to, amount, max, send}, {3, 1, 0, 0}));
    connect(max, &QPushButton::clicked, amount, [amount, holding] { amount->setText(Text(holding["balance"])); });
    connect(send, &QPushButton::clicked, this, [this, to, amount, id, label] {
        if (to->text().trimmed().isEmpty() || amount->text().isEmpty()) return say(tr("Give an address and an amount."), true);
        if (!confirm(tr("Send %1").arg(label), tr("Send %1 %2 to %3?").arg(amount->text(), label, to->text().trimmed()))) return;
        if (call("sendasset", Args({to->text().trimmed().toStdString(), id.toStdString(), amount->text().toStdString()}), true)) {
            say(tr("Sent %1 %2: it arrives with the next block.").arg(amount->text(), label));
            amount->clear();
            to->clear();
        }
    });

    if (control && !fixed) {
        layout->addWidget(Heading(tr("Mint more"), panel));
        auto* mint_amount{AmountEdit(tr("Amount"), panel)};
        auto* mint_to{new QLineEdit(panel)};
        mint_to->setPlaceholderText(tr("To: this wallet, or an address"));
        auto* mint{new QPushButton(tr("Mint"), panel)};
        layout->addLayout(Row({mint_amount, mint_to, mint}, {1, 2, 0}));
        connect(mint, &QPushButton::clicked, this, [this, mint_amount, mint_to, id, label] {
            if (mint_amount->text().isEmpty()) return;
            UniValue args{Args({id.toStdString(), mint_amount->text().toStdString()})};
            if (!mint_to->text().trimmed().isEmpty()) args.push_back(mint_to->text().trimmed().toStdString());
            if (call("mintasset", args, true)) {
                say(tr("Minting %1 %2: done with the next block.").arg(mint_amount->text(), label));
                mint_amount->clear();
            }
        });

        // Its data, folded away.
        auto* data_box{new QWidget};
        auto* data_layout{new QFormLayout(data_box)};
        const UniValue data{(*asset).exists("data") ? (*asset)["data"] : UniValue{UniValue::VOBJ}};
        auto* info{new QLineEdit(data.exists("info") ? Text(data["info"]) : QString{}, data_box)};
        info->setMaxLength(512);
        auto* commitment{new QLineEdit(data.exists("commitment") ? Text(data["commitment"]) : QString{}, data_box)};
        commitment->setPlaceholderText(tr("A hash (64 hex digits) of documents kept elsewhere"));
        auto* save{new QPushButton(tr("Save"), data_box)};
        data_layout->addRow(tr("Description"), info);
        data_layout->addRow(tr("Commitment"), commitment);
        data_layout->addRow(QString{}, save);
        layout->addWidget(Foldable(tr("Its description"), data_box, panel));
        connect(save, &QPushButton::clicked, this, [this, info, commitment, data, id, label] {
            UniValue updates(UniValue::VOBJ);
            const auto put{[&](const char* key, const QString& now) {
                const QString was{data.exists(key) ? Text(data[key]) : QString{}};
                if (now == was) return;
                if (now.isEmpty()) {
                    updates.pushKV(key, UniValue{});
                } else {
                    updates.pushKV(key, now.toStdString());
                }
            }};
            put("info", info->text().trimmed());
            put("commitment", commitment->text().trimmed());
            if (updates.empty()) return say(tr("Nothing changed."));
            if (call("updateasset", Args({id.toStdString(), updates}), true)) say(tr("Saved: the description of %1 changes with the next block.").arg(label));
        });

        auto* control_box{new QWidget};
        auto* control_layout{new QVBoxLayout(control_box)};
        auto* give_to{new QLineEdit(control_box)};
        give_to->setPlaceholderText(tr("Address"));
        auto* give{new QPushButton(tr("Give the control coin"), control_box)};
        control_layout->addLayout(Row({give_to, give}, {1, 0}));
        auto* fix{new QPushButton(tr("Fix the supply for good"), control_box)};
        fix->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
        control_layout->addWidget(fix, 0, Qt::AlignLeft);
        control_layout->addWidget(Dim(tr("Fixing the supply burns the control coin: nobody can mint more or change the description, ever. "
                                         "Holders know the supply will never grow."), control_box));
        layout->addWidget(Foldable(tr("Control and supply"), control_box, panel));
        connect(give, &QPushButton::clicked, this, [this, give_to, id, label] {
            const QString address{give_to->text().trimmed()};
            if (address.isEmpty()) return;
            if (!confirm(tr("Give control of %1").arg(label), tr("Give the control coin of %1 to %2? Its holder mints and describes it; only they can give it back.").arg(label, address), true)) return;
            if (call("transferassetcontrol", Args({id.toStdString(), address.toStdString()}), true)) say(tr("The control of %1 goes to %2 with the next block.").arg(label, address));
        });
        connect(fix, &QPushButton::clicked, this, [this, id, label] {
            if (!confirm(tr("Fix the supply of %1").arg(label), tr("Burn the control coin of %1? Nobody will ever mint more of it or change its description. This cannot be undone.").arg(label), true)) return;
            if (call("fixassetsupply", Args({id.toStdString(), true}), true)) say(tr("The supply of %1 is fixed with the next block.").arg(label));
        });
    }

    auto* burn_box{new QWidget};
    auto* burn_layout{new QHBoxLayout(burn_box)};
    auto* burn_amount{AmountEdit(tr("Amount"), burn_box)};
    auto* burn{new QPushButton(tr("Burn"), burn_box)};
    burn->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
    burn_layout->addWidget(burn_amount, 1);
    burn_layout->addWidget(burn);
    layout->addWidget(Foldable(tr("Burn some"), burn_box, panel));
    connect(burn, &QPushButton::clicked, this, [this, burn_amount, id, label] {
        if (burn_amount->text().isEmpty()) return;
        if (!confirm(tr("Burn %1").arg(label), tr("Destroy %1 %2 for good? The supply goes down by as much.").arg(burn_amount->text(), label), true)) return;
        if (call("burnasset", Args({id.toStdString(), burn_amount->text().toStdString()}), true)) say(tr("Burning %1 %2 with the next block.").arg(burn_amount->text(), label));
    });
    layout->addStretch();
    setPanel(m_mine_detail, panel);
}

void BitAssetsPage::showReservationPanel(const UniValue& reservation)
{
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    const QString name{reservation.exists("name") ? Text(reservation["name"]) : tr("A reservation")};
    auto* title{new QLabel(name, panel)};
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    if (reservation["taken"].isTrue()) {
        layout->addWidget(Dim(tr("Someone registered %1 first. The reservation can only be released.").arg(name), panel));
    } else {
        const int wait{reservation.exists("wait") ? reservation["wait"].getInt<int>() : 0};
        layout->addWidget(Dim(wait > 0 ? tr("Being created: reserved, it is registered by itself in %n block(s), once the reservation is deep enough "
                                            "that whoever makes a block cannot take the name first; then it is in your assets after the block after. "
                                            "Keep this page open meanwhile.", "", wait)
                                       : tr("Being created: reserved, it is registered by itself with the next block, then in your assets after the "
                                            "one after. Keep this page open meanwhile."), panel));
    }
    if (reservation.exists("txid") && reservation.exists("name")) {
        auto* release{new QPushButton(tr("Release this reservation"), panel)};
        release->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
        layout->addWidget(release, 0, Qt::AlignLeft);
        connect(release, &QPushButton::clicked, this, [this, name] {
            if (!confirm(tr("Release"), tr("Give up the reservation of %1?").arg(name))) return;
            if (!call("releaseassetreservation", Args({name.toStdString()}), true)) return;
            // Nor register it any more.
            if (const auto wallet{m_wallet_name()}) {
                QSettings settings;
                QStringList pending{settings.value(PendingKey(*wallet)).toStringList()};
                pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const QString& e) {
                    UniValue obj;
                    return obj.read(e.toStdString()) && Text(obj["name"]) == name;
                }), pending.end());
                settings.setValue(PendingKey(*wallet), pending);
            }
            say(tr("The reservation of %1 is released with the next block.").arg(name));
        });
    }
    layout->addStretch();
    setPanel(m_mine_detail, panel);
}

void BitAssetsPage::showLiquidityPanel(const UniValue& position)
{
    const QString a{Text(position["asset_a"])}, b{Text(position["asset_b"])};
    const QString a_arg{AssetArg(position["asset_a_id"])}, b_arg{AssetArg(position["asset_b_id"])};
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("%1 / %2 pool").arg(a, b), panel)};
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    auto* share{new QLabel(tr("%1% of the pool").arg(QString::number(position["percent"].get_real(), 'f', 4)), panel)};
    share->setObjectName("bigNumber");
    layout->addWidget(share);
    layout->addWidget(Dim(tr("Your %1 shares are worth %2 %3 and %4 %5 now, and earn 0.3% of every trade in the pool.")
                              .arg(Text(position["shares"]), Num(position["value_a"]), a, Num(position["value_b"]), b), panel));
    layout->addWidget(Heading(tr("Take liquidity out"), panel));
    auto* percent{new QSpinBox(panel)};
    percent->setRange(1, 100);
    percent->setValue(100);
    percent->setSuffix(QStringLiteral(" %"));
    auto* remove{new QPushButton(tr("Take out"), panel)};
    remove->setObjectName("primary");
    auto* add{new QPushButton(tr("Add more"), panel)};
    auto* row{Row({percent, remove, add})};
    row->addStretch();
    layout->addLayout(row);
    layout->addWidget(Dim(tr("CHN taken out is paid by the next block's coinbase; assets come as coins of the transaction."), panel));
    layout->addStretch();
    connect(remove, &QPushButton::clicked, this, [this, percent, a, b, a_arg, b_arg] {
        if (!confirm(tr("Take liquidity out"), tr("Take %1% of your shares of the %2 / %3 pool out?").arg(percent->value()).arg(a, b))) return;
        if (const auto r{call("removeliquidity", Args({a_arg.toStdString(), b_arg.toStdString(), percent->value()}), true)}) {
            say(tr("Taking out about %1 %2 and %3 %4, with the next block.").arg(Num((*r)["amount_a"]), a, Num((*r)["amount_b"]), b));
        }
    });
    connect(add, &QPushButton::clicked, this, [this, a_arg, b_arg] {
        SelectAsset(m_lq_a, a_arg);
        SelectAsset(m_lq_b, b_arg);
        m_tabs->setCurrentIndex(1);
        m_lq_amount_a->setFocus();
    });
    setPanel(m_mine_detail, panel);
}

void BitAssetsPage::showReceiptPanel(const QString& auction_id)
{
    const auto auction{call("getauction", Args({auction_id.toStdString()}), false)};
    if (!auction) return;
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("Your auction of %1").arg(Text((*auction)["base"])), panel)};
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    const QString status{Text((*auction)["status"])};
    layout->addWidget(Pill(status.toUpper(), status == QLatin1String("open") ? GREEN : status == QLatin1String("upcoming") ? BLUE : PURPLE, panel), 0, Qt::AlignLeft);
    layout->addWidget(new QLabel(tr("%1 of %2 %3 left · brought in %4 %5 from %6 bids")
                                     .arg(Num((*auction)["remaining"]), Num((*auction)["amount"]), Text((*auction)["base"]), Num((*auction)["proceeds"]), Text((*auction)["quote"]), Text((*auction)["bids"])),
                                 panel));
    layout->addWidget(Dim(tr("Bids from block %1 to %2; the price of all of it falls from %3 to %4 %5.")
                              .arg(Text((*auction)["start_height"]), Text((*auction)["end_height"]), Num((*auction)["start_price"]), Num((*auction)["end_price"]), Text((*auction)["quote"])), panel));
    const bool collectable{status == QLatin1String("sold out") || status == QLatin1String("ended") || (*auction)["bids"].getInt<int>() == 0};
    auto* collect{new QPushButton((*auction)["bids"].getInt<int>() == 0 && status != QLatin1String("ended") ? tr("Cancel it: take everything back") : tr("Collect"), panel)};
    collect->setObjectName("primary");
    collect->setEnabled(collectable);
    layout->addWidget(collect, 0, Qt::AlignLeft);
    if (!collectable) layout->addWidget(Dim(tr("It can be collected once it ends (after block %1) or sells out.").arg(Text((*auction)["end_height"])), panel));
    layout->addStretch();
    connect(collect, &QPushButton::clicked, this, [this, auction_id] {
        if (call("collectauction", Args({auction_id.toStdString()}), true)) say(tr("Collecting with the next block: CHN by its coinbase, assets as coins."));
    });
    setPanel(m_mine_detail, panel);
}

//
// Trade.
//

QWidget* BitAssetsPage::createTradeTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    auto* cards{new QHBoxLayout};

    // Swapping.
    auto* swap_card{Card(tab)};
    auto* swap_layout{new QVBoxLayout(swap_card)};
    swap_layout->addWidget(Heading(tr("Swap"), swap_card));
    swap_layout->addWidget(Dim(tr("You pay"), swap_card));
    m_pay_asset = new QComboBox(swap_card);
    m_pay_asset->setMinimumWidth(120);
    m_pay_amount = AmountEdit(tr("0.0"), swap_card);
    swap_layout->addLayout(Row({m_pay_amount, m_pay_asset}, {1, 0}));
    auto* flip{new QPushButton(QStringLiteral("⇅"), swap_card)};
    flip->setToolTip(tr("Swap the other way"));
    flip->setFixedWidth(40);
    swap_layout->addWidget(flip, 0, Qt::AlignHCenter);
    swap_layout->addWidget(Dim(tr("You get"), swap_card));
    m_get_asset = new QComboBox(swap_card);
    m_get_asset->setMinimumWidth(120);
    m_get_amount = new QLabel(QStringLiteral("—"), swap_card);
    m_get_amount->setObjectName("getAmount");
    swap_layout->addLayout(Row({m_get_amount, m_get_asset}, {1, 0}));
    m_quote_line = new QLabel(swap_card);
    m_quote_line->setWordWrap(true);
    m_quote_line->setTextFormat(Qt::RichText);
    swap_layout->addWidget(m_quote_line);
    m_slippage = new QDoubleSpinBox(swap_card);
    m_slippage->setRange(0, 50);
    m_slippage->setDecimals(1);
    m_slippage->setSingleStep(0.5);
    m_slippage->setValue(0.5);
    m_slippage->setSuffix(QStringLiteral(" %"));
    m_slippage->setToolTip(tr("How much less than quoted the trade may give, if the pool moves before it is mined"));
    m_swap = new QPushButton(tr("Swap"), swap_card);
    m_swap->setObjectName("primary");
    m_swap->setEnabled(false);
    auto* slip_row{Row({new QLabel(tr("Slippage"), swap_card), m_slippage})};
    slip_row->addStretch();
    slip_row->addWidget(m_swap);
    swap_layout->addLayout(slip_row);
    swap_layout->addStretch();
    cards->addWidget(swap_card, 1);

    // Adding liquidity.
    auto* lq_card{Card(tab)};
    auto* lq_layout{new QVBoxLayout(lq_card)};
    lq_layout->addWidget(Heading(tr("Add liquidity"), lq_card));
    lq_layout->addWidget(Dim(tr("Put two assets in their pool and earn 0.3% of every trade. A new pool takes the price your amounts set, and opens with at least "
                                "0.01 CHN on its CHN side. A pool everyone leaves closes until someone reopens it, at the price their amounts set."), lq_card));
    m_lq_a = new QComboBox(lq_card);
    m_lq_a->setMinimumWidth(120);
    m_lq_amount_a = AmountEdit(tr("Amount"), lq_card);
    lq_layout->addLayout(Row({m_lq_amount_a, m_lq_a}, {1, 0}));
    m_lq_b = new QComboBox(lq_card);
    m_lq_b->setMinimumWidth(120);
    m_lq_amount_b = AmountEdit(tr("Amount"), lq_card);
    lq_layout->addLayout(Row({m_lq_amount_b, m_lq_b}, {1, 0}));
    m_lq_line = new QLabel(lq_card);
    m_lq_line->setWordWrap(true);
    lq_layout->addWidget(m_lq_line);
    auto* add{new QPushButton(tr("Add"), lq_card)};
    add->setObjectName("primary");
    lq_layout->addWidget(add, 0, Qt::AlignRight);
    lq_layout->addStretch();
    cards->addWidget(lq_card, 1);
    layout->addLayout(cards);

    layout->addWidget(Heading(tr("Pools"), tab));
    m_pools = ViewTable({tr("Pool"), tr("Holds"), tr("Price"), tr("Traded"), tr("Trades")}, tab);
    m_pools->setColumnWidth(0, 160);
    m_pools->setColumnWidth(1, 260);
    m_pools->setColumnWidth(2, 200);
    m_pools->setColumnWidth(3, 260);
    layout->addWidget(m_pools, 1);

    m_quote_timer = new QTimer(this);
    m_quote_timer->setSingleShot(true);
    m_quote_timer->setInterval(300);
    connect(m_quote_timer, &QTimer::timeout, this, &BitAssetsPage::updateQuote);
    for (QComboBox* combo : {m_pay_asset, m_get_asset}) connect(combo, &QComboBox::currentTextChanged, m_quote_timer, qOverload<>(&QTimer::start));
    connect(m_pay_amount, &QLineEdit::textChanged, m_quote_timer, qOverload<>(&QTimer::start));
    connect(m_slippage, &QDoubleSpinBox::valueChanged, m_quote_timer, qOverload<>(&QTimer::start));
    connect(flip, &QPushButton::clicked, this, [this] {
        const QString pay{ArgOf(m_pay_asset)}, get{ArgOf(m_get_asset)};
        SelectAsset(m_pay_asset, get);
        SelectAsset(m_get_asset, pay);
    });
    connect(m_swap, &QPushButton::clicked, this, [this] {
        const QString pay{m_pay_asset->currentText()}, get{m_get_asset->currentText()}, amount{m_pay_amount->text()};
        const QString pay_arg{ArgOf(m_pay_asset)}, get_arg{ArgOf(m_get_asset)};
        if (amount.isEmpty()) return;
        // A noticeable price impact is asked about, with the numbers.
        if (m_last_impact > 2 && !confirm(tr("Price impact"), tr("This trade moves the pool's price: you pay %1% more than its current price for %2 (%3 for %4 %5). Swap anyway?")
                                                                  .arg(QString::number(m_last_impact, 'f', 2), get, m_get_amount->text(), amount, pay), true)) return;
        if (const auto r{call("swapasset", Args({pay_arg.toStdString(), amount.toStdString(), get_arg.toStdString(), m_slippage->value()}), true)}) {
            say(tr("Swapping %1 %2 for about %3 %4 (at least %5) with the next block.").arg(amount, pay, Num((*r)["quote"]), get, Num((*r)["min_out"])));
            m_pay_amount->clear();
        }
    });

    m_lq_timer = new QTimer(this);
    m_lq_timer->setSingleShot(true);
    m_lq_timer->setInterval(300);
    connect(m_lq_timer, &QTimer::timeout, this, &BitAssetsPage::updateLiquidityQuote);
    for (QComboBox* combo : {m_lq_a, m_lq_b}) connect(combo, &QComboBox::currentTextChanged, m_lq_timer, qOverload<>(&QTimer::start));
    connect(m_lq_amount_a, &QLineEdit::textEdited, m_lq_timer, qOverload<>(&QTimer::start));
    connect(m_lq_amount_b, &QLineEdit::textEdited, m_lq_timer, qOverload<>(&QTimer::start));
    connect(add, &QPushButton::clicked, this, [this] {
        const QString a{m_lq_a->currentText()}, b{m_lq_b->currentText()};
        const QString a_arg{ArgOf(m_lq_a)}, b_arg{ArgOf(m_lq_b)};
        if (a_arg == b_arg || m_lq_amount_a->text().isEmpty()) return say(tr("Pick two different assets and an amount."), true);
        UniValue args{Args({a_arg.toStdString(), m_lq_amount_a->text().toStdString(), b_arg.toStdString()})};
        // Into a pool someone provides liquidity to, the wallet works the second amount out at the
        // pool's price. A new pool, or one nobody provides liquidity to any more (what it holds is
        // dust, at a price anyone could have set), takes both amounts, which set its price.
        const auto pool{call("getpool", Args({a_arg.toStdString(), b_arg.toStdString()}), false, true)};
        const bool abandoned{pool && (*pool)["abandoned"].isTrue()};
        const bool sets_price{!pool || abandoned};
        // The pool changed since the form was filled in (made, reopened or left meanwhile): the
        // amounts typed were for another case. Shown again, and asked for again.
        if (sets_price != m_lq_sets_price) {
            updateLiquidityQuote();
            return say(sets_price ? tr("Nobody provides liquidity to this pool any more since you typed the amounts: give both amounts, which set its price, and click Add again.")
                                  : tr("A pool of these assets was made meanwhile: its price sets the second amount now. Check the amounts and click Add again."), true);
        }
        if (sets_price) {
            if (m_lq_amount_b->text().isEmpty()) return say(abandoned ? tr("Nobody provides liquidity to this pool: give both amounts, which set its price.") : tr("A new pool: give both amounts, which set its price."), true);
            args.push_back(m_lq_amount_b->text().toStdString());
            const QString question{abandoned ? tr("Reopen the %1 / %2 pool with %3 %1 and %4 %2? Nobody provides liquidity to it: these amounts set its price, "
                                                  "whatever price it was left at. Check it is the price you want. The dust it holds (%5 %1 and %6 %2) counts "
                                                  "toward them: you put in the rest. Should someone reopen it first, your deposit goes in at their price, "
                                                  "what it does not take comes back, and it is refused if it would give fewer shares than quoted.")
                                                   .arg(a, b, m_lq_amount_a->text(), m_lq_amount_b->text(), Num((*pool)["reserve_a"]), Num((*pool)["reserve_b"]))
                                             : tr("Make the %1 / %2 pool with %3 %1 and %4 %2? Its price starts at what these amounts set.").arg(a, b, m_lq_amount_a->text(), m_lq_amount_b->text())};
            if (!confirm(abandoned ? tr("Reopen a pool") : tr("Make a pool"), question, abandoned)) return;
        }
        if (const auto r{call("addliquidity", args, true)}) {
            if (sets_price) {
                say(tr("%1 the %2 / %3 pool with %4 %2 and %5 %3, with the next block.").arg(abandoned ? tr("Reopening") : tr("Making"), a, b, m_lq_amount_a->text(), m_lq_amount_b->text()));
            } else {
                say(tr("Adding %1 %2 and %3 %4 to their pool with the next block, at its price: should it move first, what its price does not take comes back.")
                        .arg(Num((*r)["amount_a"]), a, Num((*r)["amount_b"]), b));
            }
            m_lq_amount_a->clear();
            m_lq_amount_b->clear();
            m_pools_listed.clear();
        }
    });
    connect(m_pools, &QTableWidget::cellClicked, this, [this](int row) {
        const QStringList pair{m_pools->item(row, 0)->data(Qt::UserRole).toStringList()};
        if (pair.size() != 2) return;
        SelectAsset(m_pay_asset, pair[1]);
        SelectAsset(m_get_asset, pair[0]);
        SelectAsset(m_lq_a, pair[0]);
        SelectAsset(m_lq_b, pair[1]);
    });
    return tab;
}

void BitAssetsPage::tradeAsset(const QString& asset)
{
    SelectAsset(m_pay_asset, QStringLiteral("CHN"));
    SelectAsset(m_get_asset, asset);
    m_tabs->setCurrentIndex(1);
    m_pay_amount->setFocus();
}

void BitAssetsPage::updateQuote()
{
    if (!m_client_model) return;
    const QString pay{m_pay_asset->currentText()}, get{m_get_asset->currentText()}, amount{m_pay_amount->text()};
    const QString pay_arg{ArgOf(m_pay_asset)}, get_arg{ArgOf(m_get_asset)};
    m_swap->setEnabled(false);
    m_get_amount->setText(QStringLiteral("—"));
    if (pay_arg.isEmpty() || get_arg.isEmpty()) return m_quote_line->clear();
    if (pay_arg == get_arg) return m_quote_line->setText(tr("Pick two different assets."));
    const auto pool{call("getpool", Args({get_arg.toStdString(), pay_arg.toStdString()}), false, true)};
    if (!pool) {
        m_quote_line->setText(tr("<span style='color:%1'>No pool trades %2 for %3 yet.</span> Make one with Add liquidity.").arg(QLatin1String(ORANGE), pay, get));
        return;
    }
    // A pool nobody provides liquidity to: whatever goes in buys dust, and stays there.
    if ((*pool)["abandoned"].isTrue()) {
        m_quote_line->setText(tr("<span style='color:%1'><b>This pool is closed: nobody provides liquidity to it any more.</b></span> It holds only the minimum every "
                                 "pool keeps for good (%2 %3 and %4 %5). Adding liquidity reopens it.")
                                  .arg(QLatin1String(RED), Num((*pool)["reserve_a"]), get, Num((*pool)["reserve_b"]), pay));
        return;
    }
    const QString spot{tr("1 %1 = %2 %3").arg(get, Text((*pool)["price"]), pay)};
    if (amount.isEmpty() || amount.toDouble() == 0) return m_quote_line->setText(spot);
    QString error;
    const auto quote{NodeRpc::Call(m_client_model, "quoteswap", Args({pay_arg.toStdString(), amount.toStdString(), get_arg.toStdString()}), error)};
    if (!quote) return m_quote_line->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(RED), error.toHtmlEscaped()));
    m_get_amount->setText(Num((*quote)["amount_out"]));
    // CHN a pool pays out is at least MIN_CHN_PAYOUT (the second audit's rules): less is refused.
    if (get_arg == QLatin1String("CHN")) {
        const auto info{call("getbitassetsinfo", UniValue{UniValue::VARR}, false, true)};
        const bool audit2{info && (*info)["height"].getInt<int>() >= (*info)["audit2_height"].getInt<int>()};
        const double least{static_cast<double>(bitassets::MIN_CHN_PAYOUT) / COIN};
        if (audit2 && QString::fromStdString((*quote)["amount_out"].getValStr()).toDouble() < least) {
            m_quote_line->setText(tr("<span style='color:%1'>Too little: the least CHN a pool pays out is %2 CHN.</span>").arg(QLatin1String(RED), QString::number(least, 'f', 8)));
            return;
        }
    }
    const double impact{(*quote)["price_impact"].get_real()};
    const int decimals{m_decimals.contains(get_arg) ? m_decimals.at(get_arg) : 0};
    const double least{QString::fromStdString((*quote)["amount_out"].getValStr()).toDouble() * (1 - m_slippage->value() / 100)};
    m_last_impact = impact;
    m_quote_line->setText(tr("%1 · price impact <span style='color:%2'>%3%</span> · at least %4 %5 · pool fee %6 %7")
                              .arg(spot, QLatin1String(impact > 5 ? RED : impact > 1 ? ORANGE : GREEN), QString::number(impact, 'f', 2),
                                   Grouped(QString::number(least, 'f', decimals)), get, Num((*quote)["fee"]), pay));
    // Too large for the pool: the wallet refuses it too (swapasset's max_impact).
    if (impact > 10) {
        m_quote_line->setText(m_quote_line->text() + tr("<br><span style='color:%1'><b>Too large for this pool:</b> you would pay %2% more than its price. "
                                                         "Trade less, or wait for more liquidity.</span>").arg(QLatin1String(RED), QString::number(impact, 'f', 1)));
        return;
    }
    m_swap->setEnabled(true);
}

void BitAssetsPage::updateLiquidityQuote()
{
    if (!m_client_model) return;
    const QString a{m_lq_a->currentText()}, b{m_lq_b->currentText()};
    const QString a_arg{ArgOf(m_lq_a)}, b_arg{ArgOf(m_lq_b)};
    if (a_arg.isEmpty() || b_arg.isEmpty() || a_arg == b_arg) return m_lq_line->setText(tr("Pick two different assets."));
    const auto pool{call("getpool", Args({a_arg.toStdString(), b_arg.toStdString()}), false, true)};
    const bool abandoned{pool && (*pool)["abandoned"].isTrue()};
    if (!pool || abandoned) {
        // Typed in when the form changes from a pool there is: what it showed there was its price.
        if (!m_lq_sets_price) m_lq_amount_b->clear();
        m_lq_sets_price = true;
        m_lq_amount_b->setReadOnly(false);
        m_lq_amount_b->setPlaceholderText(tr("Amount"));
        const double qa{m_lq_amount_a->text().toDouble()}, qb{m_lq_amount_b->text().toDouble()};
        const QString price{qa > 0 && qb > 0 ? tr("at 1 %1 = %2 %3").arg(a, QString::number(qb / qa, 'g', 10), b) : QString{}};
        if (abandoned) {
            m_lq_line->setText(tr("<span style='color:%1'><b>Nobody provides liquidity to this pool any more.</b></span> It holds only the dust every pool "
                                  "keeps (%3 %4 and %5 %6), at whatever price it was left at (anyone can set it): your amounts set its price, as for a new "
                                  "pool, and the dust counts toward them. %2")
                                   .arg(QLatin1String(ORANGE), price.isEmpty() ? tr("Give both amounts.") : tr("It reopens %1.").arg(price),
                                        Num((*pool)["reserve_a"]), a, Num((*pool)["reserve_b"]), b));
        } else {
            m_lq_line->setText(price.isEmpty() ? tr("A new pool: your amounts set its price.") : tr("A new pool, %1.").arg(price));
        }
        return;
    }
    // The second amount follows from the first, at the pool's price.
    m_lq_sets_price = false;
    m_lq_amount_b->setReadOnly(true);
    m_lq_amount_b->setPlaceholderText(tr("Worked out from the pool's price"));
    const double price{(*pool)["price"].get_real()};
    const double qa{m_lq_amount_a->text().toDouble()};
    const int decimals{m_decimals.contains(b_arg) ? m_decimals.at(b_arg) : 0};
    m_lq_amount_b->setText(qa > 0 ? QString::number(qa * price, 'f', decimals) : QString{});
    m_lq_line->setText(tr("1 %1 = %2 %3 · the pool holds %4 %1 and %5 %3").arg(a, Text((*pool)["price"]), b, Num((*pool)["reserve_a"]), Num((*pool)["reserve_b"])));
}

void BitAssetsPage::refreshTrade()
{
    const auto pools{call("listpools", UniValue{UniValue::VARR}, false, true)};
    if (!pools) return;
    const std::string listed{pools->write()};
    if (listed == m_pools_listed) return;
    m_pools_listed = listed;
    m_pools->setRowCount(0);
    for (const UniValue& p : pools->getValues()) {
        const int row{m_pools->rowCount()};
        m_pools->insertRow(row);
        const QString a{Text(p["asset_a"])}, b{Text(p["asset_b"])};
        auto* pair{Item(QStringLiteral("%1 / %2").arg(a, b))};
        pair->setData(Qt::UserRole, QStringList{AssetArg(p["asset_a_id"]), AssetArg(p["asset_b_id"])});
        m_pools->setItem(row, 0, pair);
        if (p["abandoned"].isTrue()) {
            auto* empty{Item(tr("no liquidity: %1 %2 · %3 %4 kept for good").arg(Num(p["reserve_a"]), a, Num(p["reserve_b"]), b))};
            empty->setForeground(QColor{GREY});
            empty->setToolTip(tr("Nobody provides liquidity to this pool any more: trading in it gets almost nothing. Add liquidity to bring it back."));
            m_pools->setItem(row, 1, empty);
        } else {
            m_pools->setItem(row, 1, Item(QStringLiteral("%1 %2 · %3 %4").arg(Num(p["reserve_a"]), a, Num(p["reserve_b"]), b)));
        }
        m_pools->setItem(row, 2, Item(p.exists("price") ? QStringLiteral("1 %1 = %2 %3").arg(a, Text(p["price"]), b) : QStringLiteral("—")));
        m_pools->setItem(row, 3, Item(QStringLiteral("%1 %2 · %3 %4").arg(Num(p["volume_a"]), a, Num(p["volume_b"]), b)));
        m_pools->setItem(row, 4, Item(Text(p["swaps"])));
    }
    if (m_pools->rowCount() == 0) {
        m_pools->insertRow(0);
        m_pools->setItem(0, 0, Item(tr("No pools yet: add liquidity to make the first.")));
    }
    // The prices moved: so do the quotes.
    m_quote_timer->start();
}

//
// Auctions.
//

QWidget* BitAssetsPage::createAuctionsTab()
{
    auto* split{new QSplitter(Qt::Horizontal, this)};
    split->setChildrenCollapsible(false);
    auto* left{new QWidget(split)};
    auto* left_layout{new QVBoxLayout(left)};
    left_layout->setContentsMargins(0, 6, 0, 0);
    m_auctions = ViewTable({tr("Selling"), tr("For"), tr("Price now"), tr("Status"), tr("Ends")}, left);
    m_auctions->setColumnWidth(0, 160);
    m_auctions->setColumnWidth(1, 70);
    m_auctions->setColumnWidth(2, 200);
    m_auctions->setColumnWidth(3, 90);
    left_layout->addWidget(m_auctions, 1);

    // Selling by auction.
    auto* sell_card{Card(left)};
    auto* sell_layout{new QVBoxLayout(sell_card)};
    sell_layout->addWidget(Heading(tr("Sell by auction"), sell_card));
    auto* form{new QFormLayout};
    m_sell_asset = new QComboBox(sell_card);
    m_sell_amount = AmountEdit(tr("How much"), sell_card);
    auto* what{Row({m_sell_amount, m_sell_asset}, {1, 0})};
    form->addRow(tr("Sell"), what);
    m_sell_for = new QComboBox(sell_card);
    form->addRow(tr("For"), m_sell_for);
    m_sell_start = AmountEdit(tr("for all of it, at the start"), sell_card);
    m_sell_end = AmountEdit(tr("for all of it, at the end: the least you take"), sell_card);
    form->addRow(tr("Start price"), m_sell_start);
    form->addRow(tr("End price"), m_sell_end);
    m_sell_duration = new QSpinBox(sell_card);
    m_sell_duration->setRange(1, 1'000'000);
    m_sell_duration->setValue(60);
    m_sell_duration->setSuffix(tr(" blocks"));
    m_sell_start_in = new QSpinBox(sell_card);
    m_sell_start_in->setRange(1, 100'000);
    m_sell_start_in->setValue(1);
    m_sell_start_in->setSuffix(tr(" blocks"));
    form->addRow(tr("Runs for"), m_sell_duration);
    form->addRow(tr("Starts in"), m_sell_start_in);
    sell_layout->addLayout(form);
    auto* sell{new QPushButton(tr("Start the auction"), sell_card)};
    sell->setObjectName("primary");
    sell_layout->addWidget(sell, 0, Qt::AlignRight);
    left_layout->addWidget(sell_card);
    split->addWidget(left);

    m_auction_detail = new QScrollArea(split);
    m_auction_detail->setWidgetResizable(true);
    m_auction_detail->setFrameShape(QFrame::NoFrame);
    m_auction_detail->setWidget(Dim(tr("Pick an auction to bid on it. The price falls block by block: the longer you wait, the less you pay — "
                                       "if anything is left."), m_auction_detail));
    split->addWidget(m_auction_detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    connect(m_auctions, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || !m_auctions->item(row, 0)) return;
        const QString id{m_auctions->item(row, 0)->data(Qt::UserRole).toString()};
        if (id.isEmpty() || id == m_auction_shown) return;
        m_auction_shown = id;
        showAuctionDetail();
    });
    connect(sell, &QPushButton::clicked, this, [this] {
        const QString base{m_sell_asset->currentText()}, quote{m_sell_for->currentText()};
        const QString base_arg{ArgOf(m_sell_asset)}, quote_arg{ArgOf(m_sell_for)};
        if (base_arg == quote_arg) return say(tr("An auction sells one asset for another."), true);
        if (m_sell_amount->text().isEmpty() || m_sell_start->text().isEmpty() || m_sell_end->text().isEmpty()) return say(tr("Give the amount and both prices."), true);
        if (m_sell_end->text().toDouble() > m_sell_start->text().toDouble()) return say(tr("The price falls: the end price is at most the start price."), true);
        if (!confirm(tr("Start an auction"), tr("Sell %1 %2 for %3, from %4 down to %5 %3 for all of it, over %6 blocks?")
                                                  .arg(m_sell_amount->text(), base, quote, m_sell_start->text(), m_sell_end->text())
                                                  .arg(m_sell_duration->value()))) return;
        if (call("createauction", Args({base_arg.toStdString(), m_sell_amount->text().toStdString(), quote_arg.toStdString(), m_sell_start->text().toStdString(),
                                        m_sell_end->text().toStdString(), m_sell_duration->value(), m_sell_start_in->value()}), true)) {
            say(tr("The auction starts after the next block. Its receipt, in My assets, collects what it brings in."));
            m_sell_amount->clear();
            m_auctions_listed.clear();
        }
    });
    return split;
}

void BitAssetsPage::refreshAuctions()
{
    const auto auctions{call("listauctions", UniValue{UniValue::VARR}, false, true)};
    if (!auctions) return;
    const std::string listed{auctions->write()};
    if (listed == m_auctions_listed) return;
    m_auctions_listed = listed;
    const QSignalBlocker blocker{m_auctions};
    m_auctions->setRowCount(0);
    int picked{-1};
    for (const UniValue& a : auctions->getValues()) {
        const int row{m_auctions->rowCount()};
        m_auctions->insertRow(row);
        const QString id{Text(a["auction"])};
        auto* selling{Item(QStringLiteral("%1 of %2 %3").arg(Num(a["remaining"]), Num(a["amount"]), Text(a["base"])))};
        selling->setData(Qt::UserRole, id);
        m_auctions->setItem(row, 0, selling);
        m_auctions->setItem(row, 1, Item(Text(a["quote"])));
        m_auctions->setItem(row, 2, Item(UnitPrice(a["unit_price"], Text(a["base"]), Text(a["quote"]))));
        const QString status{Text(a["status"])};
        auto* status_item{Item(status)};
        status_item->setForeground(QColor{status == QLatin1String("open") ? GREEN : status == QLatin1String("upcoming") ? BLUE : GREY});
        m_auctions->setItem(row, 3, status_item);
        m_auctions->setItem(row, 4, Item(tr("block %1").arg(Text(a["end_height"]))));
        if (id == m_auction_shown) picked = row;
    }
    if (m_auctions->rowCount() == 0) {
        m_auctions->insertRow(0);
        m_auctions->setItem(0, 0, Item(tr("No auctions running.")));
    }
    if (picked >= 0) {
        m_auctions->selectRow(picked);
        // Its price moved: the panel is rebuilt unless it is being typed in.
        QWidget* focus{QApplication::focusWidget()};
        if (!m_auction_detail->widget() || !focus || !m_auction_detail->widget()->isAncestorOf(focus)) showAuctionDetail();
    }
}

void BitAssetsPage::showAuctionDetail()
{
    const auto auction{call("getauction", Args({m_auction_shown.toStdString()}), false)};
    if (!auction) return;
    const QString id{m_auction_shown};
    const QString base{Text((*auction)["base"])}, quote{Text((*auction)["quote"])}, status{Text((*auction)["status"])};
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("%1 %2 for %3").arg(Num((*auction)["remaining"]), base, quote), panel)};
    title->setObjectName("panelTitle");
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(Pill(status.toUpper(), status == QLatin1String("open") ? GREEN : status == QLatin1String("upcoming") ? BLUE : GREY, panel), 0, Qt::AlignLeft);
    auto* price{new QLabel(UnitPrice((*auction)["unit_price"], base, quote), panel)};
    price->setObjectName("bigNumber");
    layout->addWidget(price);
    layout->addWidget(Dim(tr("in the next block · all of what is left: %1 %2 · %3 of %4 left · %5 bids")
                              .arg(Num((*auction)["price"]), quote, Num((*auction)["remaining"]), Num((*auction)["amount"]), Text((*auction)["bids"])), panel));
    layout->addWidget(Dim(tr("The price of all of it falls from %1 to %2 %3, from block %4 to block %5.")
                              .arg(Num((*auction)["start_price"]), Num((*auction)["end_price"]), quote, Text((*auction)["start_height"]), Text((*auction)["end_height"])), panel));
    if (status == QLatin1String("open")) {
        layout->addWidget(Heading(tr("Bid"), panel));
        auto* amount{AmountEdit(tr("What you pay, in %1").arg(quote), panel)};
        auto* bid{new QPushButton(tr("Bid"), panel)};
        bid->setObjectName("primary");
        auto* all{new QPushButton(tr("Buy all that is left"), panel)};
        layout->addLayout(Row({amount, bid, all}, {1, 0, 0}));
        auto* buys{new QLabel(panel)};
        layout->addWidget(buys);
        connect(amount, &QLineEdit::textChanged, panel, [this, amount, buys, id, base] {
            if (amount->text().isEmpty()) return buys->clear();
            QString error;
            const auto q{NodeRpc::Call(m_client_model, "quotebid", Args({id.toStdString(), amount->text().toStdString()}), error)};
            buys->setText(q ? tr("buys %1 %2 in the next block, more if it waits").arg(Num((*q)["buys"]), base) : error);
        });
        connect(bid, &QPushButton::clicked, this, [this, amount, id, base, quote] {
            if (amount->text().isEmpty()) return;
            if (const auto r{call("bidauction", Args({id.toStdString(), amount->text().toStdString()}), true)}) {
                say(tr("Bidding %1 %2 for at least %3 %4, with the next block.").arg(Num((*r)["pays"]), quote, Num((*r)["buys"]), base));
                amount->clear();
            }
        });
        connect(all, &QPushButton::clicked, this, [this, id, base, quote] {
            // What is left now, and what all of it costs in the next block: not the price of all the auction sold.
            const auto now{call("getauction", Args({id.toStdString()}), false)};
            if (!now) return;
            // The wallet pays exactly that (less, should the price fall first), never more: it is the most it is given.
            if (!confirm(tr("Buy all"), tr("Pay %1 %2 for the %3 %4 left?").arg(Num((*now)["cost_of_remaining"]), quote, Num((*now)["remaining"]), base))) return;
            if (const auto r{call("bidauction", Args({id.toStdString(), (*now)["cost_of_remaining"], true}), true)}) {
                say(tr("Bidding %1 %2 for the %3 %4 left, with the next block.").arg(Num((*r)["pays"]), quote, Num((*r)["buys"]), base));
            }
        });
    } else if (status == QLatin1String("upcoming")) {
        layout->addWidget(Dim(tr("It takes bids from block %1.").arg(Text((*auction)["start_height"])), panel));
    }
    layout->addStretch();
    setPanel(m_auction_detail, panel);
}

//
// Explore and activity.
//

QWidget* BitAssetsPage::createExploreTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    layout->setContentsMargins(0, 6, 0, 0);
    m_search = new QLineEdit(tab);
    m_search->setPlaceholderText(tr("Find an asset by its name, number (1739-0029) or hash (0x…)"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumHeight(28);
    layout->addWidget(m_search);
    auto* split{new QSplitter(Qt::Horizontal, tab)};
    split->setChildrenCollapsible(false);
    m_all = PickList(split);
    m_all->setMinimumWidth(220);
    split->addWidget(m_all);
    m_all_detail = new QScrollArea(split);
    m_all_detail->setWidgetResizable(true);
    m_all_detail->setFrameShape(QFrame::NoFrame);
    m_all_detail->setWidget(Dim(tr("Pick an asset."), m_all_detail));
    split->addWidget(m_all_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);
    connect(m_all, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item && !item->data(Qt::UserRole).toString().isEmpty()) showExploreDetail(item->data(Qt::UserRole).toString());
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& words) {
        for (int i{0}; i < m_all->count(); ++i) m_all->item(i)->setHidden(!words.isEmpty() && !m_all->item(i)->text().contains(words, Qt::CaseInsensitive));
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QString query{m_search->text().trimmed()};
        if (query.isEmpty()) return;
        if (const auto asset{call("getasset", Args({query.toStdString()}), false)}) showExploreDetail(QStringLiteral("0x") + Text((*asset)["asset"]));
    });
    return tab;
}

void BitAssetsPage::refreshExplore()
{
    const auto assets{call("listassets", Args({1000, 0}), false, true)};
    if (!assets) return;
    const std::string listed{assets->write()};
    if (listed == m_all_listed) return;
    m_all_listed = listed;
    const QString picked{m_all->currentItem() ? m_all->currentItem()->data(Qt::UserRole).toString() : QString{}};
    const QSignalBlocker blocker{m_all};
    m_all->clear();
    for (const UniValue& a : assets->getValues()) {
        const QString id{QStringLiteral("0x") + Text(a["asset"])};
        auto* item{new QListWidgetItem(QStringLiteral("%1   #%2").arg(Text(a["label"]).left(40), Text(a["seq"])), m_all)};
        item->setData(Qt::UserRole, id);
        if (id == picked) m_all->setCurrentItem(item);
    }
    if (m_all->count() == 0) {
        auto* item{new QListWidgetItem(tr("No assets yet."), m_all)};
        item->setFlags(Qt::NoItemFlags);
    }
    if (!m_search->text().isEmpty()) Q_EMIT m_search->textChanged(m_search->text());
}

void BitAssetsPage::showExploreDetail(const QString& asset_id)
{
    const auto asset{call("getasset", Args({asset_id.toStdString()}), false)};
    if (!asset) return;
    const QString label{Text((*asset)["label"])};
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(label, panel)};
    title->setObjectName("panelTitle");
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* tags{new QHBoxLayout};
    tags->addWidget(Pill(QStringLiteral("#") + Text((*asset)["seq"]), GREY, panel));
    if ((*asset)["fixed"].isTrue()) tags->addWidget(Pill(tr("FIXED SUPPLY"), GREEN, panel));
    tags->addStretch();
    layout->addLayout(tags);
    auto* supply{new QLabel(tr("%1 %2").arg(Num((*asset)["supply"]), label), panel)};
    supply->setObjectName("bigNumber");
    layout->addWidget(supply);
    layout->addWidget(Dim(tr("in all · %1 minted · %2 burned · %3 decimals · registered in block %4")
                              .arg(Num((*asset)["minted"]), Num((*asset)["burned"]), Text((*asset)["decimals"]), Text((*asset)["registered"])), panel));
    if ((*asset).exists("data") && (*asset)["data"].exists("info")) {
        auto* info{new QLabel(Text((*asset)["data"]["info"]), panel)};
        info->setWordWrap(true);
        info->setTextInteractionFlags(Qt::TextBrowserInteraction);
        info->setOpenExternalLinks(true);
        layout->addWidget(info);
    }
    if ((*asset).exists("control") && (*asset)["control"].exists("address")) {
        auto* control{Dim(tr("Controlled by %1").arg(Text((*asset)["control"]["address"])), panel)};
        control->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(control);
    }
    // A dead asset: anyone may retire it.
    if ((*asset)["releasable"].isTrue()) {
        auto* dead{new QFrame(panel)};
        dead->setObjectName("card");
        auto* dead_layout{new QVBoxLayout(dead)};
        dead_layout->addWidget(Heading(tr("This asset is dead"), dead));
        dead_layout->addWidget(Dim(tr("Its supply is fixed, nobody holds any of it, and its pools have no liquidity providers. Retiring it deletes it and its "
                                      "pools and frees its name; the %1 CHN its pools hold go to mainchain miners, as a fee. Anyone may do it.")
                                       .arg(Num((*asset)["release_fee"])), dead));
        auto* retire{new QPushButton(tr("Retire %1").arg(label), dead)};
        retire->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
        dead_layout->addWidget(retire, 0, Qt::AlignLeft);
        layout->addWidget(dead);
        connect(retire, &QPushButton::clicked, this, [this, asset_id, label] {
            if (!confirm(tr("Retire %1").arg(label), tr("Retire %1 for good? It and its pools are deleted; the CHN in its pools go to mainchain miners.").arg(label), true)) return;
            if (call("releaseasset", Args({asset_id.toStdString()}), true)) say(tr("%1 is retired with the next block.").arg(label));
        });
    }
    auto* hash{Dim(tr("Hash %1").arg(Text((*asset)["asset"])), panel)};
    hash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(hash);
    // Its pools.
    if (const auto pools{call("listpools", Args({asset_id.toStdString()}), false, true)}; pools && !pools->empty()) {
        layout->addWidget(Heading(tr("Markets"), panel));
        for (const UniValue& p : pools->getValues()) {
            auto* row{new QHBoxLayout};
            row->addWidget(new QLabel(tr("1 %1 = <b>%2 %3</b> · pool of %4 %1 and %5 %3").arg(label, Text(p["price"]), Text(p["asset_b"]), Num(p["reserve_a"]), Num(p["reserve_b"])), panel), 1);
            auto* trade{new QPushButton(tr("Trade"), panel)};
            row->addWidget(trade);
            layout->addLayout(row);
            const QString other{AssetArg(p["asset_b_id"])};
            connect(trade, &QPushButton::clicked, this, [this, asset_id, other] {
                SelectAsset(m_pay_asset, other);
                SelectAsset(m_get_asset, asset_id);
                m_tabs->setCurrentIndex(1);
                m_pay_amount->setFocus();
            });
        }
    }
    // Its history.
    auto* history{ViewTable({tr("Field"), tr("Value"), tr("Block")}, nullptr)};
    history->setMinimumHeight(140);
    if (const auto h{call("getassethistory", Args({asset_id.toStdString()}), false, true)}) {
        // An asset's description and commitment; the other fields of the data (addresses and keys,
        // from BitNames) are not shown.
        const std::map<std::string, QString> shown{{"info", tr("Description")}, {"commitment", tr("Commitment")}};
        for (const auto& [field, name] : shown) {
            if (!h->exists(field)) continue;
            const auto& entries{(*h)[field].getValues()};
            for (size_t i{0}; i < entries.size(); ++i) {
                const UniValue& entry{entries[i]};
                // The registration stamps every field, set or not: an empty first value is not a change.
                if (entry["value"].isNull() && i == 0) continue;
                const int row{history->rowCount()};
                history->insertRow(row);
                history->setItem(row, 0, Item(name));
                history->setItem(row, 1, Item(entry["value"].isNull() ? tr("(none)") : Text(entry["value"])));
                auto* height{new QTableWidgetItem};
                height->setData(Qt::DisplayRole, entry["height"].getInt<int>());
                height->setFlags(height->flags() & ~Qt::ItemIsEditable);
                history->setItem(row, 2, height);
            }
        }
        history->sortItems(2, Qt::DescendingOrder);
    }
    if (history->rowCount() > 0) {
        layout->addWidget(Foldable(tr("History of its description"), history, panel));
    } else {
        delete history;
    }
    layout->addStretch();
    setPanel(m_all_detail, panel);
}

QWidget* BitAssetsPage::createActivityTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    layout->setContentsMargins(0, 6, 0, 0);
    m_activity = ViewTable({tr("When"), tr("What"), tr("Confirmations"), tr("Transaction")}, tab);
    m_activity->setColumnWidth(0, 140);
    m_activity->setColumnWidth(1, 520);
    m_activity->setColumnWidth(2, 100);
    layout->addWidget(m_activity, 1);
    return tab;
}

void BitAssetsPage::refreshActivity()
{
    const auto activity{call("listassetactivity", Args({200}), true, true)};
    if (!activity) return;
    const std::string listed{activity->write()};
    if (listed == m_activity_listed) return;
    m_activity_listed = listed;
    m_activity->setRowCount(0);
    for (const UniValue& a : activity->getValues()) {
        const int row{m_activity->rowCount()};
        m_activity->insertRow(row);
        m_activity->setItem(row, 0, Item(QDateTime::fromSecsSinceEpoch(a["time"].getInt<int64_t>()).toString(QStringLiteral("yyyy-MM-dd hh:mm"))));
        m_activity->setItem(row, 1, Item(Text(a["summary"])));
        const int confirmations{a["confirmations"].getInt<int>()};
        auto* conf{Item(confirmations == 0 ? tr("waiting") : QString::number(confirmations))};
        if (confirmations == 0) conf->setForeground(QColor{ORANGE});
        m_activity->setItem(row, 2, conf);
        m_activity->setItem(row, 3, Item(Text(a["txid"])));
    }
    if (m_activity->rowCount() == 0) {
        m_activity->insertRow(0);
        m_activity->setItem(0, 1, Item(tr("Nothing yet.")));
    }
}

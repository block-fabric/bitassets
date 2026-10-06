// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/noderpc.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>

#include <QObject>
#include <QTableWidgetItem>
#include <QUrl>

namespace NodeRpc {

std::optional<UniValue> Call(ClientModel* client_model, const std::string& method, const UniValue& params, QString& error, const std::optional<QString>& wallet)
{
    if (!client_model) {
        error = QObject::tr("The node is not available.");
        return std::nullopt;
    }
    std::string uri{"/"};
    if (wallet) uri = "/wallet/" + QString::fromUtf8(QUrl::toPercentEncoding(*wallet)).toStdString();
    try {
        return client_model->node().executeRpc(method, params, uri);
    } catch (const UniValue& rpc_error) {
        error = rpc_error.isObject() && rpc_error.exists("message") ? Text(rpc_error["message"]) : QString::fromStdString(rpc_error.write());
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
    }
    return std::nullopt;
}

UniValue Args(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const auto& value : values) params.push_back(value);
    return params;
}

QString Text(const UniValue& value)
{
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString::fromStdString(value.getValStr());
}

QTableWidgetItem* Item(const QString& text)
{
    auto* item{new QTableWidgetItem(text)};
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString HashRate(double hashes_per_second)
{
    static const char* const UNITS[]{"H/s", "kH/s", "MH/s", "GH/s", "TH/s", "PH/s", "EH/s"};
    size_t unit{0};
    while (hashes_per_second >= 1000 && unit + 1 < std::size(UNITS)) {
        hashes_per_second /= 1000;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(hashes_per_second, 0, 'f', unit == 0 ? 0 : 2).arg(UNITS[unit]);
}

} // namespace NodeRpc

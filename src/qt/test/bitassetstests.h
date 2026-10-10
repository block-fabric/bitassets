// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_BITASSETSTESTS_H
#define BITCOIN_QT_TEST_BITASSETSTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class BitAssetsTests : public QObject
{
public:
    explicit BitAssetsTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void bitAssetsTests();
};

#endif // BITCOIN_QT_TEST_BITASSETSTESTS_H

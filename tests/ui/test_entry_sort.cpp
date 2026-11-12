// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/EntrySort.h"

#include <QTest>

using namespace eu;
using namespace eu::ui;

class TestEntrySort : public QObject
{
    Q_OBJECT

private slots:
    void suffixes()
    {
        QCOMPARE(suffixOf(QStringLiteral("foto.JPG")), QStringLiteral("jpg"));
        QCOMPARE(suffixOf(QStringLiteral("archivo.tar.gz")), QStringLiteral("gz"));
        QCOMPARE(suffixOf(QStringLiteral(".bashrc")), QString());
        QCOMPARE(suffixOf(QStringLiteral("sin_extension")), QString());
        QCOMPARE(suffixOf(QStringLiteral("termina.")), QString());
    }

    void modifiedOrderWithNameAsTieBreak()
    {
        std::vector<fs::Entry> entries(3);
        entries[0].name = QStringLiteral("b"); entries[0].modifiedMs = 100;
        entries[1].name = QStringLiteral("a"); entries[1].modifiedMs = 100;
        entries[2].name = QStringLiteral("c"); entries[2].modifiedMs = 50;
        sortEntries(entries, Column::Modified, Qt::AscendingOrder);
        QCOMPARE(entries[0].name, QStringLiteral("c"));
        QCOMPARE(entries[1].name, QStringLiteral("a"));
        QCOMPARE(entries[2].name, QStringLiteral("b"));
    }

    void foldersStayFirstWhenDescending()
    {
        std::vector<fs::Entry> entries(3);
        entries[0].name = QStringLiteral("z.txt"); entries[0].size = 9;
        entries[1].name = QStringLiteral("carpeta"); entries[1].isDirectory = true;
        entries[2].name = QStringLiteral("a.txt"); entries[2].size = 1;
        sortEntries(entries, Column::Size, Qt::DescendingOrder);
        QCOMPARE(entries[0].name, QStringLiteral("carpeta"));
        QCOMPARE(entries[1].name, QStringLiteral("z.txt"));
    }
};

QTEST_APPLESS_MAIN(TestEntrySort)
#include "test_entry_sort.moc"

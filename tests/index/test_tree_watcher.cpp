// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/TreeWatcher.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace eu::index;

namespace {

void writeFile(const QString &path, const QByteArray &data = "x")
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(data);
}

/// Espera a que llegue un aviso de esa carpeta.
bool waitForFolder(QSignalSpy &spy, const QString &folder)
{
    return QTest::qWaitFor([&] {
        for (const QList<QVariant> &arguments : spy) {
            if (arguments.first().toString() == folder) {
                return true;
            }
        }
        return false;
    }, 15000);
}

} // namespace

class TestTreeWatcher : public QObject
{
    Q_OBJECT

private slots:
    void reportsTheFolderThatChanged()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("a/b")));
        QVERIFY(QDir(root).mkpath(QStringLiteral("c")));

        const std::unique_ptr<TreeWatcher> watcher = TreeWatcher::create(root);
        if (!watcher->isActive()) {
            QSKIP("Este sistema no tiene vigilancia de árboles.");
        }
        QCOMPARE(watcher->root(), root);
        // Donde se vigila carpeta a carpeta (Linux) hay que pedirlas; en los
        // demás sistemas no hace nada.
        for (const QString &folder : {root, root + QStringLiteral("/a"), root + QStringLiteral("/a/b"),
                                      root + QStringLiteral("/c")}) {
            watcher->addFolder(folder);
        }
        QVERIFY(!watcher->isLimited());
        QSignalSpy changes(watcher.get(), &TreeWatcher::folderChanged);
        QTest::qWait(500);   // FSEvents tarda un instante en empezar a avisar

        // Un archivo nuevo, en lo hondo.
        writeFile(root + QStringLiteral("/a/b/nuevo.txt"));
        QVERIFY(waitForFolder(changes, root + QStringLiteral("/a/b")));

        // Una carpeta nueva.
        changes.clear();
        QVERIFY(QDir(root).mkpath(QStringLiteral("c/recien")));
        QVERIFY(waitForFolder(changes, root + QStringLiteral("/c")));

        // Lo que se crea dentro de la nueva avisa en cuanto se pide vigilarla.
        watcher->addFolder(root + QStringLiteral("/c/recien"));
        changes.clear();
        writeFile(root + QStringLiteral("/c/recien/dentro.txt"));
        QVERIFY(waitForFolder(changes, root + QStringLiteral("/c/recien")));

        // Renombrar y borrar.
        changes.clear();
        QVERIFY(QFile::rename(root + QStringLiteral("/a/b/nuevo.txt"), root + QStringLiteral("/a/b/otro.txt")));
        QVERIFY(waitForFolder(changes, root + QStringLiteral("/a/b")));
        changes.clear();
        QVERIFY(QFile::remove(root + QStringLiteral("/a/b/otro.txt")));
        QVERIFY(waitForFolder(changes, root + QStringLiteral("/a/b")));

        // Un archivo que crece.
        writeFile(root + QStringLiteral("/crece.txt"));
        QVERIFY(waitForFolder(changes, root));
        changes.clear();
        writeFile(root + QStringLiteral("/crece.txt"), QByteArray(5000, 'y'));
        QVERIFY(waitForFolder(changes, root));
    }

    void missingRootIsNotFatal()
    {
        const std::unique_ptr<TreeWatcher> watcher = TreeWatcher::create(QStringLiteral("/no/existe/esta/carpeta"));
        watcher->addFolder(QStringLiteral("/no/existe/esta/carpeta"));
        QVERIFY(!watcher->isLimited());
    }
};

QTEST_GUILESS_MAIN(TestTreeWatcher)
#include "test_tree_watcher.moc"

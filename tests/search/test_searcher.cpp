// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/search/Searcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace eu;
using namespace eu::search;

namespace {

void writeFile(const QString &path, qsizetype bytes = 0)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QByteArray(bytes, 'x'));
}

/// En Unix un nombre que empieza por punto ya es oculto; en Windows hay que
/// ponerle el atributo.
void markHidden(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    QVERIFY2(SetFileAttributesW(native.c_str(), GetFileAttributesW(native.c_str()) | FILE_ATTRIBUTE_HIDDEN),
             qPrintable(path));
#else
    Q_UNUSED(path);
#endif
}

Query parsed(const QString &text)
{
    const auto query = Query::parse(text);
    return query.ok() ? query.value() : Query();
}

struct Collected
{
    QStringList paths;
    SearchSummary summary;
    int deliveries = 0;
};

Collected run(const QStringList &roots, const QString &text, const SearchOptions &options = {},
              const tasks::CancelToken &cancel = {}, const PauseGate *pause = nullptr)
{
    Collected collected;
    collected.summary = searchTree(roots, parsed(text), options, cancel, pause,
                                   [&](std::vector<Hit> &&hits, const SearchProgress &) {
        ++collected.deliveries;
        for (const Hit &hit : hits) {
            collected.paths.append(hit.path());
        }
    });
    collected.paths.sort();
    return collected;
}

} // namespace

class TestSearcher : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tree;
    QString m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tree.isValid());
        m_root = QDir::cleanPath(m_tree.path());
        const QDir root(m_root);
        QVERIFY(root.mkpath(QStringLiteral("fotos/2025/verano")));
        QVERIFY(root.mkpath(QStringLiteral("documentos/facturas")));
        QVERIFY(root.mkpath(QStringLiteral(".oculta/dentro")));
        writeFile(root.filePath(QStringLiteral("informe.pdf")), 10);
        writeFile(root.filePath(QStringLiteral("fotos/playa.jpg")), 3000);
        writeFile(root.filePath(QStringLiteral("fotos/2025/verano/playa 2.JPG")), 5000);
        writeFile(root.filePath(QStringLiteral("fotos/2025/verano/notas.txt")), 1);
        writeFile(root.filePath(QStringLiteral("documentos/facturas/factura playa.pdf")), 2000000);
        writeFile(root.filePath(QStringLiteral(".oculta/dentro/playa secreta.jpg")), 1);
        writeFile(root.filePath(QStringLiteral("documentos/.playa-oculta.txt")), 1);
        markHidden(root.filePath(QStringLiteral(".oculta")));
        markHidden(root.filePath(QStringLiteral("documentos/.playa-oculta.txt")));
    }

    void findsInSubfolders()
    {
        const Collected found = run({m_root}, QStringLiteral("playa"));
        QCOMPARE(found.paths, (QStringList{
            m_root + QStringLiteral("/documentos/facturas/factura playa.pdf"),
            m_root + QStringLiteral("/fotos/2025/verano/playa 2.JPG"),
            m_root + QStringLiteral("/fotos/playa.jpg"),
        }));
        QVERIFY(!found.summary.truncated);
        QVERIFY(!found.summary.cancelled);
        QCOMPARE(found.summary.progress.hits, 3);
        // La raíz, fotos, 2025, verano, documentos, facturas (la oculta no).
        QCOMPARE(found.summary.progress.foldersScanned, 6);
        QCOMPARE(found.summary.progress.unreadableFolders, 0);
        QVERIFY(found.summary.progress.itemsExamined >= 10);
    }

    void filtersAndFoldersAreMatchedToo()
    {
        QCOMPARE(run({m_root}, QStringLiteral("ext:jpg")).paths.size(), 2);
        QCOMPARE(run({m_root}, QStringLiteral("playa size:>1MB")).paths,
                 QStringList{m_root + QStringLiteral("/documentos/facturas/factura playa.pdf")});
        QCOMPARE(run({m_root}, QStringLiteral("verano")).paths,
                 QStringList{m_root + QStringLiteral("/fotos/2025/verano")});
        QCOMPARE(run({m_root}, QStringLiteral("type:folder")).paths.size(), 5);
    }

    void hiddenOnlyWhenAsked()
    {
        SearchOptions options;
        options.includeHidden = true;
        const Collected found = run({m_root}, QStringLiteral("playa"), options);
        QCOMPARE(found.paths.size(), 5);
        QVERIFY(found.paths.contains(m_root + QStringLiteral("/.oculta/dentro/playa secreta.jpg")));
        QVERIFY(found.paths.contains(m_root + QStringLiteral("/documentos/.playa-oculta.txt")));
    }

    void severalRoots()
    {
        const Collected found = run({m_root + QStringLiteral("/fotos/2025"), m_root + QStringLiteral("/documentos")},
                                    QStringLiteral("playa"));
        QCOMPARE(found.paths.size(), 2);
    }

    void missingRootIsCountedNotFatal()
    {
        const Collected found = run({m_root + QStringLiteral("/no-existe"), m_root}, QStringLiteral("informe"));
        QCOMPARE(found.paths, QStringList{m_root + QStringLiteral("/informe.pdf")});
        QCOMPARE(found.summary.progress.unreadableFolders, 1);
    }

    void symlinkLoopsAreNotFollowed()
    {
#ifdef Q_OS_WIN
        QSKIP("Crear enlaces simbólicos en Windows requiere permisos especiales.");
#else
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("a/b")));
        writeFile(root + QStringLiteral("/a/b/dato.txt"));
        // Un enlace que apunta hacia arriba: seguirlo sería un ciclo infinito.
        QVERIFY(QFile::link(root, root + QStringLiteral("/a/b/vuelta")));
        const Collected found = run({root}, QStringLiteral("dato"));
        QCOMPARE(found.paths, QStringList{root + QStringLiteral("/a/b/dato.txt")});
        QCOMPARE(found.summary.progress.foldersScanned, 3);
        // El enlace en sí se puede encontrar por su nombre.
        QCOMPARE(run({root}, QStringLiteral("vuelta")).paths.size(), 1);
#endif
    }

    void onlyTheFolderItselfWhenNotRecursive()
    {
        SearchOptions options;
        options.recursive = false;
        const Collected found = run({m_root}, QStringLiteral("playa OR informe"), options);
        QCOMPARE(found.paths, QStringList{m_root + QStringLiteral("/informe.pdf")});
        QCOMPARE(found.summary.progress.foldersScanned, 1);
    }

    void findsTextInsideFiles()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("notas/viejas")));
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
            file.write(data);
        };
        write(root + QStringLiteral("/notas/lista.txt"), "Comprar pan\nLlamar a Walter Riso\n");
        write(root + QStringLiteral("/notas/viejas/carta.txt"), QByteArray("Se\xf1or Riso: gracias"));   // Latin-1
        const QString utf16 = QStringLiteral("apunte sobre riso");
        write(root + QStringLiteral("/notas/unicode.txt"),
              QByteArray("\xff\xfe") + QByteArray(reinterpret_cast<const char *>(utf16.utf16()), utf16.size() * 2));
        write(root + QStringLiteral("/notas/otra.txt"), "nada que ver");
        write(root + QStringLiteral("/binario.dat"), QByteArray("riso\0riso", 9));
        write(root + QStringLiteral("/riso.txt"), "el nombre lo dice, el contenido no");
        write(root + QStringLiteral("/grande.txt"), QByteArray(3000, 'a') + " riso");

        SearchOptions options;
        options.maxContentBytes = 2000;
        const Collected found = run({root}, QStringLiteral("content:riso"), options);
        QStringList expected{root + QStringLiteral("/notas/lista.txt"), root + QStringLiteral("/notas/unicode.txt"),
                             root + QStringLiteral("/notas/viejas/carta.txt")};
        expected.sort();
        QCOMPARE(found.paths, expected);
        QVERIFY(found.summary.progress.filesRead >= 5);

        // Con la extensión por delante solo se abren los que la cumplen.
        const Collected narrowed = run({root}, QStringLiteral("content:riso ext:dat"), options);
        QVERIFY(narrowed.paths.isEmpty());
        QCOMPARE(narrowed.summary.progress.filesRead, 1);

        QCOMPARE(run({root}, QStringLiteral("content:\"Walter Riso\""), options).paths,
                 QStringList{root + QStringLiteral("/notas/lista.txt")});
    }

    void stopsAtTheLimit()
    {
        SearchOptions options;
        options.maxHits = 2;
        const Collected found = run({m_root}, QStringLiteral("*"), options);
        QVERIFY(found.summary.truncated);
        QCOMPARE(found.paths.size(), 2);
    }

    void cancelAndPauseRespondQuickly()
    {
        // Un árbol con bastantes carpetas para que la búsqueda dure algo.
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        for (int i = 0; i < 300; ++i) {
            QVERIFY(QDir(root).mkpath(QStringLiteral("c%1/d").arg(i)));
            writeFile(root + QStringLiteral("/c%1/d/archivo.txt").arg(i));
        }

        // Ya cancelada: no entrega resultados.
        tasks::CancelSource cancelled;
        cancelled.cancel();
        const Collected nothing = run({root}, QStringLiteral("archivo"), {}, cancelled.token());
        QVERIFY(nothing.summary.cancelled);
        QVERIFY(nothing.paths.isEmpty());

        // En pausa no avanza; al reanudar termina; cancelar en pausa la suelta.
        PauseGate gate;
        gate.setPaused(true);
        tasks::CancelSource source;
        std::atomic<qint64> hits{0};
        std::atomic<bool> finished{false};
        SearchSummary summary;
        std::thread worker([&] {
            summary = searchTree({root}, parsed(QStringLiteral("archivo")), {}, source.token(), &gate,
                                 [&](std::vector<Hit> &&found, const SearchProgress &) { hits += qint64(found.size()); });
            finished = true;
        });
        QTest::qWait(150);
        QCOMPARE(hits.load(), 0);
        QVERIFY(!finished);

        QElapsedTimer timer;
        timer.start();
        source.cancel();
        worker.join();
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(summary.cancelled);

        // Reanudar una búsqueda en pausa la deja terminar.
        PauseGate second;
        second.setPaused(true);
        std::atomic<qint64> secondHits{0};
        std::thread resumed([&] {
            searchTree({root}, parsed(QStringLiteral("archivo")), {}, tasks::CancelToken(), &second,
                       [&](std::vector<Hit> &&found, const SearchProgress &) { secondHits += qint64(found.size()); });
        });
        QTest::qWait(100);
        QCOMPARE(secondHits.load(), 0);
        second.setPaused(false);
        resumed.join();
        QCOMPARE(secondHits.load(), 300);
    }
};

QTEST_GUILESS_MAIN(TestSearcher)
#include "test_searcher.moc"

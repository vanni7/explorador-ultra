// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/Index.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace eu;
using namespace eu::index;

namespace {

void writeFile(const QString &path, qsizetype bytes = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
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

search::Query parsed(const QString &text)
{
    const auto query = search::Query::parse(text);
    return query.ok() ? query.value() : search::Query();
}

QStringList fromIndex(const Index &index, const QStringList &roots, const QString &text,
                      const search::SearchOptions &options = {})
{
    QStringList paths;
    index.search(roots, parsed(text), options, {}, [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            paths.append(hit.path());
        }
    });
    paths.sort();
    return paths;
}

QStringList fromDisk(const QStringList &roots, const QString &text, const search::SearchOptions &options = {})
{
    QStringList paths;
    search::searchTree(roots, parsed(text), options, {}, nullptr,
                       [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            paths.append(hit.path());
        }
    });
    paths.sort();
    return paths;
}

} // namespace

class TestIndex : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tree;
    QTemporaryDir m_data;
    QString m_root;

    QString databasePath(const QString &name) const { return m_data.filePath(name + QStringLiteral(".db")); }

    Index openIndex(const QString &name, const QString &root)
    {
        auto opened = Index::open(databasePath(name), root);
        if (!opened.ok()) {
            qFatal("No se pudo abrir el índice: %s", qPrintable(opened.error().message));
        }
        return opened.value();
    }

private slots:
    void initTestCase()
    {
        m_root = QDir::cleanPath(m_tree.path()) + QStringLiteral("/raiz");
        writeFile(m_root + QStringLiteral("/informe.pdf"), 10);
        writeFile(m_root + QStringLiteral("/fotos/playa.jpg"), 3000);
        writeFile(m_root + QStringLiteral("/fotos/2025/verano/playa 2.JPG"), 5000);
        writeFile(m_root + QStringLiteral("/fotos/2025/verano/notas.txt"), 1);
        writeFile(m_root + QStringLiteral("/documentos/facturas/factura playa.pdf"), 2000000);
        writeFile(m_root + QStringLiteral("/documentos/100%_real.txt"), 5);
        writeFile(m_root + QStringLiteral("/.oculta/dentro/playa secreta.jpg"), 1);
        writeFile(m_root + QStringLiteral("/documentos/.playa-oculta.txt"), 1);
        markHidden(m_root + QStringLiteral("/.oculta"));
        markHidden(m_root + QStringLiteral("/documentos/.playa-oculta.txt"));
    }

    void emptyIndexCannotAnswer()
    {
        const Index index = openIndex(QStringLiteral("vacio"), m_root);
        QVERIFY(!index.info().complete);
        QVERIFY(!index.canAnswer({m_root}, parsed(QStringLiteral("playa")), {}));
    }

    void answersLikeTheDisk()
    {
        const Index index = openIndex(QStringLiteral("igual"), m_root);
        QStringList visited;
        SyncOptions options;
        options.onFolder = [&](const QString &folder) { visited.append(folder); };
        const auto synced = index.sync(m_root, options, {});
        QVERIFY2(synced.ok(), synced.ok() ? "" : qPrintable(synced.error().message));
        QCOMPARE(synced.value().foldersScanned, 8);
        QCOMPARE(synced.value().added, 15);
        QCOMPARE(visited.size(), 8);
        QVERIFY(visited.contains(m_root + QStringLiteral("/fotos/2025/verano")));

        const IndexInfo info = index.info();
        QVERIFY(info.complete);
        QCOMPARE(info.entries, 15);
        QCOMPARE(info.folders, 8);
        QVERIFY(info.updatedMs > 0);

        search::SearchOptions hidden;
        hidden.includeHidden = true;
        const QStringList queries{QStringLiteral("playa"), QStringLiteral("PLAYA ext:jpg"), QStringLiteral("*.pdf"),
                                  QStringLiteral("size:>1MB"), QStringLiteral("type:carpeta"),
                                  QStringLiteral("playa OR notas"), QStringLiteral("100%_"), QStringLiteral("-playa"),
                                  QStringLiteral("regex:/^fact/"), QString()};
        const QStringList roots{m_root, m_root + QStringLiteral("/fotos"), m_root + QStringLiteral("/fotos/2025")};
        for (const QString &text : queries) {
            for (const QString &root : roots) {
                QVERIFY(index.canAnswer({root}, parsed(text), {}));
                QCOMPARE(fromIndex(index, {root}, text), fromDisk({root}, text));
                QCOMPARE(fromIndex(index, {root}, text, hidden), fromDisk({root}, text, hidden));
            }
        }
        // De verdad encuentra algo, y los ocultos solo cuando se piden.
        QCOMPARE(fromIndex(index, {m_root}, QStringLiteral("playa")).size(), 3);
        QCOMPARE(fromIndex(index, {m_root}, QStringLiteral("playa"), hidden).size(), 5);
        // Dos carpetas de partida.
        QCOMPARE(fromIndex(index, {m_root + QStringLiteral("/fotos"), m_root + QStringLiteral("/documentos")},
                           QStringLiteral("playa")),
                 fromDisk({m_root + QStringLiteral("/fotos"), m_root + QStringLiteral("/documentos")},
                          QStringLiteral("playa")));
    }

    void knowsWhatItCannotAnswer()
    {
        const Index index = openIndex(QStringLiteral("limites"), m_root);
        QVERIFY(index.sync(m_root, {}, {}).ok());
        const search::Query name = parsed(QStringLiteral("playa"));
        QVERIFY(index.canAnswer({m_root}, name, {}));
        // El contenido de los archivos no está en el índice.
        QVERIFY(!index.canAnswer({m_root}, parsed(QStringLiteral("content:playa")), {}));
        // Una carpeta de fuera.
        QVERIFY(!index.canAnswer({QDir::cleanPath(m_tree.path())}, name, {}));
        QVERIFY(!index.canAnswer({m_root + QStringLiteral("x")}, name, {}));
        // Sin subcarpetas es más rápido leer la carpeta.
        search::SearchOptions flat;
        flat.recursive = false;
        QVERIFY(!index.canAnswer({m_root}, name, flat));
        // Dentro de una carpeta oculta, sin "mostrar ocultos", el disco sí enseña su contenido.
        QVERIFY(!index.canAnswer({m_root + QStringLiteral("/.oculta")}, name, {}));
        search::SearchOptions hidden;
        hidden.includeHidden = true;
        QVERIFY(index.canAnswer({m_root + QStringLiteral("/.oculta")}, name, hidden));
        // Una carpeta que el índice aún no conoce.
        QVERIFY(!index.canAnswer({m_root + QStringLiteral("/no-existe")}, name, {}));
    }

    void followsChangesFolderByFolder()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        writeFile(root + QStringLiteral("/a/uno.txt"), 1);
        writeFile(root + QStringLiteral("/a/b/dos.txt"), 2);
        writeFile(root + QStringLiteral("/a/b/c/tres.txt"), 3);
        writeFile(root + QStringLiteral("/z/otro.txt"), 1);
        const Index index = openIndex(QStringLiteral("cambios"), root);
        QVERIFY(index.sync(root, {}, {}).ok());
        SyncOptions oneFolder;
        oneFolder.wholeTree = false;
        const auto all = [&] { return fromIndex(index, {root}, QString()); };

        // Un archivo nuevo: solo se compara su carpeta.
        writeFile(root + QStringLiteral("/a/b/nuevo.txt"), 4);
        auto synced = index.sync(root + QStringLiteral("/a/b"), oneFolder, {});
        QVERIFY(synced.ok());
        QCOMPARE(synced.value().foldersScanned, 1);
        QCOMPARE(synced.value().added, 1);
        QCOMPARE(all(), fromDisk({root}, QString()));

        // Cambia de tamaño.
        writeFile(root + QStringLiteral("/a/uno.txt"), 500);
        synced = index.sync(root + QStringLiteral("/a"), oneFolder, {});
        QVERIFY(synced.ok());
        QVERIFY(synced.value().updated >= 1);   // el archivo, y quizá la fecha de la subcarpeta "b"
        QCOMPARE(fromIndex(index, {root}, QStringLiteral("size:500")), QStringList{root + QStringLiteral("/a/uno.txt")});

        // Una carpeta nueva con cosas dentro: se baja a ella aunque solo se avise de la de arriba.
        writeFile(root + QStringLiteral("/a/nueva/dentro/hondo.txt"), 1);
        QStringList visited;
        SyncOptions watching = oneFolder;
        watching.onFolder = [&](const QString &folder) { visited.append(folder); };
        synced = index.sync(root + QStringLiteral("/a"), watching, {});
        QVERIFY(synced.ok());
        QCOMPARE(synced.value().foldersScanned, 3);
        QVERIFY(visited.contains(root + QStringLiteral("/a/nueva/dentro")));
        QCOMPARE(all(), fromDisk({root}, QString()));

        // El aviso llega por una carpeta que el índice aún no conoce.
        writeFile(root + QStringLiteral("/z/x/y/perdido.txt"), 1);
        QVERIFY(index.sync(root + QStringLiteral("/z/x/y"), oneFolder, {}).ok());
        QCOMPARE(all(), fromDisk({root}, QString()));

        // Se borra una carpeta entera: desaparece con todo lo que colgaba.
        QVERIFY(QDir(root + QStringLiteral("/a/b")).removeRecursively());
        QVERIFY(index.sync(root + QStringLiteral("/a"), oneFolder, {}).ok());
        QCOMPARE(all(), fromDisk({root}, QString()));
        QVERIFY(fromIndex(index, {root}, QStringLiteral("tres")).isEmpty());
        // Y si el aviso llega por la propia carpeta borrada.
        QVERIFY(QDir(root + QStringLiteral("/z/x")).removeRecursively());
        QVERIFY(index.sync(root + QStringLiteral("/z/x"), oneFolder, {}).ok());
        QCOMPARE(all(), fromDisk({root}, QString()));

        // Una carpeta pasa a ser un archivo con el mismo nombre.
        QVERIFY(QDir(root + QStringLiteral("/a/nueva")).removeRecursively());
        writeFile(root + QStringLiteral("/a/nueva"), 7);
        QVERIFY(index.sync(root + QStringLiteral("/a"), oneFolder, {}).ok());
        QCOMPARE(all(), fromDisk({root}, QString()));

        // Un recorrido completo deja lo mismo y no cambia nada.
        synced = index.sync(root, {}, {});
        QVERIFY(synced.ok());
        QCOMPARE(synced.value().added + synced.value().removed, 0);
        QCOMPARE(index.info().entries, qint64(all().size()));
    }

    void excludedNamesAreLeftOut()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        writeFile(root + QStringLiteral("/papelera/dentro/borrado.txt"), 1);
        writeFile(root + QStringLiteral("/sub/papelera/otro.txt"), 1);
        writeFile(root + QStringLiteral("/sub/queda.txt"), 1);
        const Index index = openIndex(QStringLiteral("excluidos"), root);
        SyncOptions options;
        options.excludedNames = QStringList{QStringLiteral("papelera")};
        QVERIFY(index.sync(root, options, {}).ok());
        QCOMPARE(fromIndex(index, {root}, QString()),
                 (QStringList{root + QStringLiteral("/sub"), root + QStringLiteral("/sub/queda.txt")}));
    }

    void symlinkedFoldersAreNotFollowed()
    {
#ifdef Q_OS_WIN
        QSKIP("Crear enlaces simbólicos en Windows requiere permisos especiales.");
#else
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        writeFile(root + QStringLiteral("/a/dato.txt"), 1);
        QVERIFY(QFile::link(root, root + QStringLiteral("/a/vuelta")));
        const Index index = openIndex(QStringLiteral("enlaces"), root);
        const auto synced = index.sync(root, {}, {});
        QVERIFY(synced.ok());
        QCOMPARE(synced.value().foldersScanned, 2);
        QCOMPARE(fromIndex(index, {root}, QString()), fromDisk({root}, QString()));
#endif
    }

    void cancelledSyncIsNotComplete()
    {
        const Index index = openIndex(QStringLiteral("cancelado"), m_root);
        tasks::CancelSource source;
        source.cancel();
        const auto synced = index.sync(m_root, {}, source.token());
        QVERIFY(!synced.ok());
        QCOMPARE(synced.error().code, core::ErrorCode::Cancelled);
        QVERIFY(!index.info().complete);
    }

    void aFileOfAnotherRootStartsEmpty()
    {
        const QString path = databasePath(QStringLiteral("reutilizado"));
        {
            const auto first = Index::open(path, m_root);
            QVERIFY(first.ok());
            QVERIFY(first.value().sync(m_root, {}, {}).ok());
            QVERIFY(first.value().info().complete);
        }
        // Abrirlo otra vez para la misma carpeta lo conserva.
        const auto again = Index::open(path, m_root);
        QVERIFY(again.ok());
        QVERIFY(again.value().info().complete);
        // Para otra carpeta, empieza de cero.
        const auto other = Index::open(path, m_root + QStringLiteral("/fotos"));
        QVERIFY(other.ok());
        QVERIFY(!other.value().info().complete);
        QCOMPARE(other.value().info().entries, 0);
        Index::remove(path);
        QVERIFY(!QFile::exists(path));
    }
};

QTEST_GUILESS_MAIN(TestIndex)
#include "test_index.moc"

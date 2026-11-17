// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tags/TagStore.h"

#include "eu/ops/Engine.h"
#include "eu/search/Query.h"
#include "eu/search/Searcher.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::tags;

namespace {

void touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QStringList names(const QList<Tag> &tags)
{
    QStringList out;
    for (const Tag &tag : tags) {
        out.append(tag.name);
    }
    return out;
}

} // namespace

class TestTags : public QObject
{
    Q_OBJECT

private slots:
    void createAssignAndSearch()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/factura.pdf"));
        touch(root + QStringLiteral("/sub/contrato.pdf"));
        touch(root + QStringLiteral("/foto.jpg"));
        auto store = TagStore::open(data.filePath(QStringLiteral("etiquetas.db"))).value();
        const Tag urgent = store->createTag(QStringLiteral("Urgente"), QStringLiteral("#e74c3c")).value();
        const Tag work = store->createTag(QStringLiteral("Trabajo"), QStringLiteral("#3498db")).value();
        QVERIFY(!store->createTag(QStringLiteral("urgente"), QStringLiteral("#000000")).ok());   // ya existe
        QVERIFY(!store->createTag(QStringLiteral("  "), QStringLiteral("#000000")).ok());
        QVERIFY(store->assign({root + QStringLiteral("/factura.pdf"), root + QStringLiteral("/sub/contrato.pdf")},
                              urgent.id).ok());
        QVERIFY(store->assign({root + QStringLiteral("/sub/contrato.pdf")}, work.id).ok());
        QCOMPARE(names(store->tags()), (QStringList{QStringLiteral("Trabajo"), QStringLiteral("Urgente")}));
        const auto tagged = store->tagsFor({root + QStringLiteral("/sub/contrato.pdf"), root + QStringLiteral("/foto.jpg")});
        QCOMPARE(names(tagged.value(root + QStringLiteral("/sub/contrato.pdf"))),
                 (QStringList{QStringLiteral("Trabajo"), QStringLiteral("Urgente")}));
        QVERIFY(!tagged.contains(root + QStringLiteral("/foto.jpg")));

        // Buscar con tag:, que también combina con lo demás.
        const auto find = [&](const QString &text) {
            QStringList found;
            search::SearchOptions options;
            options.pathsWithTag = [&](const QString &tag) { return store->pathsWithTag(tag); };
            search::searchTree({root}, search::Query::parse(text).value(), options, {}, nullptr,
                               [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
                for (const search::Hit &hit : hits) {
                    found.append(QDir(root).relativeFilePath(hit.path()));
                }
            });
            found.sort();
            return found;
        };
        QCOMPARE(find(QStringLiteral("tag:urgente")),
                 (QStringList{QStringLiteral("factura.pdf"), QStringLiteral("sub/contrato.pdf")}));
        QCOMPARE(find(QStringLiteral("tag:urgente tag:trabajo")), QStringList{QStringLiteral("sub/contrato.pdf")});
        QCOMPARE(find(QStringLiteral("tag:urgente NOT contrato*")), QStringList{QStringLiteral("factura.pdf")});
        QVERIFY(search::Query::parse(QStringLiteral("tag:x")).value().unavailableFilters().isEmpty());

        // Quitar, renombrar y borrar etiquetas.
        QVERIFY(store->unassign({root + QStringLiteral("/factura.pdf")}, urgent.id).ok());
        QCOMPARE(store->pathsWithTag(QStringLiteral("URGENTE")), QStringList{root + QStringLiteral("/sub/contrato.pdf")});
        QVERIFY(store->renameTag(work.id, QStringLiteral("Oficina")).ok());
        QCOMPARE(store->pathsWithTag(QStringLiteral("oficina")).size(), 1);
        QVERIFY(store->deleteTag(work.id).ok());
        QVERIFY(store->pathsWithTag(QStringLiteral("oficina")).isEmpty());
    }

    void tagsFollowMovesAndRenames()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/carpeta/dentro.txt"));
        touch(root + QStringLiteral("/suelto.txt"));
        auto journal = journal::Journal::open(data.filePath(QStringLiteral("h.db"))).value();
        auto store = TagStore::open(data.filePath(QStringLiteral("etiquetas.db"))).value();
        const Tag tag = store->createTag(QStringLiteral("Azul"), QStringLiteral("#3498db")).value();
        QVERIFY(store->assign({root + QStringLiteral("/carpeta/dentro.txt"), root + QStringLiteral("/suelto.txt")}, tag.id).ok());

        // Desde la aplicación: el historial dice qué pasó.
        ops::Engine engine(journal);
        const ops::Outcome renamed = engine.rename(root + QStringLiteral("/carpeta"), QStringLiteral("Proyecto"));
        QVERIFY(renamed.ok());
        store->followSteps(journal->steps(renamed.operationId).value());
        QDir().mkpath(root + QStringLiteral("/destino"));
        const ops::Outcome moved = engine.move({root + QStringLiteral("/suelto.txt")}, root + QStringLiteral("/destino"),
                                               ops::CollisionPolicy::Skip, {});
        QVERIFY(moved.ok());
        store->followSteps(journal->steps(moved.operationId).value());
        QStringList paths = store->pathsWithTag(QStringLiteral("azul"));
        paths.sort();
        QCOMPARE(paths, (QStringList{root + QStringLiteral("/Proyecto/dentro.txt"), root + QStringLiteral("/destino/suelto.txt")}));

        // Desde fuera (mismo volumen): al verlo, se reconoce por su identidad.
        QVERIFY(QFile::rename(root + QStringLiteral("/destino/suelto.txt"), root + QStringLiteral("/otro nombre.txt")));
        const auto seen = store->tagsFor({root + QStringLiteral("/otro nombre.txt")});
        QCOMPARE(names(seen.value(root + QStringLiteral("/otro nombre.txt"))), QStringList{QStringLiteral("Azul")});
        QVERIFY(store->pathsWithTag(QStringLiteral("azul")).contains(root + QStringLiteral("/otro nombre.txt")));
    }

    void orphansAreNeverGuessed()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/informe.pdf"), "contenido del informe");
        touch(root + QStringLiteral("/borrado.txt"), "se va a borrar");
        auto store = TagStore::open(data.filePath(QStringLiteral("etiquetas.db"))).value();
        const Tag tag = store->createTag(QStringLiteral("Rojo"), QStringLiteral("#e74c3c")).value();
        QVERIFY(store->assign({root + QStringLiteral("/informe.pdf"), root + QStringLiteral("/borrado.txt")}, tag.id).ok());

        // Movido por fuera a una subcarpeta y otro borrado: al repasar, los dos huérfanos.
        QDir().mkpath(root + QStringLiteral("/archivo"));
        QVERIFY(QFile::rename(root + QStringLiteral("/informe.pdf"), root + QStringLiteral("/archivo/informe.pdf")));
        QVERIFY(QFile::remove(root + QStringLiteral("/borrado.txt")));
        QList<Orphan> lost = store->review();
        QCOMPARE(lost.size(), 2);
        QVERIFY(store->pathsWithTag(QStringLiteral("rojo")).isEmpty());   // una huérfana no cuenta

        // Buscarlo de nuevo: un único archivo con su identidad -> se reasigna.
        for (const Orphan &orphan : lost) {
            const core::Result<bool> found = store->relocate(orphan.markId, {root}, {});
            QVERIFY(found.ok());
            QCOMPARE(found.value(), orphan.path.endsWith(QStringLiteral("informe.pdf")));
        }
        QCOMPARE(store->pathsWithTag(QStringLiteral("rojo")), QStringList{root + QStringLiteral("/archivo/informe.pdf")});
        QCOMPARE(store->orphans().size(), 1);   // el borrado sigue huérfano: no hay a quién dárselo

        // Si hay dos candidatos igual de buenos, no se elige ninguno.
        touch(root + QStringLiteral("/a/gemelo.txt"), "igual");
        auto saved = store->assign({root + QStringLiteral("/a/gemelo.txt")}, tag.id);
        QVERIFY(saved.ok());
        QVERIFY(QFile::remove(root + QStringLiteral("/a/gemelo.txt")));
        QVERIFY(store->review().size() == 2);
        touch(root + QStringLiteral("/b/gemelo.txt"), "igual");
        touch(root + QStringLiteral("/c/gemelo.txt"), "igual");
        for (const Orphan &orphan : store->orphans()) {
            if (!orphan.path.endsWith(QStringLiteral("gemelo.txt"))) {
                continue;
            }
            // Los dos con el mismo nombre, tamaño y fecha que tenía: igual de buenos.
            for (const QString &twin : {QStringLiteral("/b/gemelo.txt"), QStringLiteral("/c/gemelo.txt")}) {
                QFile file(root + twin);
                QVERIFY(file.open(QIODevice::ReadWrite));
                QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(orphan.modifiedMs), QFileDevice::FileModificationTime));
            }
            QCOMPARE(store->relocate(orphan.markId, {root}, {}).value(), false);
            QVERIFY(store->removeMark(orphan.markId).ok());
        }
        QCOMPARE(store->orphans().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestTags)

#include "test_tags.moc"

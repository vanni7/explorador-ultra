// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/organize/Organize.h"

#include "eu/journal/Journal.h"
#include "eu/ops/Engine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::organize;

namespace {

void touch(const QString &path, qint64 bytes = 10, const QDateTime &modified = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(bytes, 'x'));
    file.flush();   // si no, al cerrar se escribe y la fecha vuelve a ser la de ahora
    if (modified.isValid()) {
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
    }
}

QStringList tree(const QString &root)
{
    QStringList result;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).isFile()) {
            result.append(QDir(root).relativeFilePath(path));
        }
    }
    result.sort();
    return result;
}

QStringList targets(const Plan &plan, const QString &root)
{
    QStringList out;
    for (const Move &move : plan.moves) {
        out.append(QDir(root).relativeFilePath(move.target));
    }
    out.sort();
    return out;
}

} // namespace

class TestOrganize : public QObject
{
    Q_OBJECT

private slots:
    void defaultRulesByType()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/informe.PDF"));
        touch(root + QStringLiteral("/carta.docx"));
        touch(root + QStringLiteral("/foto.jpg"));
        touch(root + QStringLiteral("/toma.CR2"));
        touch(root + QStringLiteral("/peli.mkv"));
        touch(root + QStringLiteral("/canción.flac"));
        touch(root + QStringLiteral("/raro.xyz"));
        touch(root + QStringLiteral("/sub/dentro.pdf"));
        touch(root + QStringLiteral("/guía.html"));
        Options options;
        options.rules = defaultRules();
        Plan plan = simulate(root, options, {});
        QCOMPARE(targets(plan, root),
                 (QStringList{QStringLiteral("Audio/FLAC/canción.flac"), QStringLiteral("Documentos/PDF/informe.PDF"),
                              QStringLiteral("Documentos/Web/guía.html"), QStringLiteral("Documentos/Word/carta.docx"),
                              QStringLiteral("Imágenes/JPG/foto.jpg"), QStringLiteral("Imágenes/RAW/toma.CR2"),
                              QStringLiteral("Videos/MKV/peli.mkv")}));
        QCOMPARE(plan.withoutRule, 1);   // raro.xyz se queda
        QCOMPARE(plan.inSubfolders, 1);  // sub/dentro.pdf no se mira (y se avisa)
        QVERIFY(plan.foldersToCreate.contains(root + QStringLiteral("/Documentos")));
        QVERIFY(plan.foldersToCreate.contains(root + QStringLiteral("/Documentos/PDF")));
        // Las carpetas, de fuera hacia dentro.
        QVERIFY(plan.foldersToCreate.indexOf(root + QStringLiteral("/Documentos"))
                < plan.foldersToCreate.indexOf(root + QStringLiteral("/Documentos/PDF")));

        // Con subcarpetas, también lo de dentro.
        options.recursive = true;
        plan = simulate(root, options, {});
        QVERIFY(targets(plan, root).contains(QStringLiteral("Documentos/PDF/dentro.pdf")));
        QCOMPARE(plan.inSubfolders, 0);
    }

    void datesSizesMetadataAndClashes()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/a.jpg"), 10, QDateTime(QDate(2023, 5, 2), QTime(10, 0)));
        touch(root + QStringLiteral("/b.jpg"), 10, QDateTime(QDate(2024, 12, 30), QTime(10, 0)));
        touch(root + QStringLiteral("/grande.bin"), 2 * 1000 * 1000);
        Options byDate;
        byDate.rules = {Rule{QStringLiteral("Todo"), {}, -1, -1, QStringLiteral("{fecha:yyyy}/{fecha:MM}")}};
        QCOMPARE(targets(simulate(root, byDate, {}), root),
                 (QStringList{QStringLiteral("2023/05/a.jpg"), QStringLiteral("2024/12/b.jpg"),
                              QStringLiteral("%1/%2/grande.bin").arg(QDate::currentDate().year())
                                  .arg(QDate::currentDate().month(), 2, 10, QLatin1Char('0'))}));
        Options bySize;
        bySize.rules = {Rule{QStringLiteral("Todo"), {}, -1, -1, QStringLiteral("{tamaño}")}};
        const QStringList sized = targets(simulate(root, bySize, {}), root);
        QVERIFY2(sized.contains(QStringLiteral("Medianos (1 a 100 MB)/grande.bin")), qPrintable(sized.join(QLatin1Char(','))));

        // Un dato con "/" o ":" no crea carpetas de más ni nombres no válidos; vacío, "Sin dato".
        Options byArtist;
        byArtist.rules = {Rule{QStringLiteral("Música"), {QStringLiteral("jpg")}, -1, -1, QStringLiteral("{artista}/{album}")}};
        PlanOptions metadata;
        metadata.rulesFor = [](const QString &) { return core::NameRules::Windows; };
        metadata.metadataFor = [](const QString &path) {
            return path.endsWith(QStringLiteral("a.jpg"))
                ? rename::Metadata{{QStringLiteral("artista"), QStringLiteral("AC/DC")},
                                   {QStringLiteral("album"), QStringLiteral("Live: 1992")}}
                : rename::Metadata();
        };
        QCOMPARE(targets(simulate(root, byArtist, {}, metadata), root),
                 (QStringList{QStringLiteral("AC-DC/Live_ 1992/a.jpg"), QStringLiteral("Sin dato/Sin dato/b.jpg")}));

        // Choques: ya hay un "a.jpg" en el destino, y dos que acabarían igual.
        touch(root + QStringLiteral("/2023/05/a.jpg"), 10, QDateTime(QDate(2023, 5, 20), QTime(8, 0)));
        touch(root + QStringLiteral("/otra/b.jpg"), 10, QDateTime(QDate(2024, 12, 1), QTime(9, 0)));
        byDate.recursive = true;
        const Plan clashing = simulate(root, byDate, {});
        const QStringList planned = targets(clashing, root);
        QVERIFY2(planned.contains(QStringLiteral("2024/12/b (2).jpg")) && planned.contains(QStringLiteral("2024/12/b.jpg")),
                 qPrintable(planned.join(QLatin1Char(','))));
        QCOMPARE(clashing.alreadyInPlace, 1);   // el a.jpg que ya estaba en 2023/05
        QVERIFY(planned.contains(QStringLiteral("2023/05/a (2).jpg")));
        bool noted = false;
        for (const Move &move : clashing.moves) {
            noted = noted || !move.note.isEmpty();
        }
        QVERIFY(noted);
    }

    void applyingIsOneOperationAndUndoRemovesFolders()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/informe.pdf"));
        touch(root + QStringLiteral("/foto.jpg"));
        touch(root + QStringLiteral("/Documentos/viejo.pdf"));   // una carpeta que ya existía
        const QStringList before = tree(root);
        Options options;
        options.rules = defaultRules();
        const Plan plan = simulate(root, options, {});
        QList<QPair<QString, QString>> moves;
        for (const Move &move : plan.moves) {
            moves.append({move.source, move.target});
        }
        auto journal = journal::Journal::open(data.filePath(QStringLiteral("h.db"))).value();
        ops::Engine engine(journal);
        const ops::Outcome outcome = engine.applyMoves(moves, plan.foldersToCreate, QStringLiteral("Organizar"), {});
        QVERIFY(outcome.ok());
        QCOMPARE(tree(root), (QStringList{QStringLiteral("Documentos/PDF/informe.pdf"), QStringLiteral("Documentos/viejo.pdf"),
                                          QStringLiteral("Imágenes/JPG/foto.jpg")}));
        QVERIFY(engine.undo(outcome.operationId, {}).ok());
        QCOMPARE(tree(root), before);
        // Las carpetas creadas se quitan; la que ya existía, no.
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/Imágenes")));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/Documentos/PDF")));
        QVERIFY(QFileInfo(root + QStringLiteral("/Documentos")).isDir());
    }

    void rulesRoundTripAsJson()
    {
        const QList<Rule> rules = defaultRules();
        const QList<Rule> back = rulesFromJson(rulesToJson(rules));
        QCOMPARE(back.size(), rules.size());
        QCOMPARE(back.first().folder, rules.first().folder);
        QCOMPARE(back.at(7).extensions, rules.at(7).extensions);
        QVERIFY(presets().size() >= 5);
    }
};

QTEST_GUILESS_MAIN(TestOrganize)

#include "test_organize.moc"

// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/rename/Rename.h"

#include "eu/journal/Journal.h"
#include "eu/ops/Engine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace eu;
using namespace eu::rename;

namespace {

Rule caseRule(Rule::CaseMode mode)
{
    Rule rule;
    rule.kind = Rule::Kind::Case;
    rule.caseMode = mode;
    return rule;
}

QString renamed(const QString &name, const QList<Rule> &rules, ExtensionMode extension = ExtensionMode::Keep,
                int index = 0, const Metadata &metadata = {}, bool directory = false)
{
    Item item;
    item.path = QStringLiteral("/tmp/") + name;
    item.isDirectory = directory;
    item.modifiedMs = QDateTime(QDate(2024, 3, 12), QTime(16, 57, 40)).toMSecsSinceEpoch();
    Options options;
    options.rules = rules;
    options.extension = extension;
    return proposeName(item, index, options, metadata);
}

void touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QStringList tree(const QString &root)
{
    QStringList result;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        result.append(QDir(root).relativeFilePath(it.next()));
    }
    result.sort();
    return result;
}

Item itemOf(const QString &path)
{
    Item item;
    item.path = path;
    item.isDirectory = QFileInfo(path).isDir();
    return item;
}

QStringList statuses(const QList<Proposal> &proposals)
{
    QStringList out;
    for (const Proposal &proposal : proposals) {
        static const char *names[] = {"igual", "ok", "conflicto", "invalido", "protegido"};
        out.append(QString::fromLatin1(names[int(proposal.status)]));
    }
    return out;
}

} // namespace

class TestRename : public QObject
{
    Q_OBJECT

private slots:
    void caseRules()
    {
        QCOMPARE(renamed(QStringLiteral("el niño y la NIÑA.JPG"), {caseRule(Rule::CaseMode::Upper)}),
                 QStringLiteral("EL NIÑO Y LA NIÑA.JPG"));
        QCOMPARE(renamed(QStringLiteral("el niño y la NIÑA.JPG"), {caseRule(Rule::CaseMode::Lower)}),
                 QStringLiteral("el niño y la niña.JPG"));
        QCOMPARE(renamed(QStringLiteral("el niño y la NIÑA.JPG"), {caseRule(Rule::CaseMode::Title)}),
                 QStringLiteral("El Niño Y La Niña.JPG"));
        QCOMPARE(renamed(QStringLiteral("EL NIÑO y la niña.JPG"), {caseRule(Rule::CaseMode::Sentence)}),
                 QStringLiteral("El niño y la niña.JPG"));
        // La extensión, según el modo.
        QCOMPARE(renamed(QStringLiteral("Foto.JPG"), {}, ExtensionMode::Lower), QStringLiteral("Foto.jpg"));
        QCOMPARE(renamed(QStringLiteral("Foto.jpg"), {}, ExtensionMode::Upper), QStringLiteral("Foto.JPG"));
        QCOMPARE(renamed(QStringLiteral("Foto.JPG"), {caseRule(Rule::CaseMode::Lower)}, ExtensionMode::Include),
                 QStringLiteral("foto.jpg"));
        // Una carpeta no tiene extensión; ".bashrc" tampoco.
        QCOMPARE(renamed(QStringLiteral("Viaje.2024"), {caseRule(Rule::CaseMode::Upper)}, ExtensionMode::Keep, 0, {}, true),
                 QStringLiteral("VIAJE.2024"));
        QCOMPARE(renamed(QStringLiteral(".bashrc"), {caseRule(Rule::CaseMode::Upper)}), QStringLiteral(".BASHRC"));
    }

    void textRules()
    {
        Rule replace;
        replace.kind = Rule::Kind::Replace;
        replace.find = QStringLiteral("img");
        replace.with = QStringLiteral("foto");
        QCOMPARE(renamed(QStringLiteral("IMG_001 img.jpg"), {replace}), QStringLiteral("foto_001 foto.jpg"));
        replace.caseSensitive = true;
        QCOMPARE(renamed(QStringLiteral("IMG_001 img.jpg"), {replace}), QStringLiteral("IMG_001 foto.jpg"));

        Rule remove;
        remove.kind = Rule::Kind::Remove;
        remove.characters = QStringLiteral("_-");
        QCOMPARE(renamed(QStringLiteral("a_b-c.txt"), {remove}), QStringLiteral("abc.txt"));
        remove.removeMode = Rule::RemoveMode::First;
        remove.count = 4;
        QCOMPARE(renamed(QStringLiteral("IMG_1234.jpg"), {remove}), QStringLiteral("1234.jpg"));
        remove.removeMode = Rule::RemoveMode::Last;
        remove.count = 3;
        QCOMPARE(renamed(QStringLiteral("informe v2.pdf"), {remove}), QStringLiteral("informe.pdf"));
        remove.removeMode = Rule::RemoveMode::Range;
        remove.from = 3;
        remove.count = 1;
        QCOMPARE(renamed(QStringLiteral("abc_def.txt"), {remove}), QStringLiteral("abcdef.txt"));

        Rule insert;
        insert.kind = Rule::Kind::Insert;
        insert.with = QStringLiteral("{fecha:yyyy-MM-dd} ");
        QCOMPARE(renamed(QStringLiteral("playa.jpg"), {insert}), QStringLiteral("2024-03-12 playa.jpg"));
        insert.position = Rule::Position::End;
        insert.with = QStringLiteral(" (copia)");
        QCOMPARE(renamed(QStringLiteral("playa.jpg"), {insert}), QStringLiteral("playa (copia).jpg"));
        insert.position = Rule::Position::At;
        insert.from = 2;
        insert.with = QStringLiteral("-");
        QCOMPARE(renamed(QStringLiteral("ab12.txt"), {insert}), QStringLiteral("ab-12.txt"));

        Rule regex;
        regex.kind = Rule::Kind::Regex;
        regex.find = QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})_(.*)$");
        regex.with = QStringLiteral("$1-$2-$3 \\4");
        QCOMPARE(renamed(QStringLiteral("20240312_cumple.mp4"), {regex}), QStringLiteral("2024-03-12 cumple.mp4"));

        Rule counter;
        counter.kind = Rule::Kind::Counter;
        counter.start = 5;
        counter.step = 2;
        counter.count = 3;
        counter.position = Rule::Position::End;
        counter.separator = QStringLiteral("_");
        QCOMPARE(renamed(QStringLiteral("viaje.jpg"), {counter}, ExtensionMode::Keep, 2), QStringLiteral("viaje_009.jpg"));

        // En cadena: cada regla sobre lo que dejó la anterior.
        QCOMPARE(renamed(QStringLiteral("IMG_001.JPG"), {replace, caseRule(Rule::CaseMode::Title)}, ExtensionMode::Lower),
                 QStringLiteral("Img_001.jpg"));
    }

    void templatesWithMetadata()
    {
        Rule rule;
        rule.kind = Rule::Kind::Template;
        rule.with = QStringLiteral("{fecha_foto:yyyy-MM-dd HH.mm} {camara} {contador:3}");
        Metadata photo{{QStringLiteral("fecha_foto"), QStringLiteral("2023-08-01 09:15:00")},
                       {QStringLiteral("camara"), QStringLiteral("Canon EOS 80D")}};
        QCOMPARE(renamed(QStringLiteral("IMG_9.CR2"), {rule}, ExtensionMode::Lower, 6, photo),
                 QStringLiteral("2023-08-01 09.15 Canon EOS 80D 007.cr2"));
        // Sin fecha de toma, la de modificación.
        QCOMPARE(renamed(QStringLiteral("x.jpg"), {rule}, ExtensionMode::Keep, 0, {}),
                 QStringLiteral("2024-03-12 16.57  001.jpg"));
        rule.with = QStringLiteral("{artista} - {album} - {pista:2} {titulo}");
        Metadata song{{QStringLiteral("artista"), QStringLiteral("Soda Stereo")}, {QStringLiteral("album"), QStringLiteral("Signos")},
                      {QStringLiteral("pista"), QStringLiteral("3/12")}, {QStringLiteral("titulo"), QStringLiteral("Prófugos")}};
        QCOMPARE(renamed(QStringLiteral("track03.mp3"), {rule}, ExtensionMode::Keep, 0, song),
                 QStringLiteral("Soda Stereo - Signos - 03 Prófugos.mp3"));
        rule.with = QStringLiteral("{nombre} {desconocido} {carpeta}");
        QCOMPARE(renamed(QStringLiteral("a.txt"), {rule}), QStringLiteral("a {desconocido} tmp.txt"));
        QVERIFY(templateTokens().size() >= 10);
    }

    void planFindsEveryProblem()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/a.txt"));
        touch(root + QStringLiteral("/b.txt"));
        touch(root + QStringLiteral("/c.txt"));
        touch(root + QStringLiteral("/ocupado.txt"));
        touch(root + QStringLiteral("/Mayus.txt"));

        // a↔b en ciclo: no es conflicto. c -> "ocupado" (existe y no está en la lista): conflicto.
        Rule swap;
        swap.kind = Rule::Kind::Regex;
        swap.find = QStringLiteral("^a$|^b$|^c$");
        // Plantilla por elemento: se simula con reglas distintas según el índice.
        Options options;
        const auto proposalsFor = [&](const QList<QPair<QString, QString>> &wanted) {
            QList<Item> items;
            for (const auto &pair : wanted) {
                items.append(itemOf(root + QLatin1Char('/') + pair.first));
            }
            // Una regla de plantilla que da el nombre pedido para cada uno.
            QList<Proposal> result;
            PlanOptions planOptions;
            planOptions.metadataFor = [&](const Item &item) {
                for (const auto &pair : wanted) {
                    if (item.path.endsWith(QLatin1Char('/') + pair.first)) {
                        return Metadata{{QStringLiteral("titulo"), pair.second}};
                    }
                }
                return Metadata();
            };
            Rule rule;
            rule.kind = Rule::Kind::Template;
            rule.with = QStringLiteral("{titulo}");
            Options byName;
            byName.rules = {rule};
            byName.extension = ExtensionMode::Include;
            return plan(items, byName, planOptions);
        };
        QList<Proposal> proposals = proposalsFor({{QStringLiteral("a.txt"), QStringLiteral("b.txt")},
                                                  {QStringLiteral("b.txt"), QStringLiteral("a.txt")},
                                                  {QStringLiteral("c.txt"), QStringLiteral("ocupado.txt")}});
        QCOMPARE(statuses(proposals), (QStringList{QStringLiteral("ok"), QStringLiteral("ok"), QStringLiteral("conflicto")}));
        QVERIFY(proposals[2].problem.contains(QStringLiteral("Ya existe")));

        // Dos que acabarían igual, y uno que solo cambia mayúsculas de algo que existe.
        proposals = proposalsFor({{QStringLiteral("a.txt"), QStringLiteral("nuevo.txt")},
                                  {QStringLiteral("b.txt"), QStringLiteral("NUEVO.txt")},
                                  {QStringLiteral("c.txt"), QStringLiteral("mayus.txt")}});
        QCOMPARE(statuses(proposals), (QStringList{QStringLiteral("conflicto"), QStringLiteral("conflicto"),
                                                   QStringLiteral("conflicto")}));
        QVERIFY(proposals[2].problem.contains(QStringLiteral("mayúsculas")));

        // Un conflicto deja ocupado su nombre: quien lo quería, también choca.
        proposals = proposalsFor({{QStringLiteral("a.txt"), QStringLiteral("ocupado.txt")},
                                  {QStringLiteral("b.txt"), QStringLiteral("a.txt")}});
        QCOMPARE(statuses(proposals), (QStringList{QStringLiteral("conflicto"), QStringLiteral("conflicto")}));

        // Cambiar solo mayúsculas de sí mismo, sin cambios, y no válidos.
        proposals = proposalsFor({{QStringLiteral("a.txt"), QStringLiteral("A.txt")},
                                  {QStringLiteral("b.txt"), QStringLiteral("b.txt")},
                                  {QStringLiteral("c.txt"), QStringLiteral("x/y.txt")}});
        QCOMPARE(statuses(proposals), (QStringList{QStringLiteral("ok"), QStringLiteral("igual"), QStringLiteral("invalido")}));

        // Con las reglas de Windows (un disco NTFS), ":" no vale.
        Rule colon;
        colon.kind = Rule::Kind::Insert;
        colon.with = QStringLiteral("12:30 ");
        Options withColon;
        withColon.rules = {colon};
        PlanOptions windows;
        windows.rulesFor = [](const QString &) { return core::NameRules::Windows; };
        QCOMPARE(statuses(plan({itemOf(root + QStringLiteral("/a.txt"))}, withColon, windows)),
                 QStringList{QStringLiteral("invalido")});
        QCOMPARE(statuses(plan({itemOf(root + QStringLiteral("/a.txt"))}, withColon)), QStringList{QStringLiteral("ok")});

        // Una expresión regular mal escrita.
        Rule broken;
        broken.kind = Rule::Kind::Regex;
        broken.find = QStringLiteral("(sin cerrar");
        Options bad;
        bad.rules = {broken};
        const QList<Proposal> invalid = plan({itemOf(root + QStringLiteral("/a.txt"))}, bad);
        QCOMPARE(statuses(invalid), QStringList{QStringLiteral("invalido")});
        QVERIFY(invalid.first().problem.contains(QStringLiteral("regular")));

#ifdef Q_OS_UNIX
        // Carpeta sin permiso de escritura.
        touch(root + QStringLiteral("/cerrada/z.txt"));
        QFile::setPermissions(root + QStringLiteral("/cerrada"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (!QFileInfo(root + QStringLiteral("/cerrada")).isWritable()) {
            Options upper;
            upper.rules = {caseRule(Rule::CaseMode::Upper)};
            QCOMPARE(statuses(plan({itemOf(root + QStringLiteral("/cerrada/z.txt"))}, upper)),
                     QStringList{QStringLiteral("protegido")});
        }
        QFile::setPermissions(root + QStringLiteral("/cerrada"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    }

    void engineRenamesCyclesCaseAndFoldersAndUndoes()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        touch(root + QStringLiteral("/a.txt"), "A");
        touch(root + QStringLiteral("/b.txt"), "B");
        touch(root + QStringLiteral("/foto.jpg"), "F");
        touch(root + QStringLiteral("/viaje/dentro.txt"), "D");
        const QStringList before = tree(root);
        auto journal = journal::Journal::open(data.filePath(QStringLiteral("h.db"))).value();
        ops::Engine engine(journal);
        const ops::Outcome outcome = engine.renameMany(
            {{root + QStringLiteral("/a.txt"), QStringLiteral("b.txt")},
             {root + QStringLiteral("/b.txt"), QStringLiteral("a.txt")},
             {root + QStringLiteral("/foto.jpg"), QStringLiteral("FOTO.jpg")},
             {root + QStringLiteral("/viaje"), QStringLiteral("Viaje 2024")},
             {root + QStringLiteral("/viaje/dentro.txt"), QStringLiteral("Dentro.txt")}},
            {});
        QVERIFY2(outcome.ok(), qPrintable(outcome.errors.isEmpty() ? QString() : outcome.errors.first().message));
        QCOMPARE(tree(root), (QStringList{QStringLiteral("FOTO.jpg"), QStringLiteral("Viaje 2024"),
                                          QStringLiteral("Viaje 2024/Dentro.txt"), QStringLiteral("a.txt"),
                                          QStringLiteral("b.txt")}));
        QFile a(root + QStringLiteral("/a.txt"));
        QVERIFY(a.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), QByteArray("B"));   // se intercambiaron
        a.close();
        QCOMPARE(journal->recentOperations(5).value().first().kind, journal::OperationKind::Rename);

        // Deshacer devuelve todo, en bloque.
        const ops::Outcome undone = engine.undo(outcome.operationId, {});
        QVERIFY2(undone.ok(), qPrintable(undone.errors.isEmpty() ? QString() : undone.errors.first().message));
        QCOMPARE(tree(root), before);
        QFile again(root + QStringLiteral("/a.txt"));
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), QByteArray("A"));

        // Si un nombre final aparece ocupado entre la vista previa y el renombrado,
        // ese elemento vuelve a su nombre (nunca se sobrescribe).
        touch(root + QStringLiteral("/c.txt"), "C");
        const ops::Outcome blocked = engine.renameMany({{root + QStringLiteral("/a.txt"), QStringLiteral("c.txt")}}, {});
        QCOMPARE(blocked.errors.size(), 1);
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/a.txt")));
        QFile c(root + QStringLiteral("/c.txt"));
        QVERIFY(c.open(QIODevice::ReadOnly));
        QCOMPARE(c.readAll(), QByteArray("C"));
    }
};

QTEST_GUILESS_MAIN(TestRename)

#include "test_rename.moc"
